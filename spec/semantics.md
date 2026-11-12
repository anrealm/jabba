# jabba execution semantics

`spec_version: 1`

Normative for every jabba implementation. The conformance cases in [`cases/`](cases/) pin the observable behavior; what cannot be expressed as data (type checks, panic/exception policy, logging, API ergonomics) is covered by each implementation's own tests. Where an implementation does not yet conform, its driver marks the case `pending` for that language.

## 1. Graph model

- A **node** has a name (unique within a graph), **data dependencies** (their results arrive as inputs), **ordering dependencies** `after` (wait for completion, no data), an `optional` flag with a `default`, a per-node `timeout`, and optional access to the run's `request` and `ctx`. A node may list the same provider in both data dependencies and `after`.
- `default: null` is a real default, distinct from "no default declared".
- `request` and `ctx` are reserved and cannot be node names.
- Inputs are shared between consumers and must be treated as immutable. Mutating an input is undefined behavior.
- A graph may name one node as its `output`.

## 2. Compilation

A graph is assembled, then compiled once into an immutable graph; nothing is validated per request. Compile errors carry a **kind**:

| kind | when |
|---|---|
| `reserved_name` | a node is named `request` or `ctx` |
| `duplicate_name` | two nodes share a name |
| `unknown_dep` | a data dependency names no node |
| `unknown_after` | an `after` entry names no node |
| `unknown_output` | `output` names no node |
| `optional_without_default` | an optional node has data dependents but no default (`after`-only dependents need none) |
| `optional_output_without_default` | the `output` node is optional but has no default |
| `cycle` | dependencies (data or `after`, including a node on itself) form a cycle |
| `type_mismatch` | a provider's output type is incompatible with a consumer's input type; how strictly this is checked is language-specific |

The order in which checks run is not specified; each conformance case contains exactly one error.

## 3. Execution

- A node starts as soon as all its data dependencies and `after` dependencies have completed. There are no stages or barriers.
- **Ticks.** The executor processes events in ticks. Within one tick it records, in this order: (1) nodes that have completed, (2) expired per-node timeouts, (3) the graph deadline. A node whose completion is recorded before the deadline is processed keeps its status. Races between a completion and the deadline are not part of conformance.
- **Success.** The node's status is `ok`; its value goes to its data dependents.
- **Failure.** A node that raises/returns an error gets status `failed`, error kind `failed`.
  - If the node is optional, its data dependents receive its `default`, its `after` dependents are unblocked, and the run continues.
  - Otherwise the run **fails fast**: it ends with status `failed` and a `NodeFailed` error naming the node. If several non-optional nodes fail in the same tick, the error names one of them; all are recorded `failed`.
- **Per-node timeout.** When a node exceeds its `timeout`, it is cancelled and the run does not wait for it at all. Status `timeout`, error kind `timeout`. Then the node is treated like a failure: optional → `default`, otherwise fail-fast.
  - A timeout error raised by the node's own code (for example, its client's timeout) is a failure (`failed`), not a jabba timeout.
- **Graph deadline.** When the run's deadline passes, every node still running is cancelled and recorded as status `timeout`, error kind `deadline`. The run ends with status `timeout` and a `GraphTimeout` error.
- **Fail-fast cancellation.** When a run fails fast, every node still running is cancelled and recorded as status `skipped`, error kind `cancelled`. Waiting for cancelled nodes never turns a failure into a timeout.
- **Outer cancellation.** When the caller cancels the run, every node still running is cancelled, the cancellation propagates to the caller, and **no record is produced**.
- Nodes that never started are recorded as `skipped` with no error kind.
- **Synchronous nodes** run to completion; they cannot be interrupted. Timeouts and cancellation take effect only at suspension points of asynchronous nodes. A synchronous node that completes is recorded by the tick rule.

## 4. Cancellation

Cancellation is cooperative in every language: jabba requests it, and the node's code decides when it actually stops. See [`docs/cancellation.md`](../docs/cancellation.md) for what this means in each language.

- Statuses of cancelled nodes are set **when jabba decides to cancel**, not when the node reacts. A node that ignores cancellation cannot change its own record.
- After a graph deadline or fail-fast, the run waits at most `CANCEL_GRACE` = **50 ms** for cancelled nodes, then returns anyway. After a per-node timeout it does not wait at all. Nodes that are still running are left to finish in the background; their results are discarded.
- Consequently a run returns within `deadline + CANCEL_GRACE` (plus scheduling noise), not strictly within `deadline`.

## 5. Request record

Every run that ends `ok`, `failed` or `timeout` produces exactly one record (schema: [`record.schema.json`](record.schema.json)).

- The record is **frozen when the run returns**. Later writes by background nodes (statuses, `ctx` fields) are discarded.
- `duration_ms` (run and node) is measured on a monotonic clock. For a node, it runs from start until its outcome is recorded (completion or the decision to cancel). It is `null` for nodes that never started. The run's duration includes the cancellation wait.
- `started_at` is Unix epoch time in seconds (float), `null` for nodes that never started.
- `error_kind` is normative. `error` is human-readable text whose format differs between languages; do not parse it.
- `request_id` is the caller's id or, if none is given, a generated unique string (format not specified).
- `fields` holds values written by nodes through `ctx`. Writes are thread-safe; the last write to a key wins.

## 6. Hooks

`on_run_done(record)` is called exactly once for each run that produces a record, after the record is frozen, on the thread where the run completed. It must not throw or panic. In Python this is the `record_sink` of `GraphRunner`.

## 7. Versioning

- `spec_version` (this document) and `record_version` (in every record) are integers, versioned independently of release tags.
- Adding a field does not bump either. Renaming or removing a field, or changing semantics, bumps the corresponding version.
- A change to `spec/` is merged only when every existing implementation passes the new cases, or a case is marked `pending` for a specific language.
