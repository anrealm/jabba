//! Using a foreign node in a Rust graph: the host side of spec/ffi.md §3.

use std::any::Any;
use std::cell::UnsafeCell;
use std::ffi::{c_char, c_void, CStr};
use std::future::Future;
use std::marker::PhantomData;
use std::pin::Pin;
use std::sync::atomic::{AtomicBool, AtomicI32, Ordering::SeqCst};
use std::sync::{Arc, Mutex};
use std::task::{Context, Poll, Waker};

use super::sys::*;
use super::value::{to_json, FfiValue, Scratch};
use crate::ctx::{lock, Ctx};
use crate::error::{CompileError, CompileErrorKind, NodeError};
use crate::node::{Dyn, Inputs, NodeBuilder, NodeDef, Typed};

/// A node instance from another language. Owns it: `release` is called once, after the
/// graph and every call that used it are gone. Cheap to clone.
#[derive(Clone)]
pub struct Foreign {
    inst: Arc<Instance>,
}

struct Instance {
    node: *const jabba_node,
    abi: u32,
    /// Readable through our layout only for our ABI version; otherwise never called into.
    compatible: bool,
}

unsafe impl Send for Instance {}
unsafe impl Sync for Instance {}

impl Drop for Instance {
    fn drop(&mut self) {
        if self.compatible {
            let n = unsafe { &*self.node };
            if let Some(release) = n.release {
                unsafe { release(n.self_) };
            }
        }
    }
}

impl Foreign {
    /// Take ownership of an instance.
    ///
    /// # Safety
    /// `node` must point to a `jabba_node` handed over to the caller (from a factory, ...),
    /// not owned by anyone else.
    pub unsafe fn adopt(node: *const jabba_node) -> Foreign {
        assert!(!node.is_null(), "jabba::ffi::Foreign::adopt: null node");
        // Only the header is readable before the version is known: no `&jabba_node` yet.
        let abi = std::ptr::addr_of!((*node).abi).read();
        let size = std::ptr::addr_of!((*node).size).read();
        let compatible = abi == JABBA_FFI_ABI && size as usize >= std::mem::size_of::<jabba_node>();
        Foreign {
            inst: Arc::new(Instance {
                node,
                abi,
                compatible,
            }),
        }
    }

    /// A new instance from a factory.
    ///
    /// # Safety
    /// `factory` must follow spec/ffi.md (each call returns a new instance).
    pub unsafe fn from_factory(factory: Factory) -> Foreign {
        Foreign::adopt(factory())
    }

    /// Only for a compatible instance.
    fn node(&self) -> &jabba_node {
        debug_assert!(self.inst.compatible);
        unsafe { &*self.inst.node }
    }

    fn check(&self, name: &str, inputs: &[u32], output: u32) -> Result<(), CompileError> {
        let fail = |kind, why: String| {
            Err(CompileError {
                kind,
                message: format!("foreign node '{name}': {why}"),
            })
        };
        if !self.inst.compatible {
            return fail(
                CompileErrorKind::AbiMismatch,
                format!("ABI version {}, expected {JABBA_FFI_ABI}", self.inst.abi),
            );
        }
        let n = self.node();
        if n.start.is_none() || n.cancel.is_none() || n.end.is_none() {
            return fail(
                CompileErrorKind::AbiMismatch,
                "start, cancel and end are required".to_owned(),
            );
        }
        if n.n_inputs != inputs.len() {
            return fail(
                CompileErrorKind::TypeMismatch,
                format!(
                    "takes {} inputs, the graph wires {}",
                    n.n_inputs,
                    inputs.len()
                ),
            );
        }
        let Some(declared) = (unsafe { items(n.input_kinds, n.n_inputs) }) else {
            return fail(
                CompileErrorKind::TypeMismatch,
                format!("declares {} inputs without their kinds", n.n_inputs),
            );
        };
        for (i, (&ours, &theirs)) in inputs.iter().zip(declared).enumerate() {
            if !known_kind(theirs) || !kind_feeds(ours, theirs) {
                return fail(
                    CompileErrorKind::TypeMismatch,
                    format!(
                        "input {i} is {}, the graph passes {}",
                        kind_name(theirs),
                        kind_name(ours)
                    ),
                );
            }
        }
        if !known_kind(n.output_kind) || !kind_feeds(n.output_kind, output) {
            return fail(
                CompileErrorKind::TypeMismatch,
                format!(
                    "returns {}, the graph expects {}",
                    kind_name(n.output_kind),
                    kind_name(output)
                ),
            );
        }
        Ok(())
    }
}

