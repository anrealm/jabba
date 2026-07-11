"""Toy scoring service: the full jabba cycle on fake data.

Default graph: parse -> (user_features | external_report) -> features ->
score -> response. A personal graph for the "vip" service reuses the same
nodes but shapes the response differently.

Run:  uvicorn examples.demo_scoring.app:app --reload
"""

import asyncio

from fastapi import HTTPException
from pydantic import BaseModel

from jabba import (
    GraphRegistry,
    GraphSpec,
    GraphTimeoutError,
    NodeFailedError,
    create_app,
    node,
)
from jabba.nodes import ModelApply, PydanticParse
from jabba.obs.request_record import RecordSink


class ScoreRequest(BaseModel):
    user_id: str
    amount: float = 0.0
    service: str | None = None


parse = PydanticParse(ScoreRequest, name="parsed")


@node(timeout=0.5)
async def user_features(parsed: ScoreRequest) -> dict:
    await asyncio.sleep(0.01)  # imagine a feature-store call
    return {"activity": float(len(parsed.user_id)), "amount": parsed.amount}


@node(optional=True, default={}, timeout=0.3)
async def external_report(parsed: ScoreRequest) -> dict:
    await asyncio.sleep(0.01)  # imagine a flaky third-party source
    return {"reports": 1.0}


@node()
async def features(user_features: dict, external_report: dict) -> dict:
    return {**user_features, **external_report}


class LinearModel(ModelApply):
    async def load(self, resources):
        return resources["weights"]

    def apply(self, model, features):
        return sum(model.get(name, 0.0) * value for name, value in features.items())


score = LinearModel(name="score", features="features")


@node()
async def response(parsed: ScoreRequest, score, ctx) -> dict:
    ctx.stat.set("score", score)
    return {"user_id": parsed.user_id, "score": score}


@node()
async def vip_response(parsed: ScoreRequest, score) -> dict:
    return {"user_id": parsed.user_id, "score": score * 2}


def build_registry() -> GraphRegistry:
    shared = (parse, user_features, external_report, features, score)
    default = GraphSpec("scoring_default", output="response").add(*shared, response).compile()
    vip = GraphSpec("scoring_vip", output="vip_response").add(*shared, vip_response).compile()
    registry = GraphRegistry(default=default)
    registry.register("vip", vip)
    return registry


def build_app(record_sink: RecordSink | None = None):
    app = create_app(
        registry=build_registry(),
        resources={"weights": {"activity": 1.0, "amount": 0.5, "reports": 2.0}},
        record_sink=record_sink,
        title="demo-scoring",
    )

    @app.post("/score")
    async def score_endpoint(payload: ScoreRequest, debug: bool = False) -> dict:
        try:
            result = await app.state.runner.run(payload.service, payload.model_dump())
        except NodeFailedError as error:
            raise HTTPException(status_code=500, detail=str(error)) from error
        except GraphTimeoutError as error:
            raise HTTPException(status_code=504, detail=str(error)) from error
        body = dict(result.output)
        if debug:
            body["debug"] = result.stat.to_record()
        return body

    return app


app = build_app()
