/* SPDX-License-Identifier: MIT
 *
 * Scripted foreign nodes for running the conformance cases (spec/cases) with every
 * node behind the ABI. Each native implementation ships this API as a shared library;
 * a host's conformance driver builds its graph from these nodes instead of native ones.
 *
 * A session groups the nodes of one case: signals (signal/wait_for) and the start/end
 * events used by "order" and "stopped" checks are per session.
 */
#ifndef JABBA_TESTKIT_H
#define JABBA_TESTKIT_H

#include "../jabba_ffi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jabba_testkit_session jabba_testkit_session;

jabba_testkit_session* jabba_testkit_session_new(void);
/* Nodes created from the session may outlive it. */
void jabba_testkit_session_free(jabba_testkit_session* session);

/* A new node instance (its taker releases it). `spec` is JSON:
 *   {"name": "...", "deps": ["a", "request", ...], "behavior": {...}}
 * Inputs and output are JSON (null is the JSON text `null`); behaviors as in
 * spec/case.schema.json. */
const jabba_node* jabba_testkit_node(jabba_testkit_session* session, const char* spec, size_t spec_len);

/* The session's events as a JSON array of strings ("start:a", "end:a", ...). Writes at
 * most `cap` bytes into `out` and returns the full length. */
size_t jabba_testkit_events(jabba_testkit_session* session, char* out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* JABBA_TESTKIT_H */