/// Dependencies whose values can be passed to a foreign node.
pub trait ForeignInputs: Inputs {
    #[doc(hidden)]
    fn kinds(n: usize) -> Vec<u32>;
    #[doc(hidden)]
    fn views(args: &Self::Args, scratch: &mut Scratch) -> Vec<jabba_value>;
}

impl ForeignInputs for () {
    fn kinds(_: usize) -> Vec<u32> {
        Vec::new()
    }
    fn views(_: &(), _: &mut Scratch) -> Vec<jabba_value> {
        Vec::new()
    }
}

impl<V: FfiValue> ForeignInputs for Dyn<V> {
    fn kinds(n: usize) -> Vec<u32> {
        vec![V::KIND; n]
    }
    fn views(args: &Self::Args, scratch: &mut Scratch) -> Vec<jabba_value> {
        args.iter().map(|(_, v)| v.view(scratch)).collect()
    }
}

macro_rules! foreign_tuple {
    ($($t:ident),+) => {
        impl<$($t: FfiValue),+> ForeignInputs for Typed<($($t,)+)> {
            fn kinds(_: usize) -> Vec<u32> {
                vec![$($t::KIND),+]
            }
            #[allow(non_snake_case)]
            fn views(args: &Self::Args, scratch: &mut Scratch) -> Vec<jabba_value> {
                let ($($t,)+) = args;
                vec![$($t.view(scratch)),+]
            }
        }
    };
}

foreign_tuple!(A);
foreign_tuple!(A, B);
foreign_tuple!(A, B, C);
foreign_tuple!(A, B, C, D);
foreign_tuple!(A, B, C, D, E);
foreign_tuple!(A, B, C, D, E, F);
foreign_tuple!(A, B, C, D, E, F, G);
foreign_tuple!(A, B, C, D, E, F, G, H);

impl<I: ForeignInputs> NodeBuilder<I> {
    /// The node is implemented by a foreign node: `node("score").deps((&f,)).foreign::<f64>(&score)`.
    /// Kinds are checked against the Rust types; a mismatch is reported by `compile`.
    pub fn foreign<T: FfiValue>(self, f: &Foreign) -> NodeDef<T> {
        let name = self.name.to_string();
        let error = f.check(&name, &I::kinds(self.slots.len()), T::KIND).err();
        let inst = f.inst.clone();
        let call_name: Arc<str> = Arc::from(name.as_str());
        let mut def = self.run(move |args: I::Args, ctx: Ctx| {
            let mut scratch = Scratch::default();
            let inputs = I::views(&args, &mut scratch);
            ForeignCall::<T>::new(
                Slot::new(inst.clone(), ctx, inputs, scratch, Box::new(args)),
                call_name.clone(),
            )
        });
        def.error = error;
        def
    }
}

enum Outcome {
    Ok(jabba_value),
    Failed(String),
    Cancelled,
}

#[derive(Default)]
struct SlotState {
    outcome: Option<Outcome>,
    waker: Option<Waker>,
    abandoned: bool,
}

/// One call: the `jabba_call` the host allocates, and everything the node may touch until
/// end(). Pinned on the heap (Arc) and kept alive by itself until end().
struct Slot {
    call: UnsafeCell<jabba_call>,
    host: UnsafeCell<jabba_host>,
    inputs: Vec<jabba_value>,
    _scratch: Scratch,
    _args: Box<dyn Any + Send>,
    request_id: String,
    ctx: Ctx,
    inst: Arc<Instance>,
    state: Mutex<SlotState>,
    refs: AtomicI32,
    start_returned: AtomicBool,
    cancel_wanted: AtomicBool,
    cancel_issued: AtomicBool,
    keep: Mutex<Option<Arc<Slot>>>,
}

unsafe impl Send for Slot {}
unsafe impl Sync for Slot {}

