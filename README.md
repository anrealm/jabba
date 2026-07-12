# 🛸 JABBA

**Just Another Backend-Based Application** — a graph-based async backend framework on top of FastAPI.

![Jabba](docs/images/jabba.png)

The core idea: a request is served by a **precompiled graph of computable async nodes**. Independent nodes run in parallel, timings and failures are recorded automatically, and every run produces one finalized, schema-versioned record.

## Concepts

- **Node** — a unit of computation with a simple interface. Dependencies are inferred from the signature: each parameter names the node whose result it receives (`request` and `ctx` are reserved and injected).
- **GraphSpec → CompiledGraph** — graphs are assembled declaratively and compiled once at startup: cycle/dependency/contract validation happens before the first request, not during it.
- **GraphRegistry** — resolution, not inheritance: a default graph serves everyone, while a service (or any other key) may register its own personal graph assembled from the same reusable nodes.
- **RequestStat** — a per-request entity enriched by the executor (per-node status and timings) and by nodes themselves (`ctx.stat.set(...)`).

## Quick taste

```python
from jabba import GraphSpec, GraphRegistry, create_app, node

@node()
async def parse(request) -> dict:
    return {"user_id": request["user_id"]}

@node(optional=True, default={}, timeout=0.3)
async def side_data(parse: dict) -> dict:      # runs in parallel with main_data
    ...

@node()
async def main_data(parse: dict) -> dict:
    ...

@node()
async def result(main_data: dict, side_data: dict, ctx) -> dict:
    ctx.stat.set("side_used", bool(side_data))
    return {**main_data, **side_data}

graph = GraphSpec("main", output="result").add(parse, main_data, side_data, result).compile()
registry = GraphRegistry(default=graph)

app = create_app(registry=registry)  # /ping, /metrics, node lifecycle — wired

@app.post("/handle")
async def handle(payload: dict):
    run = await app.state.runner.run(payload.get("service"), payload)
    return run.output
```

## Execution semantics

- **Parallel by construction** — a node starts the moment its dependencies are done; no stages, no barriers.
- **Fail-fast by default** — a failed node aborts the run. Mark a node `optional=True` (with a declared `default`) and its failure or timeout degrades gracefully: dependents receive the default, the error lands in RequestStat.
- **Timeouts** — per-node `timeout` plus a whole-graph deadline.
- **Ordering-only edges** — `after=["warmup"]` for dependencies that carry no data.

## Node lifecycle

Nodes that need resources (connection pools, models, warm caches) override `setup(resources)` / `shutdown()` — called once at app startup/shutdown, deduplicated across graphs. Per-request initialization is just a regular node.

## Observability

Two channels, deliberately separate:

- **Technical log** (loguru) — for humans and grep: errors, node events, `request_id` in every line.
- **RequestRecord** — exactly one JSON record per run (finalized `RequestStat`: statuses, timings, node errors, custom fields), schema-versioned, pluggable sink (`jsonl_sink(path)` out of the box). Meant for training datasets and analytics.

Plus prometheus metrics at `/metrics`: per-node and per-graph duration histograms, request and failure counters.

## Bundled nodes (`jabba.nodes`)

Reusable bricks with configurable input wiring:

- `PydanticParse` — validate the incoming request into a pydantic model.
- `ModelApply` — contract node: load a model once at startup, apply it per request. Jabba knows no model library — the consumer injects a provider. The model interface is `SupportsPredict` (anything with `predict(features)`):

  ```python
  score = ModelApply(
      name="score", features_from="user_features",   # the node whose result feeds the model
      loader=lambda resources: my_model_registry.load_model("demo", version=3),
  )
  ```

  The loader may be sync or async; for custom logic subclass and override `load()` / `apply()`.

## Demo

A complete toy scoring service — parse → parallel fetches (one optional) → linear model → response, with a personal graph for the `vip` service:

```bash
uvicorn examples.demo_scoring.app:app --reload
curl -s localhost:8000/score -X POST -H 'content-type: application/json' \
  -d '{"user_id": "alice", "amount": 2.0}'
curl -s 'localhost:8000/score?debug=1' -X POST -H 'content-type: application/json' \
  -d '{"user_id": "alice", "amount": 2.0, "service": "vip"}'
```

## Development

```bash
python -m venv .venv && .venv/bin/pip install -e . pytest pytest-asyncio httpx ruff
.venv/bin/python -m pytest
.venv/bin/ruff check .
```

Python 3.12+. Runtime deps: fastapi, pydantic v2, uvicorn, loguru, prometheus-client — nothing else.
