"""Foreign nodes: nodes implemented in another language, in a Python graph.

A foreign node crosses the C ABI described in spec/ffi.md (ffi/jabba_ffi.h). This module
is pure Python (ctypes): it loads a node instance and makes it an ordinary async node.

    score = jabba.ffi.load("./libscoring.so", "score_node")
    graph = GraphSpec("g").add(parse, jabba.ffi.foreign(score, name="score", deps=["parse"]))

Values are converted by the node's declared kinds: ``None``, ``bool``, ``int``, ``float``,
``str``, ``bytes``, JSON (dicts, lists, ...), and numeric arrays — any C-contiguous buffer
of doubles, floats or int64 (``array('d')``, a numpy array, ...), passed without copying
when it is writable. Arrays returned by a node are exposed as ``memoryview`` over the
node's memory, released when the last view is gone.
"""

from __future__ import annotations

import asyncio
import ctypes
import itertools
import json
import numbers
import operator
import sys
import threading
import weakref
from array import array
from ctypes import (
    CFUNCTYPE,
    POINTER,
    Structure,
    Union,
    byref,
    c_char_p,
    c_double,
    c_float,
    c_int64,
    c_size_t,
    c_uint32,
    c_void_p,
)
from typing import Any

from loguru import logger

from .core.errors import GraphCompileError
from .core.node import UNSET, Node

ABI = 1

NULL, BOOL, INT, FLOAT, STR, BYTES, JSON, F64S, F32S, I64S = range(10)
NULLABLE = 0x100
KIND_MASK = 0xFF
OK, FAILED, CANCELLED = 0, 1, 2

_KIND_NAMES = ["null", "bool", "int", "float", "str", "bytes", "json", "f64s", "f32s", "i64s"]
_ARRAYS = {F64S: (c_double, "d"), F32S: (c_float, "f"), I64S: (c_int64, "q")}


def kind_name(kind: int) -> str:
    base = kind & KIND_MASK
    name = _KIND_NAMES[base] if base < len(_KIND_NAMES) else f"kind {base}"
    return name + "?" if kind & NULLABLE else name


# ---- the ABI's types (ffi/jabba_ffi.h) ------------------------------------------------


class _Buf(Structure):
    _fields_ = [("data", c_void_p), ("len", c_size_t)]


class _As(Union):
    _fields_ = [("i", c_int64), ("f", c_double), ("buf", _Buf)]


_VALUE_RELEASE = CFUNCTYPE(None, c_void_p)


class _Value(Structure):
    _fields_ = [("kind", c_uint32), ("as_", _As), ("release", _VALUE_RELEASE), ("owner", c_void_p)]


_POST = CFUNCTYPE(None, c_void_p, c_void_p, c_void_p)
_SET_FIELD = CFUNCTYPE(None, c_void_p, c_void_p, c_size_t, POINTER(_Value))


class _Host(Structure):
    _fields_ = [
        ("size", c_uint32),
        ("self", c_void_p),
        ("post", _POST),
        ("set_field", _SET_FIELD),
        ("request_id", c_void_p),
        ("request_id_len", c_size_t),
    ]


class _Result(Structure):
    _fields_ = [
        ("size", c_uint32),
        ("status", c_uint32),
        ("value", _Value),
        ("error", c_void_p),
        ("error_len", c_size_t),
    ]


class _Call(Structure):
    pass


_COMPLETE = CFUNCTYPE(None, POINTER(_Call), POINTER(_Result))
_Call._fields_ = [
    ("size", c_uint32),
    ("inputs", POINTER(_Value)),
    ("n_inputs", c_size_t),
    ("host", POINTER(_Host)),
    ("complete", _COMPLETE),
    ("host_data", c_void_p),
    ("node_data", c_void_p),
]

_NODE_FN = CFUNCTYPE(None, c_void_p, POINTER(_Call))
_NODE_RELEASE = CFUNCTYPE(None, c_void_p)


class _Node(Structure):
    _fields_ = [
        ("abi", c_uint32),
        ("size", c_uint32),
        ("self", c_void_p),
        ("name", c_char_p),
        ("output_kind", c_uint32),
        ("n_inputs", c_size_t),
        ("input_kinds", POINTER(c_uint32)),
        ("start", _NODE_FN),
        ("cancel", _NODE_FN),
        ("end", _NODE_FN),
        ("release", _NODE_RELEASE),
    ]


