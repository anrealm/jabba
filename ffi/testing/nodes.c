/* SPDX-License-Identifier: MIT
 * Adversarial nodes for testing an importer. Each checks how the host treats it. */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "jabba_ffi_testing.h"

static atomic_long n_starts, n_ends, n_cancels, n_inst_made, n_inst_released, n_values_made, n_values_released,
    n_violations;
static pthread_mutex_t violation_mutex = PTHREAD_MUTEX_INITIALIZER;
static char first_violation[256];

static void violation(const char* what) {
    atomic_fetch_add(&n_violations, 1);
    pthread_mutex_lock(&violation_mutex);
    if (!first_violation[0]) snprintf(first_violation, sizeof first_violation, "%s", what);
    pthread_mutex_unlock(&violation_mutex);
}

void jt_counters_get(jt_counters* out) {
    out->starts = atomic_load(&n_starts);
    out->ends = atomic_load(&n_ends);
    out->cancels = atomic_load(&n_cancels);
    out->instances_made = atomic_load(&n_inst_made);
    out->instances_released = atomic_load(&n_inst_released);
    out->values_made = atomic_load(&n_values_made);
    out->values_released = atomic_load(&n_values_released);
    out->violations = atomic_load(&n_violations);
}

static void quarantine_free_all(void);

void jt_counters_reset(void) {
    quarantine_free_all();
    atomic_store(&n_starts, 0);
    atomic_store(&n_ends, 0);
    atomic_store(&n_cancels, 0);
    atomic_store(&n_inst_made, 0);
    atomic_store(&n_inst_released, 0);
    atomic_store(&n_values_made, 0);
    atomic_store(&n_values_released, 0);
    atomic_store(&n_violations, 0);
    pthread_mutex_lock(&violation_mutex);
    first_violation[0] = 0;
    pthread_mutex_unlock(&violation_mutex);
}

const char* jt_first_violation(void) {
    static char copy[256];
    pthread_mutex_lock(&violation_mutex);
    memcpy(copy, first_violation, sizeof copy);
    pthread_mutex_unlock(&violation_mutex);
    return copy;
}

static void sleep_us(long us) {
    struct timespec ts = {us / 1000000, (us % 1000000) * 1000};
    nanosleep(&ts, NULL);
}

/* ---- owned values ----------------------------------------------------------------- */

static void release_counted(void* owner) {
    free(owner);
    atomic_fetch_add(&n_values_released, 1);
}

static jabba_value owned_json(const char* text) {
    size_t len = strlen(text);
    char* copy = malloc(len);
    memcpy(copy, text, len);
    atomic_fetch_add(&n_values_made, 1);
    jabba_value v;
    memset(&v, 0, sizeof v);
    v.kind = JABBA_JSON;
    v.as.buf.data = copy;
    v.as.buf.len = len;
    v.release = release_counted;
    v.owner = copy;
    return v;
}

static jabba_value owned_array(uint32_t kind, const void* data, size_t len, size_t elem) {
    size_t bytes = len * elem;
    void* copy = malloc(bytes ? bytes : 1);
    if (len) memcpy(copy, data, len * elem);
    atomic_fetch_add(&n_values_made, 1);
    jabba_value v;
    memset(&v, 0, sizeof v);
    v.kind = kind;
    v.as.buf.data = copy;
    v.as.buf.len = len;
    v.release = release_counted;
    v.owner = copy;
    return v;
}

static void complete_ok(jabba_call* call, jabba_value value) {
    jabba_result r;
    memset(&r, 0, sizeof r);
    r.size = sizeof r;
    r.status = JABBA_OK;
    r.value = value;
    call->complete(call, &r);
}

static void complete_status(jabba_call* call, uint32_t status, const char* error) {
    jabba_result r;
    memset(&r, 0, sizeof r);
    r.size = sizeof r;
    r.status = status;
    r.error = error;
    r.error_len = error ? strlen(error) : 0;
    call->complete(call, &r);
}

/* ---- per-call bookkeeping: what the host did, in which order ----------------------- */

typedef struct node_call {
    atomic_int start_returned, completed, cancelled, ended;
    atomic_int cancel_flag;
    jabba_call* call;
    struct node_instance* inst;
    char* text;             /* jt_node_borrowed_str: freed at end() */
    struct node_call* next; /* quarantine */
} node_call;

/* Ended calls are kept (not freed) until jt_counters_reset, so that a host calling
 * into a call after end() is reported instead of reading freed memory. */
static pthread_mutex_t quarantine_mutex = PTHREAD_MUTEX_INITIALIZER;
static node_call* quarantine;

static void quarantine_call(node_call* c) {
    pthread_mutex_lock(&quarantine_mutex);
    c->next = quarantine;
    quarantine = c;
    pthread_mutex_unlock(&quarantine_mutex);
}

