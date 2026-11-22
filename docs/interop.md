# Mixing languages: foreign nodes

A graph in one language can contain nodes implemented in another. Such a *foreign node* crosses a small C ABI — [`ffi/jabba_ffi.h`](../ffi/jabba_ffi.h), rules in [`spec/ffi.md`](../spec/ffi.md) — and behaves like any other asynchronous node: timeouts, deadlines, optional defaults, cancellation and the record work as usual.

Values cross the boundary only on edges into and out of a foreign node: scalars, strings, bytes, JSON for structured data, and numeric arrays (`f64`, `f32`, `i64`) without copying. Edges between nodes of the same language are never converted.

## C++

`#include <jabba/ffi.hpp>`.

**Export a node** — a factory with C linkage returns a new instance per call:

```cpp
extern "C" const jabba_node* score_node() {
    return jabba::ffi::export_node<std::vector<double>>("score", [](const std::vector<double>& features) {
        double s = 0;
        for (double f : features) s += f;
        return s;
    });
}
```

The body is an ordinary node body: inputs as `const T&`, optionally `jabba::Ctx&` last (`ctx.set` writes into the host's record, `ctx.stop_token()` / `co_await ctx.stopped()` see the host's cancellation), returning `T` or `jabba::task<T>`. Synchronous bodies run on an executor, never inside the host's call. A coroutine body starts inside the host's call and runs until its first suspension there — keep that part short (the host may be a Python event loop). Exceptions become failures. `export_node_dyn<V>(name, {"a", "b"}, f)` exports a node whose inputs are named (`const DynInputs<V>&`).

**Use a foreign node**:

```cpp
auto score = jabba::ffi::Foreign::from_factory(&score_node);  // or Foreign::adopt(instance)
auto s = g.add(jabba::ffi::foreign<double>("score", jabba::deps(features), score));
```

Its declared kinds are checked against the C++ types right there (`CompileError`: `type_mismatch`, `abi_mismatch`). The node's stop token cancels the foreign call; the result is decoded into `T`.

**Types** (`jabba::ffi::Codec<T>`): `bool`, `std::int64_t`/`int`, `double`, `std::string`, `std::vector<std::uint8_t>` (bytes), `jabba::Json`, `std::vector<double|float|std::int64_t>`, `jabba::ffi::Array<double|float|std::int64_t>` (zero-copy: keeps the foreign memory until it is gone), `std::monostate` (null), `std::optional<T>` (nullable).

Foreign calls continue on `jabba::ffi::Host`, a small process-wide pool that outlives every run — a foreign node may finish after the run that started it.

## Rust

Behind the `ffi` feature: `jabba = { version = "...", features = ["ffi"] }`, then `use jabba::ffi;`. No extra dependencies.

**Export a node** — a factory with C linkage returns a new instance per call (build the crate as a `cdylib` or `staticlib` for hosts in other languages):

```rust
use jabba::ffi::{self, sys::jabba_node};

#[no_mangle]
pub extern "C" fn score_node() -> *const jabba_node {
    ffi::export("score", |(features,): (Vec<f64>,), _ctx| async move {
        Ok(features.iter().sum::<f64>())
    })
}
```

`export` takes an async body: inputs as a tuple, then the `Ctx` (`ctx.set` writes into the host's record, `ctx.cancel_token()` sees the host's cancellation). It starts inside the host's call and runs until it first waits — keep that part short. `export_sync` takes a synchronous body (`|(x,): (i64,), _ctx: &Ctx| Ok(x * 2)`), which never runs inside the host's call: always on the host's executor, or on a small pool of the crate's own. `export_dyn::<V, T, _, _>(name, &["a", "b"], f)` exports a node whose inputs are named (`DynInputs<V>`). Inputs arrive as owned values — arrays are copied, since the body's future outlives the host's loan of them. Cancellation by the host drops the body's future, as cancellation does in a Rust graph. A panic in an exported body fails the node: unwinding cannot cross the ABI.

**Use a foreign node**:

```rust,ignore
extern "C" {
    fn score_node() -> *const jabba_node; // linked in, or a symbol looked up at run time
}

let score = unsafe { ffi::Foreign::from_factory(score_node) }; // or Foreign::adopt(instance)
let s = g.add(node("score").deps((&features,)).foreign::<f64>(&score));
```

Its declared kinds are checked against the Rust types; a mismatch is reported by `compile()` (`TypeMismatch`, `AbiMismatch`). Dropping the node's future — a timeout, a deadline, a failed run — cancels the foreign call; a call the run gave up on still ends properly in the background.

**Types** (`ffi::FfiValue`): `bool`, `i64`, `f64`, `String`, `Vec<u8>` (bytes), `jabba::Json`, `Vec<f64|f32|i64>`, `ffi::Array<f64|f32|i64>` (zero-copy: keeps the foreign memory until it is gone; derefs to a slice), `()` (null), `Option<T>` (nullable).

## Python

`from jabba import ffi` — pure Python (`ctypes`); the package gets no compiled code.

**Get an instance** of a node exported by another language:

```python
score = ffi.load("./libscoring.so", "score_node")   # a factory in a shared library
score = ffi.from_capsule(module.score_node)          # a PyCapsule "jabba_node_factory" from an extension module
```

**Use it** like any node — `deps` name its inputs in the node's order (`"request"` passes the run's request), the policy is the same as for `@node`:

```python
graph = GraphSpec("scoring", output="score").add(
    parse,
    ffi.foreign(score, name="score", deps=["parse"], timeout=0.3, optional=True, default=0.0),
).compile()
```

**Values** follow the node's declared kinds: `None`, `bool`, `int` (range-checked: an int that does not fit into int64 fails the node instead of wrapping), `float`, `str`, `bytes`, JSON (dicts, lists, ...), numeric arrays. `None` may feed only nullable or JSON inputs — the graph checks this at compile time, as well as the kinds between two foreign nodes. Fields a foreign node writes into the record arrive as Python values (bytes as a list of byte values). An array input is passed **without copying** when it is a writable, aligned, C-contiguous buffer of the right element type (`array("d")`, a numpy `float64` array, ...); anything else — read-only, strided, Fortran-order, other element types, lists — is copied (unsigned integers are refused for `i64s`). An array result is a `memoryview` over the node's own memory — no copy — released when the last view of it is gone.

Native code never runs on the event loop: the host lets the node use its own executor, so it runs without the GIL and the loop stays free. A call the run gave up on (timeout, failure) still ends properly, also after the loop is closed; `ffi.live_calls()` counts calls not ended yet.

Measured on an Apple Silicon Mac, Python 3.13: a run of a one-node graph costs ~50 µs with a native async node and ~66 µs with a foreign node that answers at once; passing 1,000,000 `float64` to a C++ node and getting 1,000,000 back takes ~1.4 ms in total, most of it the node's own work (no copies at the boundary).