_FACTORY = CFUNCTYPE(c_void_p)

# ---- node instances ------------------------------------------------------------------


class Foreign:
    """A node instance from another language. ``release`` is called once, after the
    last reference (the graph's node, live calls) is gone."""

    def __init__(self, address: int, *, keep: Any = None) -> None:
        if not address:
            raise ValueError("jabba.ffi: null node instance")
        self._address = address
        self._keep = keep  # e.g. the shared library the factory came from
        header = _Node.from_address(address)
        #: Readable only for our ABI: another version may lay the struct out differently.
        self.compatible = header.abi == ABI and header.size >= ctypes.sizeof(_Node)
        self.abi = header.abi
        self.node = header
        if not self.compatible:
            # Not even release() is safe to call through our layout: leak it, loudly.
            logger.warning("jabba.ffi: node instance with ABI {} left unreleased", header.abi)
            return
        with _adopted_lock:
            if address in _adopted:
                raise ValueError("jabba.ffi: this node instance already has an owner")
            _adopted.add(address)
        weakref.finalize(self, Foreign._drop, self._state())

    @staticmethod
    def from_factory(factory: Any, *, keep: Any = None) -> Foreign:
        """`factory`: a ctypes function or a function address returning an instance."""
        fn = _FACTORY(factory) if isinstance(factory, int) else factory
        if getattr(fn, "restype", c_void_p) is not c_void_p:
            fn.restype = c_void_p
        return Foreign(fn(), keep=keep)

    # The instance's refcount lives in a small object so that finalize can reach it
    # without keeping the Foreign alive.
    def _state(self) -> _InstanceState:
        st = getattr(self, "_st", None)
        if st is None:
            st = self._st = _InstanceState(self.node, self._keep)
        return st

    @staticmethod
    def _drop(state: _InstanceState) -> None:
        state.unref()


_adopted: set[int] = set()
_adopted_lock = threading.Lock()


class _InstanceState:
    def __init__(self, node: _Node, keep: Any) -> None:
        self.node = node
        self.keep = keep
        self.lock = threading.Lock()
        self.refs = 1

    def ref(self) -> None:
        with self.lock:
            self.refs += 1

    def unref(self) -> None:
        with self.lock:
            self.refs -= 1
            last = self.refs == 0
        if last:
            with _adopted_lock:
                _adopted.discard(ctypes.addressof(self.node))
            self.node.release(self.node.self)


def load(path: str, factory: str) -> Foreign:
    """An instance from a factory exported by a shared library (handy in development)."""
    lib = ctypes.CDLL(path)
    fn = getattr(lib, factory)
    fn.restype = c_void_p
    fn.argtypes = []
    return Foreign(fn(), keep=lib)


_PyCapsule_GetPointer = ctypes.pythonapi.PyCapsule_GetPointer
_PyCapsule_GetPointer.restype = c_void_p
_PyCapsule_GetPointer.argtypes = [ctypes.py_object, c_char_p]


def from_capsule(capsule: Any) -> Foreign:
    """An instance from a ``PyCapsule`` named ``"jabba_node_factory"`` holding a factory."""
    address = _PyCapsule_GetPointer(capsule, b"jabba_node_factory")
    return Foreign.from_factory(address, keep=capsule)


# ---- values ----------------------------------------------------------------------------


def _scalar(kind: int) -> _Value:
    v = _Value()
    v.kind = kind
    return v


_INT64_MIN, _INT64_MAX = -(2**63), 2**63 - 1


def _is_bool(value: Any) -> bool:
    return isinstance(value, bool) or type(value).__name__ == "bool_"  # numpy.bool_