static void quarantine_free_all(void) {
    pthread_mutex_lock(&quarantine_mutex);
    while (quarantine) {
        node_call* next = quarantine->next;
        free(quarantine);
        quarantine = next;
    }
    pthread_mutex_unlock(&quarantine_mutex);
}

typedef struct node_instance {
    jabba_node node;
    uint32_t inputs[1];
    int behaviour;
    atomic_int calls_alive;
    /* jt_node_locked: held across complete(), taken by cancel(). Per instance, since
     * end() may free a call from inside complete(). */
    pthread_mutex_t lock;
    /* The taker's reference plus one per thread of ours that outlives complete(). */
    atomic_int refs;
} node_instance;

static void inst_unref(node_instance* inst) {
    if (atomic_fetch_sub(&inst->refs, 1) == 1) {
        pthread_mutex_destroy(&inst->lock);
        free(inst);
    }
}

enum { B_INLINE, B_DELAY, B_CANCEL_INLINE, B_LOCKED, B_RACE, B_WRONG_KIND, B_FAILING, B_SCALE, B_BORROWED, B_FAILED_VALUE };

static node_call* new_call(node_instance* inst, jabba_call* call) {
    node_call* c = calloc(1, sizeof *c);
    c->call = call;
    c->inst = inst;
    call->node_data = c;
    atomic_fetch_add(&inst->calls_alive, 1);
    return c;
}

/* Mark completion; returns 0 if the node already completed (its own bug, not the host's). */
static int mark_completed(node_call* c) { return atomic_exchange(&c->completed, 1) == 0; }

static void* delay_thread(void* arg) {
    node_call* c = arg;
    for (int i = 0; i < 20 && !atomic_load(&c->cancel_flag); ++i) sleep_us(1000);
    if (!mark_completed(c)) return NULL;
    if (atomic_load(&c->cancel_flag)) complete_status(c->call, JABBA_CANCELLED, NULL);
    else complete_ok(c->call, owned_json("\"delayed\""));
    return NULL;
}

static void* locked_thread(void* arg) {
    node_call* c = arg;
    node_instance* inst = c->inst;
    sleep_us(2000);
    /* Held across complete() while cancel() takes it: a host that resumes its waiting
     * code inside complete(), and then waits for its own cancel(), deadlocks here. */
    pthread_mutex_lock(&inst->lock);
    if (mark_completed(c)) complete_ok(c->call, owned_json("\"locked\""));
    pthread_mutex_unlock(&inst->lock); /* c may be gone; inst is ours until inst_unref */
    inst_unref(inst);
    return NULL;
}

static void* race_thread(void* arg) {
    node_call* c = arg;
    sleep_us(rand() % 100);
    if (mark_completed(c)) complete_ok(c->call, owned_json("\"raced\""));
    return NULL;
}

static void spawn(void* (*fn)(void*), node_call* c) {
    pthread_t t;
    pthread_create(&t, NULL, fn, c);
    pthread_detach(t);
}

static void n_start(void* self, jabba_call* call) {
    node_instance* inst = self;
    atomic_fetch_add(&n_starts, 1);
    node_call* c = new_call(inst, call);
    switch (inst->behaviour) {
        case B_INLINE:
            if (mark_completed(c)) complete_ok(call, owned_json("\"inline\""));
            break;
        case B_DELAY: spawn(delay_thread, c); break;
        case B_CANCEL_INLINE: break;
        case B_LOCKED:
            atomic_fetch_add(&inst->refs, 1);
            spawn(locked_thread, c);
            break;
        case B_RACE: spawn(race_thread, c); break;
        case B_WRONG_KIND: {
            float f[3] = {1, 2, 3};
            if (mark_completed(c)) complete_ok(call, owned_array(JABBA_F32S, f, 3, sizeof(float)));
            break;
        }
        case B_FAILING:
            if (mark_completed(c)) complete_status(call, JABBA_FAILED, "boom");
            break;
        case B_BORROWED: {
            c->text = malloc(9);
            memcpy(c->text, "borrowed", 9);
            jabba_value v;
            memset(&v, 0, sizeof v);
            v.kind = JABBA_STR;
            v.as.buf.data = c->text;
            v.as.buf.len = 8;
            if (mark_completed(c)) complete_ok(call, v); /* no release: valid during complete only */
            break;
        }
        case B_FAILED_VALUE: {
            jabba_result r;
            memset(&r, 0, sizeof r);
            r.size = sizeof r;
            r.status = JABBA_FAILED;
            r.value = owned_json("\"should not be here\"");
            r.error = "bad";
            r.error_len = 3;
            if (mark_completed(c)) call->complete(call, &r);
            break;
        }
        case B_SCALE: {
            const jabba_value* in = &call->inputs[0];
            if ((in->kind & JABBA_KIND_MASK) != JABBA_F64S) {
                if (mark_completed(c)) complete_status(call, JABBA_FAILED, "expected f64s");
                break;
            }
            size_t n = in->as.buf.len;
            const double* d = in->as.buf.data;
            double* out = malloc(n ? n * sizeof(double) : 1);
            for (size_t i = 0; i < n; ++i) out[i] = d[i] * 2;
            jabba_value v = owned_array(JABBA_F64S, out, n, sizeof(double));
            free(out);
            if (mark_completed(c)) complete_ok(call, v);
            break;
        }
    }
    /* After complete() the call is the host's: only our own state is touched below. */
    atomic_store(&c->start_returned, 1);
}

