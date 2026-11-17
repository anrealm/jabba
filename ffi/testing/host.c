/* SPDX-License-Identifier: MIT
 * A host that drives one call of a node, to test an exporter against the protocol. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "jabba_ffi_testing.h"

typedef struct task {
    void (*fn)(void*);
    void* arg;
    struct task* next;
} task;

typedef struct drive {
    pthread_mutex_t m;
    pthread_cond_t cv;
    int completes;
    char violation[200]; /* the node broke the protocol */
    uint32_t status;
    jabba_value value;
    char error[200];
    long set_fields;
    /* the host's executor: one worker thread */
    task *head, *tail;
    int stopping;
    pthread_t worker;
    /* the call */
    jabba_host host;
    jabba_call call;
    jabba_value inputs[16];
    double f64[3];
    float f32[3];
    int64_t i64[3];
} drive;

static void* worker_main(void* arg) {
    drive* d = arg;
    pthread_mutex_lock(&d->m);
    for (;;) {
        while (!d->head && !d->stopping) pthread_cond_wait(&d->cv, &d->m);
        if (!d->head) break;
        task* t = d->head;
        d->head = t->next;
        if (!d->head) d->tail = NULL;
        pthread_mutex_unlock(&d->m);
        t->fn(t->arg);
        free(t);
        pthread_mutex_lock(&d->m);
    }
    pthread_mutex_unlock(&d->m);
    return NULL;
}

static void violate(drive* d, const char* what) { /* call with d->m held */
    if (!d->violation[0]) snprintf(d->violation, sizeof d->violation, "%s", what);
}

static void host_post(void* self, void (*fn)(void*), void* arg) {
    drive* d = self;
    task* t = calloc(1, sizeof *t);
    t->fn = fn;
    t->arg = arg;
    pthread_mutex_lock(&d->m);
    if (d->completes) violate(d, "post() after complete()");
    if (d->tail) d->tail->next = t;
    else d->head = t;
    d->tail = t;
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->m);
}

static void host_set_field(void* self, const char* key, size_t key_len, const jabba_value* value) {
    drive* d = self;
    (void)key;
    (void)key_len;
    (void)value;
    pthread_mutex_lock(&d->m);
    if (d->completes) violate(d, "set_field() after complete()");
    d->set_fields++;
    pthread_mutex_unlock(&d->m);
}

static void host_complete(jabba_call* call, const jabba_result* r) {
    drive* d = call->host_data;
    pthread_mutex_lock(&d->m);
    d->completes++;
    if (d->completes == 1) {
        d->status = r->status;
        if (r->status == JABBA_OK) d->value = r->value;
        else if (r->value.release) {
            violate(d, "non-OK result carries a value to release");
            r->value.release(r->value.owner);
        }
        if (r->status == JABBA_FAILED && r->error) {
            snprintf(d->error, sizeof d->error, "%.*s", (int)(r->error_len < 150 ? r->error_len : 150), r->error);
        }
    }
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->m);
}

static jabba_value input_for(drive* d, uint32_t declared) {
    jabba_value v;
    memset(&v, 0, sizeof v);
    v.kind = declared & JABBA_KIND_MASK;
    switch (v.kind) {
        case JABBA_BOOL: v.as.i = 1; break;
        case JABBA_INT: v.as.i = 21; break;
        case JABBA_FLOAT: v.as.f = 1.5; break;
        case JABBA_STR: v.as.buf.data = "abc"; v.as.buf.len = 3; break;
        case JABBA_BYTES: v.as.buf.data = "xyz"; v.as.buf.len = 3; break;
        case JABBA_JSON: v.as.buf.data = "{\"a\":1}"; v.as.buf.len = 7; break;
        case JABBA_F64S: v.as.buf.data = d->f64; v.as.buf.len = 3; break;
        case JABBA_F32S: v.as.buf.data = d->f32; v.as.buf.len = 3; break;
        case JABBA_I64S: v.as.buf.data = d->i64; v.as.buf.len = 3; break;
        default: break;
    }
    return v;
}

static void render(const jabba_value* v, char* out, size_t cap) {
    out[0] = 0;
    switch (v->kind & JABBA_KIND_MASK) {
        case JABBA_NULL: snprintf(out, cap, "null"); break;
        case JABBA_BOOL: snprintf(out, cap, v->as.i ? "true" : "false"); break;
        case JABBA_INT: snprintf(out, cap, "%lld", (long long)v->as.i); break;
        case JABBA_FLOAT: snprintf(out, cap, "%g", v->as.f); break;
        case JABBA_STR: snprintf(out, cap, "\"%.*s\"", (int)(v->as.buf.len < 200 ? v->as.buf.len : 200), (const char*)v->as.buf.data); break;
        case JABBA_JSON: snprintf(out, cap, "%.*s", (int)(v->as.buf.len < 250 ? v->as.buf.len : 250), (const char*)v->as.buf.data); break;
        case JABBA_F64S: {
            const double* x = v->as.buf.data;
            size_t n = v->as.buf.len, used = (size_t)snprintf(out, cap, "[");
            for (size_t i = 0; i < n && i < 8 && used < cap; ++i) used += (size_t)snprintf(out + used, cap - used, i ? ",%g" : "%g", x[i]);
            if (used < cap) snprintf(out + used, cap - used, "]");
            break;
        }
        default: snprintf(out, cap, "<kind %u>", v->kind); break;
    }
}