def _input_value(value: Any, declared: int, keep: list[Any]) -> _Value:
    """A borrowed value for an input of the declared kind; `keep` holds what it points into."""
    base = declared & KIND_MASK
    if value is None and (declared & NULLABLE or base == NULL):
        return _scalar(NULL)
    if base == BOOL and _is_bool(value):
        v = _scalar(BOOL)
        v.as_.i = int(bool(value))
        return v
    if base == INT and isinstance(value, numbers.Integral) and not _is_bool(value):
        i = operator.index(value)
        if not _INT64_MIN <= i <= _INT64_MAX:
            raise OverflowError(f"{i} does not fit into int64")
        v = _scalar(INT)
        v.as_.i = i
        return v
    if base == FLOAT and isinstance(value, numbers.Real) and not _is_bool(value):
        v = _scalar(FLOAT)
        v.as_.f = float(value)
        return v
    if base in (STR, JSON, BYTES):
        if base == STR and isinstance(value, str):
            data = value.encode()
        elif base == JSON:
            data = json.dumps(value, allow_nan=False, separators=(",", ":")).encode()
        elif base == BYTES and isinstance(value, bytes | bytearray | memoryview):
            data = bytes(value)
        else:
            raise TypeError(f"expected {kind_name(declared)}, got {type(value).__name__}")
        ptr = c_char_p(data)  # points into `data`, no copy
        keep.extend((data, ptr))
        v = _scalar(base)
        v.as_.buf.data = ctypes.cast(ptr, c_void_p).value
        v.as_.buf.len = len(data)
        return v
    if base in _ARRAYS:
        arr = _array_view(value, base)
        keep.append(arr)
        v = _scalar(base)
        v.as_.buf.data = ctypes.addressof(arr) if len(arr) else None
        v.as_.buf.len = len(arr)
        return v
    raise TypeError(f"expected {kind_name(declared)}, got {type(value).__name__}")


def _normal_format(fmt: str) -> str:
    """'@d', '=d', '<d' on a little-endian machine... -> 'd'; 'l' -> 'q' where long is
    64-bit. Non-native byte orders keep their prefix (and are converted, not viewed)."""
    native = "<" if sys.byteorder == "little" else ">"
    if fmt[:1] in ("@", "=", native):
        fmt = fmt[1:]
    if fmt == "l" and ctypes.sizeof(ctypes.c_long) == 8:
        fmt = "q"
    return fmt


def _flatten(items: Any) -> list[Any]:
    if isinstance(items, list):
        return [x for item in items for x in _flatten(item)]
    return [items]


def _array_view(value: Any, kind: int) -> Any:
    """A ctypes array over `value`'s memory when it is a writable, aligned, C-contiguous
    buffer of the native element type. Otherwise a copy: byte-for-byte when only the
    layout differs (read-only, strided, Fortran order, misaligned), converted when the
    element type differs (lists, other numeric types)."""
    ctype, fmt = _ARRAYS[kind]
    if isinstance(value, list | tuple):
        items = list(value)
    else:
        mv = memoryview(value)
        same_type = _normal_format(mv.format) == fmt and mv.itemsize == ctypes.sizeof(ctype)
        if same_type:
            count = mv.nbytes // ctypes.sizeof(ctype)
            if mv.c_contiguous and not mv.readonly:
                flat = mv.cast("B").cast(mv.format) if mv.ndim != 1 else mv
                arr = (ctype * count).from_buffer(flat)
                if ctypes.addressof(arr) % ctypes.alignment(ctype) == 0:
                    return arr
            return (ctype * count).from_buffer_copy(mv.tobytes())  # tobytes: C order
        if kind == I64S and _normal_format(mv.format) in ("L", "Q", "B", "H", "I", "N"):
            raise TypeError(f"unsigned integers ({mv.format}) do not fit into i64s")
        items = _flatten(mv.tolist())
    if kind == I64S:
        items = array("q", items)  # range-checks every element
    return (ctype * len(items))(*items)


def _owned_view(v: _Value) -> memoryview:
    """Zero-copy: a memoryview over the node's array, released when the last view is gone."""
    ctype, fmt = _ARRAYS[v.kind & KIND_MASK]
    n = v.as_.buf.len
    if n and not v.as_.buf.data:
        if v.release:
            v.release(v.owner)
        raise ValueError(f"{kind_name(v.kind)} of length {n} without data")
    if n == 0:
        if v.release:
            v.release(v.owner)
        return memoryview((ctype * 0)()).cast("B").cast(fmt)
    arr = (ctype * n).from_address(v.as_.buf.data)
    if v.release:
        weakref.finalize(arr, v.release, v.owner)
    return memoryview(arr).cast("B").cast(fmt)


def _element_size(kind: int) -> int:
    base = kind & KIND_MASK
    return ctypes.sizeof(_ARRAYS[base][0]) if base in _ARRAYS else 1


