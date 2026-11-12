import json

import jsonschema
import pytest
from conformance_driver import (
    CAPABILITIES,
    LANGUAGE,
    SPEC_DIR,
    check,
    load_case_files,
    load_cases,
    run_case,
)

CASES = load_cases()


def _param(case):
    marks = []
    pending = case.get("pending", {})
    if LANGUAGE in pending:
        marks.append(pytest.mark.xfail(reason=pending[LANGUAGE], strict=True))
    missing = set(case.get("requires", [])) - CAPABILITIES
    if missing:
        marks.append(pytest.mark.skip(reason=f"driver does not support {sorted(missing)}"))
    return pytest.param(case, id=f"{case['_file']}::{case['name']}", marks=marks)


@pytest.mark.parametrize("case", [_param(c) for c in CASES])
async def test_case(case):
    outcome = await run_case(case)
    errors = check(case, outcome)
    assert not errors, "\n".join(errors)


def test_case_files_match_schema():
    schema = json.loads((SPEC_DIR / "case.schema.json").read_text(encoding="utf-8"))
    for name, content in load_case_files().items():
        try:
            jsonschema.validate(content, schema)
        except jsonschema.ValidationError as error:
            pytest.fail(f"{name}: {error.message}")


def test_case_names_are_unique():
    names = [case["name"] for case in CASES]
    assert len(names) == len(set(names))