static void n_cancel(void* self, jabba_call* call) {
    (void)self;
    node_call* c = call->node_data;
    atomic_fetch_add(&n_cancels, 1);
    if (!atomic_load(&c->start_returned)) violation("cancel() before start() returned");
    if (atomic_exchange(&c->cancelled, 1)) violation("cancel() called twice");
    if (atomic_load(&c->ended)) violation("cancel() after end()");
    pthread_mutex_lock(&c->inst->lock);
    atomic_store(&c->cancel_flag, 1);
    pthread_mutex_unlock(&c->inst->lock);
    if (c->inst->behaviour == B_CANCEL_INLINE && mark_completed(c)) complete_status(call, JABBA_CANCELLED, NULL);
}

static void n_end(void* self, jabba_call* call) {
    node_instance* inst = self;
    node_call* c = call->node_data;
    atomic_fetch_add(&n_ends, 1);
    if (!atomic_load(&c->completed)) violation("end() before complete()");
    if (!atomic_load(&c->start_returned)) violation("end() before start() returned");
    if (atomic_exchange(&c->ended, 1)) violation("end() called twice");
    free(c->text);
    c->text = NULL;
    quarantine_call(c);
    atomic_fetch_sub(&inst->calls_alive, 1);
}

static void n_release(void* self) {
    node_instance* inst = self;
    if (atomic_load(&inst->calls_alive) != 0) violation("release() with calls not ended");
    atomic_fetch_add(&n_inst_released, 1);
    inst_unref(inst);
}

static const char* names[] = {"inline", "delay", "cancel_inline", "locked", "race", "wrong_kind", "failing", "scale", "borrowed_str", "failed_with_value"};

static const jabba_node* make(int behaviour, uint32_t output, size_t n_inputs, uint32_t input0, uint32_t abi) {
    node_instance* inst = calloc(1, sizeof *inst);
    pthread_mutex_init(&inst->lock, NULL);
    atomic_store(&inst->refs, 1);
    inst->behaviour = behaviour;
    inst->inputs[0] = input0;
    inst->node.abi = abi;
    inst->node.size = sizeof(jabba_node);
    inst->node.self = inst;
    inst->node.name = names[behaviour];
    inst->node.output_kind = output;
    inst->node.n_inputs = n_inputs;
    inst->node.input_kinds = inst->inputs;
    inst->node.start = n_start;
    inst->node.cancel = n_cancel;
    inst->node.end = n_end;
    inst->node.release = n_release;
    atomic_fetch_add(&n_inst_made, 1);
    return &inst->node;
}

const jabba_node* jt_node_inline(void) { return make(B_INLINE, JABBA_JSON, 0, 0, JABBA_FFI_ABI); }
const jabba_node* jt_node_delay(void) { return make(B_DELAY, JABBA_JSON, 0, 0, JABBA_FFI_ABI); }
const jabba_node* jt_node_cancel_inline(void) { return make(B_CANCEL_INLINE, JABBA_JSON, 0, 0, JABBA_FFI_ABI); }
const jabba_node* jt_node_locked(void) { return make(B_LOCKED, JABBA_JSON, 0, 0, JABBA_FFI_ABI); }
const jabba_node* jt_node_race(void) { return make(B_RACE, JABBA_JSON, 0, 0, JABBA_FFI_ABI); }
const jabba_node* jt_node_wrong_kind(void) { return make(B_WRONG_KIND, JABBA_F64S, 0, 0, JABBA_FFI_ABI); }
const jabba_node* jt_node_failing(void) { return make(B_FAILING, JABBA_JSON, 0, 0, JABBA_FFI_ABI); }
const jabba_node* jt_node_bad_abi(void) { return make(B_INLINE, JABBA_JSON, 0, 0, 2); }
const jabba_node* jt_node_f64_scale(void) { return make(B_SCALE, JABBA_F64S, 1, JABBA_F64S, JABBA_FFI_ABI); }
const jabba_node* jt_node_borrowed_str(void) { return make(B_BORROWED, JABBA_STR, 0, 0, JABBA_FFI_ABI); }
const jabba_node* jt_node_failed_with_value(void) { return make(B_FAILED_VALUE, JABBA_JSON, 0, 0, JABBA_FFI_ABI); }
