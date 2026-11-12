# jabba (Rust)

Compiled graphs of async nodes: independent async nodes run concurrently, timeouts and failures follow the declared policy, and every run produces one schema-versioned record. Same semantics as the other jabba implementations — see [`spec/semantics.md`](../spec/semantics.md); the shared conformance cases in [`spec/cases/`](../spec/cases/) run in this crate's tests.

- **No dependencies**, no build script, no proc macros — `std` only. Easy to vendor into other build systems.
- **Executor-agnostic**: a run is a `Send + 'static` future; await it in tokio, smol, anything — or use `jabba::block_on`.
- **Typed wiring**: dependencies are typed node handles. For handles of the same builder, a wrong input type is a `rustc` error; everything else (handles by name, handles from another builder, cycles through `after`) is rejected by `compile()` before the first request.

## Quick taste

```rust
use std::time::Duration;
use jabba::{block_on, node, Graph, NodeError};

struct Request {
    user: String,
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut g = Graph::builder::<Request>("greeting");
    let req = g.request();
    let name = g.add(node("name").deps((&req,)).run_sync(|(r,), _ctx| Ok(r.user.clone())));
    let extra = g.add(
        node("extra")
            .run(|(), _ctx| async { Err::<String, _>(NodeError::msg("source down")) })
            .optional()
            .default(String::new()) // what dependents get if it fails
            .timeout(Duration::from_millis(100)),
    );
    let greeting = g.add(node("greeting").deps((&name, &extra)).run(|(name, extra), ctx| async move {
        ctx.set("had_extra", !extra.is_empty()); // goes into the run's record
        Ok(format!("hello, {name}{extra}"))
    }));
    let graph = g.compile_with_output(&greeting)?;

    let run = block_on(graph.run(Request { user: "alice".into() }))?;
    assert_eq!(run.output.as_deref().map(String::as_str), Some("hello, alice"));
    println!("{}", run.stat.to_record()); // one JSON record per run
    Ok(())
}
```

## Concepts

- **Nodes** — `node(name)`, then `.deps((&a, &b))` (typed handles) or `.deps_dyn::<V>(names)` (by name, checked at compile), then `.run(async body)` or `.run_sync(body)`. Policy: `.optional()`, `.default(v)`, `.timeout(d)`, `.after(names)` for ordering-only edges.
- **Sync nodes** run inline on the thread that polls the run, one at a time, and cannot be interrupted — use them for short CPU work. One that overruns its `timeout` is recorded as timed out and its result is discarded. Offload anything long to a blocking pool from an async node.
- **Graph** — `Graph::builder::<Request>(name)`, `add` nodes, `compile()` once at startup: cycles, unknown references, input types and optional defaults are checked before the first request. `CompiledGraph` is cheap to clone and serves concurrent runs.
- **Run** — `graph.run(request)` or `graph.run_with(request, RunOptions::default().deadline(d))`. `Ok(RunOk { output, stat })`, or `Err(RunError::NodeFailed { .. } | RunError::GraphTimeout { .. })` — both carry the record.
- **Errors in nodes** — `NodeError`. `?` converts any type that implements `std::error::Error`; for strings and boxed errors use `NodeError::msg(e.to_string())`. Panics are bugs: they are not caught, and a run that panicked refuses to be polled again.
- **Record** — `RequestStat::to_record()` returns the JSON record ([schema](../spec/record.schema.json)). `CompiledGraph::on_run_done(hook)` is called once per recorded run.

Node bodies are `Fn` and their futures are `'static`, so shared clients are cloned into each call:

```rust,ignore
let client = Arc::new(HttpClient::new());
let fetched = g.add(node("fetch").deps((&req,)).run(move |(r,), _ctx| {
    let client = client.clone();
    async move { Ok(client.get(&r.url).await?) }
}));
```

What a timeout or cancellation can and cannot stop: [`docs/cancellation.md`](../docs/cancellation.md).

## Development

```bash
cd rust
cargo test        # unit tests, conformance cases from ../spec, doc tests (including this README)
cargo clippy --all-targets -- -D warnings
```

MSRV: Rust 1.75. Tests find `spec/` next to `rust/`; set `JABBA_SPEC_DIR` if it lives elsewhere.
