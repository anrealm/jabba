"""The Python importer against the adversarial C nodes of the protocol kit (ffi/testing).

Needs the kit as a shared library: JABBA_FFI_TESTING_LIB=/path/to/libjabba_ffi_testing.so
(built by cpp/CMakeLists.txt). Skipped without it.
"""

import asyncio
import ctypes
import gc
import os
import time
from array import array

import pytest

from jabba import ffi
from jabba.core.errors import GraphCompileError, GraphTimeoutError, NodeFailedError
from jabba.core.graph import GraphSpec

LIB_PATH = os.environ.get("JABBA_FFI_TESTING_LIB")
pytestmark = pytest.mark.skipif(not LIB_PATH, reason="JABBA_FFI_TESTING_LIB is not set")


class Counters(ctypes.Structure):
    _fields_ = [
        (n, ctypes.c_long)
        for n in (
            "starts",
            "ends",
            "cancels",
            "instances_made",
            "instances_released",
            "values_made",
            "values_released",
            "violations",
        )
    ]


@pytest.fixture(scope="module")
def kit():
    lib = ctypes.CDLL(LIB_PATH)
    lib.jt_first_violation.restype = ctypes.c_char_p
    return lib


def instance(kit, factory: str) -> ffi.Foreign:
    fn = getattr(kit, factory)
    fn.restype = ctypes.c_void_p
    return ffi.Foreign(fn(), keep=kit)


def counters(kit) -> Counters:
    c = Counters()
    kit.jt_counters_get(ctypes.byref(c))
    return c


def _settled(kit, instances: bool) -> bool:
    gc.collect()
    c = counters(kit)
    done = c.starts == c.ends and c.values_made == c.values_released
    return done and (not instances or c.instances_made == c.instances_released)


def settle(kit, *, instances: bool = True) -> Counters:
    """Wait until every call has ended (they may outlive their run), then check that the
    nodes saw no protocol violation and nothing leaked. For tests without a running loop."""
    for _ in range(400):
        if _settled(kit, instances):
            break
        time.sleep(0.005)
    return _check(kit, instances)


async def asettle(kit, *, instances: bool = True) -> Counters:
    """settle() for async tests: lets the loop run the deliveries it has been handed."""
    for _ in range(400):
        if _settled(kit, instances):
            break
        await asyncio.sleep(0.005)
    return _check(kit, instances)


def _check(kit, instances: bool) -> Counters:
    c = counters(kit)
    assert c.violations == 0, kit.jt_first_violation().decode()
    assert c.starts == c.ends
    assert c.values_made == c.values_released
    if instances:
        assert c.instances_made == c.instances_released
    assert ffi.live_calls() == 0
    return c


async def run_one(kit, factory, *, deps=(), request=None, timeout=None, deadline=None, **policy):
    node = ffi.foreign(instance(kit, factory), name="n", deps=deps, timeout=timeout, **policy)
    graph = GraphSpec("g", output="n").add(node).compile()
    return await graph.run(request, timeout=deadline)


@pytest.fixture(autouse=True)
def fresh(kit):
    kit.jt_counters_reset()
    yield


async def test_a_node_that_completes_inside_start(kit):
    result = await run_one(kit, "jt_node_inline")
    assert result.output == "inline"
    await asettle(kit)


async def test_a_node_completing_from_its_thread(kit):
    result = await run_one(kit, "jt_node_delay")
    assert result.output == "delayed"
    await asettle(kit)


async def test_a_node_timeout_cancels_the_foreign_call(kit):
    started = time.monotonic()
    result = await run_one(kit, "jt_node_delay", timeout=0.003, optional=True, default="default")
    assert time.monotonic() - started < 0.1
    assert result.output == "default"
    assert result.stat.nodes["n"].status == "timeout"
    assert (await asettle(kit)).cancels == 1


async def test_a_node_that_completes_inside_cancel(kit):
    with pytest.raises(GraphTimeoutError) as info:
        await run_one(kit, "jt_node_cancel_inline", deadline=0.01)
    assert info.value.stat.nodes["n"].error_kind == "deadline"
    del info  # its traceback holds the graph, and so the instance
    await asettle(kit)


async def test_a_node_holding_a_lock_across_complete_does_not_deadlock(kit):
    async def many():
        for i in range(200):
            timeout = 0.0015 + (i % 7) * 0.00015  # around the ~2 ms completion
            await run_one(kit, "jt_node_locked", timeout=timeout, optional=True, default=None)

    await asyncio.wait_for(many(), 30)
    await asettle(kit)


async def test_cancel_racing_complete(kit):
    foreign = instance(kit, "jt_node_race")
    node = ffi.foreign(foreign, name="n", timeout=0.00005, optional=True, default="timed out")
    graph = GraphSpec("g", output="n").add(node).compile()
    results = await asyncio.gather(*(graph.run(i) for i in range(500)))
    assert all(r.output in ("raced", "timed out") for r in results)
    await asettle(kit, instances=False)  # the graph still holds its instance