static int value_fits(uint32_t actual, uint32_t declared) {
    uint32_t base = actual & JABBA_KIND_MASK;
    if (base == JABBA_NULL) return (declared & JABBA_KIND_NULLABLE) || (declared & JABBA_KIND_MASK) == JABBA_NULL;
    return base == (declared & JABBA_KIND_MASK);
}

typedef struct canceller {
    const jabba_node* node;
    jabba_call* call;
} canceller;

static void* cancel_later(void* arg) {
    canceller* c = arg;
    struct timespec ts = {0, (rand() % 200) * 1000};
    nanosleep(&ts, NULL);
    c->node->cancel(c->node->self, c->call);
    return NULL;
}

static void fail(jt_drive_result* out, const char* why) {
    if (out->ok) {
        out->ok = 0;
        snprintf(out->error, sizeof out->error, "%s", why);
    }
}

void jt_drive(const jabba_node* node, jt_mode mode, jt_drive_result* out) {
    memset(out, 0, sizeof *out);
    out->ok = 1;
    if (node->abi != JABBA_FFI_ABI || node->size < sizeof(jabba_node)) {
        fail(out, "bad abi or size");
        return;
    }
    drive* d = calloc(1, sizeof *d);
    pthread_mutex_init(&d->m, NULL);
    pthread_cond_init(&d->cv, NULL);
    for (int i = 0; i < 3; ++i) {
        d->f64[i] = i + 1;
        d->f32[i] = (float)(i + 1);
        d->i64[i] = i + 1;
    }
    pthread_create(&d->worker, NULL, worker_main, d);
    d->host.size = sizeof d->host;
    d->host.self = d;
    d->host.post = host_post;
    d->host.set_field = host_set_field;
    d->host.request_id = "jt-request";
    d->host.request_id_len = 10;
    size_t n = node->n_inputs < 16 ? node->n_inputs : 16;
    for (size_t i = 0; i < n; ++i) d->inputs[i] = input_for(d, node->input_kinds[i]);
    d->call.size = sizeof d->call;
    d->call.inputs = d->inputs;
    d->call.n_inputs = n;
    d->call.host = &d->host;
    d->call.complete = host_complete;
    d->call.host_data = d;

    node->start(node->self, &d->call);
    pthread_t cancel_thread;
    canceller c = {node, &d->call};
    int racing = 0;
    if (mode == JT_CANCEL_SOON) node->cancel(node->self, &d->call);
    if (mode == JT_CANCEL_RACE) {
        pthread_create(&cancel_thread, NULL, cancel_later, &c);
        racing = 1;
    }

    /* wait for complete (5 s), then a little longer to catch a second one */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;
    pthread_mutex_lock(&d->m);
    int timed_out = 0;
    while (d->completes == 0 && !timed_out) timed_out = pthread_cond_timedwait(&d->cv, &d->m, &deadline) == ETIMEDOUT;
    pthread_mutex_unlock(&d->m);
    if (racing) pthread_join(cancel_thread, NULL);
    if (timed_out) {
        fail(out, "no complete() within 5 s");
        /* the call is still alive in the node: leak it rather than end() it */
        return;
    }
    struct timespec grace = {0, 20 * 1000 * 1000};
    nanosleep(&grace, NULL);

    pthread_mutex_lock(&d->m);
    int completes = d->completes;
    out->status = d->status;
    jabba_value value = d->value;
    out->set_fields = d->set_fields;
    char error[200];
    snprintf(error, sizeof error, "%s", d->error);
    char violation_seen[200];
    snprintf(violation_seen, sizeof violation_seen, "%s", d->violation);
    pthread_mutex_unlock(&d->m);

    if (violation_seen[0]) fail(out, violation_seen);
    if (completes != 1) fail(out, "complete() called more than once");
    if (error[0] && out->status != JABBA_FAILED) fail(out, error);
    if (out->status > JABBA_CANCELLED) fail(out, "unknown status");
    if (out->status == JABBA_OK) {
        if (!value_fits(value.kind, node->output_kind)) fail(out, "result kind differs from the declared output kind");
        render(&value, out->value_json, sizeof out->value_json);
        if (value.release) value.release(value.owner);
    } else if (out->status == JABBA_FAILED) {
        snprintf(out->value_json, sizeof out->value_json, "%s", error);
    }

    node->end(node->self, &d->call);
    node->release(node->self);

    /* let posted work finish, then stop the executor; late post/set_field are violations */
    nanosleep(&grace, NULL);
    pthread_mutex_lock(&d->m);
    if (d->violation[0]) fail(out, d->violation);
    pthread_mutex_unlock(&d->m);
    pthread_mutex_lock(&d->m);
    d->stopping = 1;
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->m);
    pthread_join(d->worker, NULL);
    pthread_cond_destroy(&d->cv);
    pthread_mutex_destroy(&d->m);
    free(d);
}