def _decode(v: _Value) -> Any:
    """A value the host now owns, as a Python object; releases it unless kept (arrays)."""
    base = v.kind & KIND_MASK
    try:
        if base in _ARRAYS:
            return _owned_view(v)
        if base == NULL:
            return None
        if base == BOOL:
            return bool(v.as_.i)
        if base == INT:
            return int(v.as_.i)
        if base == FLOAT:
            return float(v.as_.f)
        data = ctypes.string_at(v.as_.buf.data, v.as_.buf.len) if v.as_.buf.len else b""
        if base == STR:
            return data.decode()
        if base == BYTES:
            return data
        if base == JSON:
            return json.loads(data, parse_constant=_reject_constant)
        raise ValueError(f"unknown kind {base}")
    finally:
        if base not in _ARRAYS and v.release:
            v.release(v.owner)


def _reject_constant(name: str) -> Any:
    raise ValueError(f"{name} is not JSON (RFC 8259)")


def _to_record(v: _Value) -> Any:
    """A borrowed value (set_field) as something to put into the record."""
    base = v.kind & KIND_MASK
    if base in _ARRAYS:
        ctype, _ = _ARRAYS[base]
        return list((ctype * v.as_.buf.len).from_address(v.as_.buf.data)) if v.as_.buf.len else []
    if base == BYTES:
        return list(ctypes.string_at(v.as_.buf.data, v.as_.buf.len))
    copy = _Value.from_buffer_copy(v)
    copy.release = _VALUE_RELEASE()
    return _decode(copy)


def _feeds(produced: int, consumed: int) -> bool:
    """Can a node declaring `produced` feed an input declaring `consumed`?"""
    if produced & KIND_MASK != consumed & KIND_MASK:
        return False
    return not produced & NULLABLE or bool(consumed & NULLABLE)


def _fits(kind: int, declared: int) -> bool:
    base = kind & KIND_MASK
    if base == NULL:
        return bool(declared & NULLABLE) or declared & KIND_MASK == NULL
    return base == declared & KIND_MASK


# ---- one call: the host side of spec/ffi.md §3 --------------------------------------------

_calls: dict[int, _CallSlot] = {}
_calls_lock = threading.Lock()
_ids = itertools.count(1)


class _CallSlot:
    """Owns everything the node may touch until end(): the call, the host, the inputs and
    what they point into. Registered by id so that the C callbacks find it."""

    def __init__(
        self, inst: _InstanceState, name: str, ctx: Any, loop: asyncio.AbstractEventLoop
    ) -> None:
        self.inst = inst
        self.name = name
        self.ctx = ctx
        self.loop = loop
        self.loop_thread = threading.get_ident()
        self.future: asyncio.Future[Any] = loop.create_future()
        self.keep: list[Any] = []
        self.lock = threading.Lock()
        self.refs = 2  # "start has not returned" + "complete has not arrived"
        self.start_returned = self.cancel_wanted = self.cancel_issued = False
        self.abandoned = False
        self.id = next(_ids)
        self.request_id = (ctx.request_id or "").encode()
        self.host = _Host()
        self.host.size = ctypes.sizeof(_Host)
        self.host.self = None
        self.host.post = _POST()  # NULL: the node uses its own executor
        self.host.set_field = _set_field_trampoline
        self.host.request_id = ctypes.cast(c_char_p(self.request_id), c_void_p).value
        self.host.request_id_len = len(self.request_id)
        self.call = _Call()
        self.call.size = ctypes.sizeof(_Call)
        self.call.host = ctypes.pointer(self.host)
        self.call.complete = _complete_trampoline
        self.call.host_data = self.id
        self.call.node_data = None

    def set_inputs(self, values: list[_Value]) -> None:
        arr = (_Value * max(len(values), 1))(*values)
        self.keep.append(arr)
        self.call.inputs = ctypes.cast(arr, POINTER(_Value))
        self.call.n_inputs = len(values)

    def begin(self) -> None:
        with _calls_lock:
            _calls[self.id] = self
        self.host.self = self.id
        self.inst.ref()
        node = self.inst.node
        node.start(node.self, byref(self.call))
        with self.lock:
            self.start_returned = True
            cancel = self.cancel_wanted
        if cancel:
            self._do_cancel()
        self._unref()

    def abandon(self) -> None:
        """The run gave up on this call (on the loop thread): a result that arrived, or
        arrives later, is released, never handed to anyone."""
        with self.lock:
            self.abandoned = True
        if self.future.done() and not self.future.cancelled():
            self._discard(self.future.result())
        else:
            self.future.cancel()
        self.request_cancel()

    def request_cancel(self) -> None:
        with self.lock:
            self.cancel_wanted = True
            ready = self.start_returned
        if ready:
            self._do_cancel()

    def _do_cancel(self) -> None:
        with self.lock:
            if self.cancel_issued or self.refs == 0:
                return
            self.cancel_issued = True
            self.refs += 1
        node = self.inst.node
        node.cancel(node.self, byref(self.call))
        self._unref()

    def _unref(self) -> None:
        with self.lock:
            self.refs -= 1
            last = self.refs == 0
        if last:
            node = self.inst.node
            node.end(node.self, byref(self.call))
            with _calls_lock:
                _calls.pop(self.id, None)
            self.inst.unref()

    def on_complete(self, status: int, value: _Value | None, error: str) -> None:
        """From the complete() callback, on any thread: hand the outcome to the loop."""
        outcome = (status, value, error)
        with self.lock:
            abandoned = self.abandoned
        if abandoned:  # nobody waits: do not depend on the loop running again
            self._discard(outcome)
        elif threading.get_ident() == self.loop_thread:
            self._deliver(outcome)
        else:
            try:
                self.loop.call_soon_threadsafe(self._deliver, outcome)
            except RuntimeError:  # the loop is closed: nobody waits any more
                self._discard(outcome)
        self._unref()

    def _deliver(self, outcome: tuple[int, _Value | None, str]) -> None:
        if self.future.done():  # abandoned (timeout, cancellation)
            self._discard(outcome)
        else:
            self.future.set_result(outcome)

    @staticmethod
    def _discard(outcome: tuple[int, _Value | None, str]) -> None:
        _, value, _ = outcome
        if value is not None and value.release:
            value.release(value.owner)