async def test_a_result_of_the_wrong_kind_fails_the_node(kit):
    with pytest.raises(NodeFailedError) as info:
        await run_one(kit, "jt_node_wrong_kind")
    assert "returned f32s" in str(info.value.__cause__)
    del info
    await asettle(kit)


async def test_a_failing_node_reports_its_error(kit):
    with pytest.raises(NodeFailedError) as info:
        await run_one(kit, "jt_node_failing")
    assert str(info.value.__cause__) == "boom"
    del info
    await asettle(kit)


def test_abi_and_arity_mismatches_are_compile_errors(kit):
    with pytest.raises(GraphCompileError) as abi:
        ffi.foreign(instance(kit, "jt_node_bad_abi"), name="n")
    assert abi.value.kind == "abi_mismatch"
    with pytest.raises(GraphCompileError) as arity:
        ffi.foreign(instance(kit, "jt_node_f64_scale"), name="n")  # takes one input
    assert arity.value.kind == "type_mismatch"
    del abi, arity
    settle(kit, instances=False)
    # An instance of another ABI is not even released through our layout: leaked, by design.
    c = counters(kit)
    assert (c.instances_made, c.instances_released) == (2, 1)


def test_kinds_between_foreign_nodes_are_checked_at_compile(kit):
    produces_json = ffi.foreign(instance(kit, "jt_node_inline"), name="j")
    takes_f64s = ffi.foreign(instance(kit, "jt_node_f64_scale"), name="s", deps=["j"])
    with pytest.raises(GraphCompileError) as info:
        GraphSpec("g").add(produces_json, takes_f64s).compile()
    assert info.value.kind == "type_mismatch"
    assert "returns json" in str(info.value)
    del info, produces_json, takes_f64s
    settle(kit)


def test_a_none_default_cannot_feed_a_non_nullable_input(kit):
    from jabba.core.node import node

    @node(optional=True, default=None)
    async def maybe() -> list:
        return [1.0]

    takes_f64s = ffi.foreign(instance(kit, "jt_node_f64_scale"), name="s", deps=["maybe"])
    with pytest.raises(GraphCompileError) as info:
        GraphSpec("g").add(maybe, takes_f64s).compile()
    assert info.value.kind == "type_mismatch"
    assert "defaults to None" in str(info.value)
    del info, takes_f64s
    settle(kit)


def test_integers_are_range_checked_not_wrapped():
    with pytest.raises(OverflowError):
        ffi._input_value(2**64 + 5, ffi.INT, [])
    with pytest.raises(OverflowError):
        ffi._array_view([2**63], ffi.I64S)
    with pytest.raises(TypeError):
        ffi._array_view(array("Q", [1]), ffi.I64S)  # unsigned: refused, not wrapped


def test_non_native_buffers_are_copied_not_rejected():
    data = array("d", [1.0, 2.0, 3.0, 4.0])
    strided = memoryview(data)[::2]
    assert list(ffi._array_view(strided, ffi.F64S)) == [1.0, 3.0]
    readonly = memoryview(bytes(memoryview(data).cast("B"))).cast("d")
    assert list(ffi._array_view(readonly, ffi.F64S)) == [1.0, 2.0, 3.0, 4.0]
    two_d = memoryview(data).cast("B").cast("d", (2, 2))
    assert list(ffi._array_view(two_d, ffi.F64S)) == [1.0, 2.0, 3.0, 4.0]
    ints = array("i", [1, 2, 3])
    assert list(ffi._array_view(ints, ffi.F64S)) == [1.0, 2.0, 3.0]


async def test_arrays_cross_without_copies_and_are_released_once(kit):
    data = array("d", [1.0, 2.5, 4.0])
    result = await run_one(kit, "jt_node_f64_scale", deps=["request"], request=data)
    view = result.output
    assert isinstance(view, memoryview)
    assert view.tolist() == [2.0, 5.0, 8.0]
    assert counters(kit).values_released == 0  # still the node's memory, behind the view
    del view, result
    await asettle(kit)


async def test_a_borrowed_result_buffer_is_copied_during_complete(kit):
    result = await run_one(kit, "jt_node_borrowed_str")
    assert result.output == "borrowed"
    await asettle(kit)


async def test_a_failed_result_carrying_a_value_still_releases_it(kit):
    with pytest.raises(NodeFailedError) as info:
        await run_one(kit, "jt_node_failed_with_value")
    assert str(info.value.__cause__) == "bad"
    del info
    await asettle(kit)


def test_calls_still_end_after_the_event_loop_is_closed(kit):
    # The run gives up on the node (timeout) and returns; asyncio.run closes the loop;
    # the node completes afterwards, from its own thread.
    asyncio.run(run_one(kit, "jt_node_delay", timeout=0.003, optional=True, default=None))
    settle(kit)


async def test_wrong_input_types_fail_the_node_before_the_call(kit):
    with pytest.raises(NodeFailedError) as info:
        await run_one(kit, "jt_node_f64_scale", deps=["request"], request="not numbers")
    assert "input 'request'" in str(info.value.__cause__)
    assert counters(kit).starts == 0
    del info
    await asettle(kit)
