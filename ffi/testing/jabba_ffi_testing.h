/* SPDX-License-Identifier: MIT
 *
 * A protocol test kit for the jabba foreign-node ABI, shared by every language:
 *  - adversarial C nodes, to test a language's importer (host side);
 *  - a C host that drives a node, to test a language's exporter (node side).
 * Both count protocol violations by the other side.
 */
#ifndef JABBA_FFI_TESTING_H
#define JABBA_FFI_TESTING_H

#include "../jabba_ffi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- adversarial nodes (factories) ------------------------------------------------ */

const jabba_node* jt_node_inline(void);        /* () -> json: completes inside start() */
const jabba_node* jt_node_delay(void);         /* () -> json: from a thread after 20 ms; honours cancel */
const jabba_node* jt_node_cancel_inline(void); /* () -> json: completes only when cancelled, inside cancel() */
const jabba_node* jt_node_locked(void);        /* () -> json: completes after ~2 ms while holding a lock that cancel() takes */
const jabba_node* jt_node_race(void);          /* () -> json: completes from a thread after 0..100 us */
const jabba_node* jt_node_wrong_kind(void);    /* () -> f64s declared, returns f32s */
const jabba_node* jt_node_failing(void);       /* () -> json: fails inside start() with "boom" */
const jabba_node* jt_node_bad_abi(void);       /* declares ABI version 2 */
const jabba_node* jt_node_f64_scale(void);     /* (f64s) -> f64s: doubles every element, owned output */
const jabba_node* jt_node_borrowed_str(void);  /* () -> str: completes inside start() with a buffer without
                                                  release, freed at end(): the host must copy it in complete() */
const jabba_node* jt_node_failed_with_value(void); /* () -> json: FAILED carrying a value to release (a bad node) */

typedef struct jt_counters {
    long starts, ends, cancels;
    long instances_made, instances_released;
    long values_made, values_released;
    long violations; /* protocol violations by the host, seen by the nodes */
} jt_counters;

void jt_counters_get(jt_counters* out);
void jt_counters_reset(void);
/* Text of the first violation seen (empty if none). */
const char* jt_first_violation(void);

/* ---- a host driving one call of a node (tests an exporter) ---------------------------- */

typedef enum jt_mode {
    JT_PLAIN = 0,       /* start, wait for complete, end */
    JT_CANCEL_SOON = 1, /* cancel right after start() returns */
    JT_CANCEL_RACE = 2  /* cancel from another thread at a random moment */
} jt_mode;

typedef struct jt_drive_result {
    int ok;              /* 1 if the node followed the protocol */
    char error[256];     /* why not */
    uint32_t status;     /* the status it completed with */
    char value_json[256];/* the value, rendered as JSON when it is small */
    long set_fields;     /* set_field calls seen */
} jt_drive_result;

/* Inputs are generated from the declared kinds: int 21, float 1.5, bool 1, str "abc",
 * json {"a":1}, bytes "xyz", f64s [1,2,3], f32s [1,2,3], i64s [1,2,3]. Takes ownership
 * of `node` and releases it. */
void jt_drive(const jabba_node* node, jt_mode mode, jt_drive_result* out);

#ifdef __cplusplus
}
#endif

#endif /* JABBA_FFI_TESTING_H */