impl Slot {
    fn new(
        inst: Arc<Instance>,
        ctx: Ctx,
        inputs: Vec<jabba_value>,
        scratch: Scratch,
        args: Box<dyn Any + Send>,
    ) -> Arc<Slot> {
        let request_id = ctx.request_id().to_owned();
        let slot = Arc::new(Slot {
            call: UnsafeCell::new(jabba_call {
                size: std::mem::size_of::<jabba_call>() as u32,
                inputs: std::ptr::null(),
                n_inputs: 0,
                host: std::ptr::null(),
                complete: Some(complete),
                host_data: std::ptr::null_mut(),
                node_data: std::ptr::null_mut(),
            }),
            host: UnsafeCell::new(jabba_host {
                size: std::mem::size_of::<jabba_host>() as u32,
                self_: std::ptr::null_mut(),
                post: None, // the node uses its own executor
                set_field: Some(set_field),
                request_id: std::ptr::null(),
                request_id_len: 0,
            }),
            inputs,
            _scratch: scratch,
            _args: args,
            request_id,
            ctx,
            inst,
            state: Mutex::new(SlotState::default()),
            refs: AtomicI32::new(2), // "start has not returned" + "complete has not arrived"
            start_returned: AtomicBool::new(false),
            cancel_wanted: AtomicBool::new(false),
            cancel_issued: AtomicBool::new(false),
            keep: Mutex::new(None),
        });
        let me = Arc::as_ptr(&slot) as *mut c_void;
        unsafe {
            let host = &mut *slot.host.get();
            host.self_ = me;
            host.request_id = slot.request_id.as_ptr().cast();
            host.request_id_len = slot.request_id.len();
            let call = &mut *slot.call.get();
            call.inputs = slot.inputs.as_ptr();
            call.n_inputs = slot.inputs.len();
            call.host = slot.host.get();
            call.host_data = me;
        }
        slot
    }

    fn node(&self) -> &jabba_node {
        unsafe { &*self.inst.node }
    }

    fn begin(self: &Arc<Self>) {
        *lock(&self.keep) = Some(self.clone()); // the node may use the call until end()
        let n = self.node();
        unsafe { n.start.expect("start")(n.self_, self.call.get()) };
        self.start_returned.store(true, SeqCst);
        if self.cancel_wanted.load(SeqCst) {
            self.do_cancel();
        }
        self.unref();
    }

    fn request_cancel(&self) {
        self.cancel_wanted.store(true, SeqCst);
        if self.start_returned.load(SeqCst) {
            self.do_cancel();
        }
    }

    fn do_cancel(&self) {
        if self.cancel_issued.swap(true, SeqCst) {
            return;
        }
        // Increment if not zero: keeps end() from running while cancel() does.
        let mut r = self.refs.load(SeqCst);
        loop {
            if r == 0 {
                return;
            }
            match self.refs.compare_exchange(r, r + 1, SeqCst, SeqCst) {
                Ok(_) => break,
                Err(now) => r = now,
            }
        }
        let n = self.node();
        unsafe { n.cancel.expect("cancel")(n.self_, self.call.get()) };
        self.unref();
    }

    fn unref(&self) {
        if self.refs.fetch_sub(1, SeqCst) == 1 {
            let keep = lock(&self.keep).take();
            let n = self.node();
            unsafe { n.end.expect("end")(n.self_, self.call.get()) };
            drop(keep);
        }
    }

    /// The run gave up on this call: a result that arrived, or arrives later, is released.
    fn abandon(&self) {
        {
            let mut st = lock(&self.state);
            st.abandoned = true;
            st.waker = None;
            if let Some(Outcome::Ok(mut v)) = st.outcome.take() {
                unsafe { v.release() };
            }
        }
        self.request_cancel();
    }
}

/// A result's buffer without `release` is valid only during complete(): copy it.
unsafe fn adopt(v: jabba_value) -> jabba_value {
    let elem = match v.base() {
        JABBA_STR | JABBA_BYTES | JABBA_JSON => 1,
        JABBA_F64S | JABBA_I64S => 8,
        JABBA_F32S => 4,
        _ => return v,
    };
    if v.release.is_some() || v.as_.buf.len == 0 || v.as_.buf.data.is_null() {
        return v;
    }
    let bytes = std::slice::from_raw_parts(v.as_.buf.data.cast::<u8>(), v.as_.buf.len * elem);
    // u64 storage keeps the copy aligned for every element type.
    let mut storage = vec![0u64; bytes.len().div_ceil(8)];
    std::ptr::copy_nonoverlapping(
        bytes.as_ptr(),
        storage.as_mut_ptr().cast::<u8>(),
        bytes.len(),
    );
    let holder = Box::new(storage);
    let mut out = jabba_value::buffer(v.kind, holder.as_ptr().cast(), v.as_.buf.len);
    unsafe extern "C" fn drop_storage(owner: *mut c_void) {
        drop(Box::from_raw(owner.cast::<Vec<u64>>()));
    }
    out.owner = Box::into_raw(holder).cast();
    out.release = Some(drop_storage);
    out
}