def _copy_result(r: _Result) -> tuple[int, _Value | None, str]:
    """Inside complete(): the result is borrowed until we return."""
    status = r.status
    if status == OK:
        v = _Value.from_buffer_copy(r.value)
        base = v.kind & KIND_MASK
        borrowed = (base in (STR, BYTES, JSON) or base in _ARRAYS) and not v.release
        if borrowed and v.as_.buf.len:
            # valid only during complete(): copy it now
            nbytes = v.as_.buf.len * _element_size(v.kind)
            data = ctypes.create_string_buffer(ctypes.string_at(v.as_.buf.data, nbytes), nbytes)
            holder_id = id(data)
            _orphans[holder_id] = data
            v.as_.buf.data = ctypes.addressof(data)
            v.owner = holder_id
            v.release = _release_orphan
        return OK, v, ""
    if r.value.release:  # a misbehaving node: still no leak
        r.value.release(r.value.owner)
    error = (
        ctypes.string_at(r.error, r.error_len).decode("utf-8", "replace")
        if r.error and r.error_len
        else ""
    )
    return status, None, error


_orphans: dict[int, Any] = {}


@_VALUE_RELEASE
def _release_orphan(owner: int) -> None:
    _orphans.pop(owner, None)


@_COMPLETE
def _complete_trampoline(call: Any, result: Any) -> None:
    slot = None
    try:
        slot_id = call.contents.host_data
        with _calls_lock:
            slot = _calls.get(slot_id)
        if slot is None:
            return
        status, value, error = _copy_result(result.contents)
    except BaseException as e:  # never into C; the call still completes
        logger.opt(exception=e).error("jabba.ffi: unreadable result from a foreign node")
        if slot is not None:
            slot.on_complete(FAILED, None, f"jabba.ffi: unreadable result: {e}")
        return
    slot.on_complete(status, value, error)


@_SET_FIELD
def _set_field_trampoline(self_id: int, key: int, key_len: int, value: Any) -> None:
    try:
        with _calls_lock:
            slot = _calls.get(self_id)
        if slot is None:
            return
        name = ctypes.string_at(key, key_len).decode("utf-8", "replace")
        slot.ctx.stat.set(name, _to_record(value.contents))
    except BaseException as e:  # never into C; a malformed value is dropped
        logger.opt(exception=e).debug("jabba.ffi: field dropped")


# ---- the node ----------------------------------------------------------------------------


