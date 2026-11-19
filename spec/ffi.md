# jabba foreign-node ABI

`JABBA_FFI_ABI 1` — normative. The types are in [`ffi/jabba_ffi.h`](../ffi/jabba_ffi.h); this document states the rules. A *foreign node* is a node implemented in one language and used in a graph of another (the *host*). For the host, a foreign node is an ordinary asynchronous node: every rule of [`semantics.md`](semantics.md) applies unchanged.

## 1. Versions

- Every struct starts with `size` (`jabba_node` with `abi`, then `size`). A reader checks `size` before reading fields added later. Adding a field or a kind does not change `JABBA_FFI_ABI`; only an incompatible change does.
- A node with another `abi`, or a `jabba_node` smaller than the reader needs, is rejected when the graph is compiled: error kind `abi_mismatch`. The host does not call into such an instance at all — not even `release`, whose position it cannot know — and leaks it.
- An unknown kind in a declaration is `type_mismatch`; an unknown result status counts as `FAILED`.

## 2. Values

- Kinds: `NULL`, `BOOL` (`as.i` 0/1), `INT` (`as.i`), `FLOAT` (`as.f`), `STR` (UTF-8), `BYTES`, `JSON` (UTF-8, RFC 8259: no NaN/Infinity), `F64S`, `F32S`, `I64S`. Values in edges between nodes of the same language are never converted; values cross only into and out of foreign nodes.
- Arrays: `len` counts elements; `data` is aligned to the element size, C-contiguous and native-endian, and may be `NULL` only when `len == 0`. A host adapter copies input arrays that do not meet this.
- A declared kind may carry `JABBA_KIND_NULLABLE`: then `NULL` is accepted as well. The host rejects at compile time a provider that may yield null (an optional node whose default is null) feeding a non-nullable input; at run time it passes `NULL` only to nullable inputs.
- The host checks at compile time that the kinds it will exchange match the node's declaration, and at every call that the result's kind matches `output_kind`; a mismatch at run time fails the node.

**Ownership**
- Inputs are borrowed by the node until it calls `complete`. Their `release` belongs to the host.
- The value of an `OK` result passes to the host, which calls its `release` exactly once — after converting it, or later when it keeps the memory (zero-copy), including when the call was abandoned. `release` may be called from any thread at any time, independently of the call and of the node instance.
- A buffer whose `release` is `NULL` is valid only during the call that hands it over: the receiver copies it (a host copies such a result inside `complete`).
- A result that is not `OK` must have `value.release == NULL`; the host ignores its value.
- The result and its error text are borrowed only during `complete`.

## 3. A call

The host allocates the `jabba_call` and keeps it and its `host` valid from `start` until `end` returns — independently of the run, which may finish earlier (a per-node timeout does not wait for the node). Inputs stay valid until the node calls `complete`.

1. The node calls `call->complete` **exactly once**, from any thread — also from inside `start` or from inside `cancel`. After `complete` returns, the node does not touch the call, its inputs or `host`: no more `post`, no more `set_field`. Its own per-call state lives until `end`.
2. While calling `complete`, the node should hold no lock that its `cancel` takes. Hosts do not rely on it: a host never waits for its own `cancel` inside `complete` (the protocol kit checks exactly this with a node that does hold such a lock).
3. The host calls `cancel` at most once, only after `start` has returned and before `end`, never concurrently with `end`. `cancel` may race with `complete`; a `cancel` after `complete` is a no-op for the node. After `cancel`, `complete` still comes, with any status.
4. The host calls `end` exactly once, when `start` has returned and `complete` has been received — from any thread, possibly from inside `complete`. The node frees its per-call state in `end`; `end` may wait for a `set_field` of that call still in progress.
5. Neither side holds a lock of its own while calling the other, and the host calls no ABI function while holding the lock of its run.
6. `start` does not block: work that waits, or computes for long, goes to an executor. `complete` from inside `start` is for results that are already available.
7. Exceptions and panics never cross the boundary: they become `FAILED`.
8. A node that is cancelled reports `CANCELLED` (or `OK`/`FAILED` if it finished anyway). A `CANCELLED` the host did not ask for counts as a failure.

**Reference host state machine** (no locks): an atomic counter starts at 2 — "start has not returned" and "complete has not arrived". Cancelling increments the counter if it is not zero, calls `cancel`, and decrements. A cancellation wanted while `start` is still running sets a flag that the return path of `start` checks. Whoever brings the counter to zero calls `end`. The host never resumes its waiting code inside `complete`: it records the result and continues on its own executor.

**`post`**: runs `fn(arg)` later on the host's executor; thread-safe, never inline, every `fn` is called exactly once, even after the run has returned. It may be `NULL`: the node then uses an executor of its own. A host adapter therefore gives nodes an executor that outlives every run, not the run's own.

**`set_field`** writes into the run's record; the value is borrowed and copied by the host before it returns; writes after the run has returned are ignored.

## 4. Node instances

- A factory returns a **new** instance per call; whoever takes it calls `release` exactly once, after `end` of every call of that instance.
- If a node's own threads may still use the instance after `complete` (a lock, a counter), the node keeps the instance alive for them: `release` may come right after the last `end`, and `end` may come from inside `complete`.
- How an instance reaches the host is not part of the ABI: a factory linked into the host, a symbol of a shared library, a Python `PyCapsule` named `"jabba_node_factory"` holding the factory.

## 5. Conformance

- [`ffi/testing/jabba_ffi_testing.h`](../ffi/testing/jabba_ffi_testing.h): adversarial C nodes that check a host's importer (completing inside `start` and inside `cancel`, holding a lock across `complete`, racing `cancel` with `complete`, wrong kinds, wrong ABI, zero-copy arrays) and a C host that checks an exporter. Both sides count protocol violations by the other.
- [`ffi/testing/jabba_testkit.h`](../ffi/testing/jabba_testkit.h): scripted nodes behind the ABI. A host's conformance driver runs the cases of [`cases/`](cases/) with every node foreign.