unsafe extern "C" fn complete(call: *mut jabba_call, result: *const jabba_result) {
    // Panics must not unwind into C.
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let slot = &*((*call).host_data as *const Slot);
        let keep = lock(&slot.keep).clone(); // unref() may drop the node's reference to us
        let outcome = if result.is_null() {
            Outcome::Failed("jabba::ffi: complete() without a result".into())
        } else {
            let r = &*result;
            match r.status {
                JABBA_OK => Outcome::Ok(adopt(r.value)),
                status => {
                    let mut v = r.value;
                    v.release(); // a misbehaving node: still no leak
                    if status == JABBA_CANCELLED {
                        Outcome::Cancelled
                    } else if r.error.is_null() || r.error_len == 0 {
                        Outcome::Failed(String::new())
                    } else {
                        let bytes = std::slice::from_raw_parts(r.error.cast::<u8>(), r.error_len);
                        Outcome::Failed(String::from_utf8_lossy(bytes).into_owned())
                    }
                }
            }
        };
        let waker = {
            let mut st = lock(&slot.state);
            if st.abandoned {
                if let Outcome::Ok(mut v) = outcome {
                    v.release();
                }
                None
            } else {
                st.outcome = Some(outcome);
                st.waker.take()
            }
        };
        // Waking schedules a poll; it never runs the run's code here.
        if let Some(w) = waker {
            w.wake();
        }
        slot.unref();
        drop(keep);
    }));
}

unsafe extern "C" fn set_field(
    host: *mut c_void,
    key: *const c_char,
    key_len: usize,
    value: *const jabba_value,
) {
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let slot = &*(host as *const Slot);
        let Some(key) = items(key.cast::<u8>(), key_len) else {
            return;
        };
        let key = String::from_utf8_lossy(key).into_owned();
        if let Ok(json) = to_json(&*value) {
            slot.ctx.set(key, json); // dropped once the run has returned
        }
    }));
}

/// The future of one foreign call (a node body).
struct ForeignCall<T> {
    slot: Arc<Slot>,
    name: Arc<str>,
    started: bool,
    done: bool,
    _type: PhantomData<fn() -> T>,
}

impl<T> ForeignCall<T> {
    fn new(slot: Arc<Slot>, name: Arc<str>) -> Self {
        ForeignCall {
            slot,
            name,
            started: false,
            done: false,
            _type: PhantomData,
        }
    }
}

impl<T: FfiValue> Future for ForeignCall<T> {
    type Output = Result<T, NodeError>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        if !self.started {
            self.started = true;
            self.slot.begin();
        }
        let outcome = {
            let mut st = lock(&self.slot.state);
            match st.outcome.take() {
                Some(o) => o,
                None => {
                    st.waker = Some(cx.waker().clone());
                    return Poll::Pending;
                }
            }
        };
        self.done = true;
        let name = &self.name;
        Poll::Ready(match outcome {
            Outcome::Ok(mut v) => {
                let declared = self.slot.node().output_kind;
                if !value_fits(v.kind, T::KIND) || !value_fits(v.kind, declared) {
                    let got = v.kind;
                    unsafe { v.release() };
                    Err(NodeError::msg(format!(
                        "foreign node '{name}' returned {}, expected {}",
                        kind_name(got),
                        kind_name(T::KIND)
                    )))
                } else {
                    unsafe { T::take(v) }.map_err(NodeError::msg)
                }
            }
            Outcome::Cancelled => Err(NodeError::msg(format!(
                "foreign node '{name}' was cancelled"
            ))),
            Outcome::Failed(e) if e.is_empty() => {
                Err(NodeError::msg(format!("foreign node '{name}' failed")))
            }
            Outcome::Failed(e) => Err(NodeError::msg(e)),
        })
    }
}

impl<T> Drop for ForeignCall<T> {
    fn drop(&mut self) {
        if self.started && !self.done {
            // Dropped by the run: timeout, deadline, failure or outer cancellation.
            self.slot.abandon();
        }
    }
}

/// The name a foreign node gives itself (for diagnostics).
pub fn foreign_name(f: &Foreign) -> String {
    if !f.inst.compatible {
        return String::new();
    }
    let n = f.node();
    if n.name.is_null() {
        return String::new();
    }
    unsafe { CStr::from_ptr(n.name) }
        .to_string_lossy()
        .into_owned()
}