class ForeignNode(Node):
    """A foreign node in a Python graph. Behaves like any async node: timeouts, deadline,
    optional defaults and cancellation apply as usual (spec/semantics.md)."""

    def __init__(
        self,
        foreign: Foreign,
        *,
        name: str,
        deps: list[str] | tuple[str, ...] = (),
        optional: bool = False,
        default: Any = UNSET,
        timeout: float | None = None,
        after: list[str] | tuple[str, ...] = (),
    ) -> None:
        node = foreign.node
        if not foreign.compatible:
            raise GraphCompileError(
                f"foreign node '{name}': ABI version {foreign.abi}, expected {ABI}",
                kind="abi_mismatch",
            )
        kinds = [node.input_kinds[i] for i in range(node.n_inputs)] + [node.output_kind]
        unknown = [k for k in kinds if k & KIND_MASK > I64S or k & ~(KIND_MASK | NULLABLE)]
        if unknown:
            raise GraphCompileError(
                f"foreign node '{name}': unknown kind {unknown[0]}", kind="type_mismatch"
            )
        if node.n_inputs != len(deps):
            raise GraphCompileError(
                f"foreign node '{name}': takes {node.n_inputs} inputs, the graph wires {len(deps)}",
                kind="type_mismatch",
            )
        self._foreign = foreign
        self._state = foreign._state()
        self._inputs = list(deps)
        self._kinds = [node.input_kinds[i] for i in range(node.n_inputs)]
        self._output_kind = node.output_kind
        self.foreign_output_kind = node.output_kind  # read by check_wiring of consumers
        self.name = name
        self.deps = tuple(d for d in deps if d != "request")  # the request is injected
        self.after = tuple(after)
        self.optional = optional
        self.default = default
        self.timeout = timeout
        super().__init__()

    def check_wiring(self, nodes: dict[str, Node], graph: str) -> None:
        """Called by GraphSpec.compile: kinds between foreign nodes must match, and a
        provider that may yield None (an optional node with default None) may feed only
        a nullable input — or a JSON one, where None is the JSON null."""
        for dep, kind in zip(self._inputs, self._kinds, strict=True):
            if dep == "request":
                continue
            provider = nodes[dep]
            produced = getattr(provider, "foreign_output_kind", None)
            if produced is not None and not _feeds(produced, kind):
                raise GraphCompileError(
                    f"graph '{graph}': foreign node '{self.name}' takes {kind_name(kind)} as "
                    f"'{dep}', which returns {kind_name(produced)}",
                    kind="type_mismatch",
                )
            maybe_none = provider.optional and provider.default is None
            if maybe_none and not kind & NULLABLE and kind & KIND_MASK not in (JSON, NULL):
                raise GraphCompileError(
                    f"graph '{graph}': optional node '{dep}' defaults to None, but foreign "
                    f"node '{self.name}' takes a non-nullable {kind_name(kind)}",
                    kind="type_mismatch",
                )

    async def run(self, request: Any, ctx: Any, **inputs: Any) -> Any:
        loop = asyncio.get_running_loop()
        slot = _CallSlot(self._state, self.name, ctx, loop)
        values = []
        for dep, kind in zip(self._inputs, self._kinds, strict=True):
            value = request if dep == "request" else inputs[dep]
            try:
                values.append(_input_value(value, kind, slot.keep))
            except (TypeError, ValueError, OverflowError, NotImplementedError) as e:
                raise TypeError(f"foreign node '{self.name}', input '{dep}': {e}") from None
        slot.set_inputs(values)
        slot.begin()
        try:
            status, value, error = await asyncio.shield(slot.future)
        except asyncio.CancelledError:
            slot.abandon()  # complete() still comes; end() follows from the slot
            raise
        if status == OK:
            assert value is not None
            if not (_fits(value.kind, self._output_kind)):
                got = value.kind
                _CallSlot._discard((status, value, error))
                raise TypeError(
                    f"foreign node '{self.name}' returned {kind_name(got)}, "
                    f"expected {kind_name(self._output_kind)}"
                )
            return _decode(value)
        if status == CANCELLED:
            raise RuntimeError(f"foreign node '{self.name}' was cancelled")
        raise RuntimeError(error or f"foreign node '{self.name}' failed")


def foreign(
    target: Foreign, *, name: str, deps: list[str] | tuple[str, ...] = (), **policy: Any
) -> ForeignNode:
    """A graph node for a foreign node instance. `deps` name its inputs in the node's
    order (``"request"`` passes the run's request); `policy`: optional, default,
    timeout, after — as for ``@node``."""
    return ForeignNode(target, name=name, deps=deps, **policy)


def live_calls() -> int:
    """Foreign calls not ended yet (a node that ignores cancellation keeps one alive)."""
    with _calls_lock:
        return len(_calls)
