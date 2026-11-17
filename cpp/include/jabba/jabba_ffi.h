/* SPDX-License-Identifier: MIT
 *
 * jabba foreign-node ABI: a node implemented in one language, used in a graph of
 * another. Types only, no code. The normative rules are in spec/ffi.md; the short
 * version is in the comments below.
 *
 * Extensibility: every struct starts with `size` (sizeof as built by its author);
 * readers check it before reading fields added later. Adding a field or a kind
 * does not change JABBA_FFI_ABI; only an incompatible change does.
 */
#ifndef JABBA_FFI_H
#define JABBA_FFI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JABBA_FFI_ABI 1u

/* ---- values ------------------------------------------------------------------ */

typedef enum jabba_kind {
    JABBA_NULL = 0,
    JABBA_BOOL = 1,  /* as.i: 0 or 1 */
    JABBA_INT = 2,   /* as.i */
    JABBA_FLOAT = 3, /* as.f */
    JABBA_STR = 4,   /* as.buf: UTF-8, len bytes, not NUL-terminated */
    JABBA_BYTES = 5, /* as.buf */
    JABBA_JSON = 6,  /* as.buf: UTF-8 JSON text (RFC 8259: no NaN/Infinity) */
    JABBA_F64S = 7,  /* as.buf: len doubles */
    JABBA_F32S = 8,  /* as.buf: len floats */
    JABBA_I64S = 9   /* as.buf: len int64_t */
} jabba_kind;

/* OR'ed into a declared kind: JABBA_NULL is accepted as well. */
#define JABBA_KIND_NULLABLE 0x100u
#define JABBA_KIND_MASK 0xFFu

/* Arrays: data aligned to the element size, C-contiguous, native-endian; data may be
 * NULL only when len == 0. */
typedef struct jabba_buf {
    const void* data;
    size_t len;
} jabba_buf;

typedef struct jabba_value {
    uint32_t kind;
    union {
        int64_t i;
        double f;
        jabba_buf buf;
    } as;
    /* Called once by whoever owns the value when done with it. May be called from any
     * thread, at any time, independently of calls and nodes. NULL: nothing to free, and a
     * buffer is valid only during the call that hands it over (the receiver copies it). */
    void (*release)(void* owner);
    void* owner;
} jabba_value;

/* ---- one call ------------------------------------------------------------------ */

typedef struct jabba_host {
    uint32_t size;
    void* self;
    /* Run fn(arg) later on the host's executor. Thread-safe, never inline; every fn is
     * called exactly once, even after the run has returned. May be NULL: the node then
     * uses its own executor. */
    void (*post)(void* self, void (*fn)(void* arg), void* arg);
    /* Write a field into the run's record. `value` is borrowed (the host copies it
     * before returning). Ignored once the run has returned. */
    void (*set_field)(void* self, const char* key, size_t key_len, const jabba_value* value);
    const char* request_id;
    size_t request_id_len;
} jabba_host;

typedef enum jabba_status { JABBA_OK = 0, JABBA_FAILED = 1, JABBA_CANCELLED = 2 } jabba_status;

typedef struct jabba_result {
    uint32_t size;
    uint32_t status;   /* jabba_status; an unknown status counts as FAILED */
    jabba_value value; /* JABBA_OK only; otherwise value.release must be NULL */
    const char* error; /* JABBA_FAILED: UTF-8 message, borrowed during complete() */
    size_t error_len;
} jabba_result;

typedef struct jabba_call jabba_call;

/* Allocated by the host; valid from start() until end() returns. */
struct jabba_call {
    uint32_t size;
    const jabba_value* inputs; /* borrowed until the node calls complete(), in dependency order */
    size_t n_inputs;
    const jabba_host* host;    /* valid until end(), independently of the run */
    /* Exactly once per call, from any thread, also from inside start() or cancel().
     * The result is borrowed during the call; afterwards the node must not touch the
     * call, its inputs or host (no more post or set_field). The node should hold no lock
     * that its cancel() takes while calling it; hosts do not rely on that. */
    void (*complete)(jabba_call* call, const jabba_result* result);
    void* host_data; /* the host's */
    void* node_data; /* the node's; may be set in start() */
};

/* ---- a node instance ---------------------------------------------------------------- */

typedef struct jabba_node {
    uint32_t abi;  /* JABBA_FFI_ABI */
    uint32_t size; /* sizeof(jabba_node) as built by the node's author */
    void* self;
    const char* name;            /* NUL-terminated UTF-8, lives as long as the instance */
    uint32_t output_kind;        /* jabba_kind, optionally | JABBA_KIND_NULLABLE */
    size_t n_inputs;
    const uint32_t* input_kinds; /* n_inputs entries */
    /* Start one call. Must not block: work that waits or computes for long goes to an
     * executor. May call call->complete before returning. */
    void (*start)(void* self, jabba_call* call);
    /* Request cancellation. Called at most once, after start() returned and before
     * end(), never concurrently with end(); may race with complete(). complete() is
     * still called exactly once; cancel() after complete() is a no-op. */
    void (*cancel)(void* self, jabba_call* call);
    /* The host is done with the call: start() returned, complete() was received, and
     * cancel() will not follow. Called exactly once, from any thread, possibly from
     * inside complete(); it may wait for a set_field of the call still in progress. The
     * node frees its per-call state here. */
    void (*end)(void* self, jabba_call* call);
    /* The instance is no longer used: called once, after end() of every call. */
    void (*release)(void* self);
} jabba_node;

/* A node library exports factories: each call returns a new instance, which its
 * taker releases exactly once. */
typedef const jabba_node* (*jabba_node_factory)(void);

#ifdef __cplusplus
}
#endif

#endif /* JABBA_FFI_H */
