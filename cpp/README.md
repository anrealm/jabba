# jabba (C++)

Compiled graphs of async nodes: independent async nodes run concurrently, timeouts and failures follow the declared policy, and every run produces one schema-versioned record. Same semantics as the other jabba implementations — see [`spec/semantics.md`](../spec/semantics.md); the shared conformance cases in [`spec/cases/`](../spec/cases/) run in this library's tests.

- **Header-only, C++20, standard library only.** Include `<jabba/jabba.hpp>`, link threads. Easy to vendor into other build systems.
- **Bring your own executor**: jabba runs node code and continuations through a one-method `jabba::Executor` (`post`). `run_blocking` brings its own.
- **Typed wiring**: `deps(parse, side)` passes typed node handles, so node bodies get `const T&` inputs — generic lambdas included. Wrong types between graphs or by-name dependencies are rejected by `compile()` before the first request.
- Coroutine nodes return `jabba::task<T>`; plain functions are sync nodes.

## Quick taste

```cpp
#include <jabba/jabba.hpp>
using namespace std::chrono_literals;

struct Request { std::string user; };

jabba::GraphBuilder<Request> g("greeting");
auto req = g.request();
auto name = g.add(jabba::node("name", jabba::deps(req), [](const Request& r) { return r.user; }));
auto extra = g.add(jabba::node("extra", []() -> jabba::task<std::string> {
                       throw std::runtime_error("source down");
                       co_return "";
                   })
                       .optional()
                       .default_value("")  // what dependents get if it fails
                       .timeout(100ms));
auto greeting = g.add(jabba::node("greeting", jabba::deps(name, extra),
                                  [](const std::string& who, const std::string& more, jabba::Ctx& ctx) {
                                      ctx.set("had_extra", !more.empty());  // goes into the run's record
                                      return "hello, " + who + more;
                                  }));
auto graph = g.compile_with_output(greeting);

auto run = graph.run_blocking(Request{"alice"});
// *run.output == "hello, alice"; run.stat->to_record().dump() is the JSON record
```

This is [`examples/greeting.cpp`](examples/greeting.cpp), built and run by the tests.

## Concepts

- **Nodes** — `node(name, deps(a, b), f)`, `node(name, f)` without inputs, or `node(name, deps_dyn<V>({"a", "b"}), f)` by name (the body gets `const DynInputs<V>&`). The body may take `jabba::Ctx&` as its last parameter. Returning `task<T>` makes an async node; returning `T` makes a sync node (a returned reference is copied). Policy: `.optional()`, `.default_value(v)`, `.timeout(d)`, `.after({names})` for ordering-only edges.
- **Bodies are shared by concurrent runs** and called as `const`: no `mutable` lambdas (that is a compile error). They may own move-only state, such as a `std::unique_ptr` to a client.
- **Sync nodes** run inline on an executor thread and cannot be interrupted — use them for short CPU work. One that overruns its `timeout` is recorded as timed out and its result is discarded.
- **Graph** — `GraphBuilder<Request>`, `add` nodes, then `compile()` / `compile_with_output(node)` once at startup; problems throw `jabba::CompileError` with a `kind()`. A compiled graph is cheap to copy and serves concurrent runs.
- **Runs** — `run_blocking(request, options)` blocks the calling thread until the run finishes; `co_await graph.run(request, executor, options)` from a coroutine (start a top-level one with `jabba::spawn(task)`); or `run_async(request, executor, options, callback)`. `RunOptions{.deadline = 300ms, .stop = token}` (durations are integral; `duration_cast` floating ones). The result has `status`, `output`, `stat` (the record) and, for failures, `failed_node` and `error`.
- **Errors in nodes** — exceptions: a node that throws is `failed` with `what()` as its error.
- **Record** — `RequestStat::to_record()` returns the JSON record ([schema](../spec/record.schema.json)); `graph.on_run_done(hook)` is called once per recorded run.

### Executors

`jabba::Executor::post(fn)` must queue `fn` and return — never run it inline or block. The executor must outlive the run (until its result is delivered). `run_blocking` runs node code on the calling thread, except where a node's own awaitable resumes it elsewhere; do not call it from a thread whose event loop the nodes need, that deadlocks.

What a timeout or cancellation can and cannot stop: [`docs/cancellation.md`](../docs/cancellation.md).

## Building

```cmake
add_subdirectory(jabba/cpp)          # or install it and find the headers
target_link_libraries(app PRIVATE jabba::jabba)
```

Without CMake: add `cpp/include` to the include path, compile with `-std=c++20`, link with `-pthread`.

Tests: `cmake -S cpp -B build && cmake --build build && ctest --test-dir build`. They find `spec/` next to `cpp/`; set `JABBA_SPEC_DIR` if it lives elsewhere.

**Compilers**: GCC ≥ 13, Clang ≥ 16, Apple Clang from Xcode 15 (any deployment target). Exceptions and RTTI must be enabled. GCC 12 is not supported: it mis-destroys temporaries in `co_await` expressions, which breaks ordinary code such as `co_await graph.run(Request{...}, pool)`.
