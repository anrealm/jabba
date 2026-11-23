//! Exporting a Rust node for graphs in other languages: the node side of spec/ffi.md §3.

use std::ffi::{c_void, CString};
use std::future::Future;
use std::pin::Pin;
use std::sync::atomic::{AtomicBool, AtomicU8, Ordering::SeqCst};
use std::sync::{Arc, Mutex};
use std::task::{Context, Poll, Wake, Waker};

use super::pool;
use super::sys::*;
use super::value::FfiValue;
use crate::ctx::{lock, CancelToken, Ctx, RunShared};
use crate::error::NodeError;
use crate::json::Json;
use crate::node::DynInputs;

type BoxFuture = Pin<Box<dyn Future<Output = Result<jabba_value, String>> + Send>>;
type Body = Box<dyn Fn(&[jabba_value], Ctx) -> Result<BoxFuture, String> + Send + Sync>;

/// Inputs of an exported node, decoded from the ABI: `()` or a tuple of [`FfiValue`]s.
pub trait ExportArgs: Sized + Send + 'static {
    #[doc(hidden)]
    fn kinds() -> Vec<u32>;
    #[doc(hidden)]
    unsafe fn decode(inputs: &[jabba_value]) -> Result<Self, String>;
}

impl ExportArgs for () {
    fn kinds() -> Vec<u32> {
        Vec::new()
    }
    unsafe fn decode(_: &[jabba_value]) -> Result<(), String> {
        Ok(())
    }
}

macro_rules! export_tuple {
    ($($t:ident $i:tt),+) => {
        impl<$($t: FfiValue),+> ExportArgs for ($($t,)+) {
            fn kinds() -> Vec<u32> {
                vec![$($t::KIND),+]
            }
            unsafe fn decode(inputs: &[jabba_value]) -> Result<Self, String> {
                Ok(($($t::read(&inputs[$i])?,)+))
            }
        }
    };
}

export_tuple!(A 0);
export_tuple!(A 0, B 1);
export_tuple!(A 0, B 1, C 2);
export_tuple!(A 0, B 1, C 2, D 3);
export_tuple!(A 0, B 1, C 2, D 3, E 4);
export_tuple!(A 0, B 1, C 2, D 3, E 4, F 5);
export_tuple!(A 0, B 1, C 2, D 3, E 4, F 5, G 6);
export_tuple!(A 0, B 1, C 2, D 3, E 4, F 5, G 6, H 7);

/// Export an async Rust node: each call returns a new instance (`release` it once).
///
/// ```ignore
/// #[no_mangle]
/// pub extern "C" fn score_node() -> *const jabba::ffi::sys::jabba_node {
///     jabba::ffi::export("score", |(features,): (Vec<f64>,), _ctx| async move {
///         Ok(features.iter().sum::<f64>())
///     })
/// }
/// ```
/// The body starts inside the host's call and runs until it first waits — keep that part
/// short. `ctx.set` writes into the host's record; the host's cancellation drops the body
/// (and cancels `ctx.cancel_token()`).
pub fn export<A, T, F, Fut>(name: &str, f: F) -> *const jabba_node
where
    A: ExportArgs,
    T: FfiValue,
    F: Fn(A, Ctx) -> Fut + Send + Sync + 'static,
    Fut: Future<Output = Result<T, NodeError>> + Send + 'static,
{
    let body: Body = Box::new(move |inputs, ctx| {
        let args = unsafe { A::decode(inputs) }?;
        let fut = f(args, ctx);
        Ok(Box::pin(async move {
            fut.await
                .map(T::into_value)
                .map_err(|e| e.message().to_owned())
        }))
    });
    ExportedNode::expose(name, A::kinds(), T::KIND, body, false)
}

/// Export a synchronous Rust node. Its body never runs inside the host's call: always on
/// the host's executor (or ours).
pub fn export_sync<A, T, F>(name: &str, f: F) -> *const jabba_node
where
    A: ExportArgs,
    T: FfiValue,
    F: Fn(A, &Ctx) -> Result<T, NodeError> + Send + Sync + 'static,
{
    let f = Arc::new(f);
    let body: Body = Box::new(move |inputs, ctx| {
        let args = unsafe { A::decode(inputs) }?;
        let f = f.clone();
        Ok(Box::pin(async move {
            f(args, &ctx)
                .map(T::into_value)
                .map_err(|e| e.message().to_owned())
        }))
    });
    ExportedNode::expose(name, A::kinds(), T::KIND, body, true)
}

/// Export an async node whose inputs are named, all of type `V`.
pub fn export_dyn<V, T, F, Fut>(name: &str, inputs: &[&str], f: F) -> *const jabba_node
where
    V: FfiValue,
    T: FfiValue,
    F: Fn(DynInputs<V>, Ctx) -> Fut + Send + Sync + 'static,
    Fut: Future<Output = Result<T, NodeError>> + Send + 'static,
{
    let names: Vec<Arc<str>> = inputs.iter().map(|n| Arc::from(*n)).collect();
    let kinds = vec![V::KIND; names.len()];
    let body: Body = Box::new(move |values, ctx| {
        let mut items = Vec::with_capacity(names.len());
        for (name, v) in names.iter().zip(values) {
            items.push((name.clone(), Arc::new(unsafe { V::read(v) }?)));
        }
        let fut = f(DynInputs::from_parts(items), ctx);
        Ok(Box::pin(async move {
            fut.await
                .map(T::into_value)
                .map_err(|e| e.message().to_owned())
        }))
    });
    ExportedNode::expose(name, kinds, T::KIND, body, false)
}

struct ExportedNode {
    abi: jabba_node,
    _name: CString,
    kinds: Vec<u32>,
    body: Body,
    sync: bool,
    self_ref: Mutex<Option<Arc<ExportedNode>>>, // the reference held by whoever took the instance
}

unsafe impl Send for ExportedNode {}
unsafe impl Sync for ExportedNode {}

impl ExportedNode {
    fn expose(
        name: &str,
        kinds: Vec<u32>,
        output: u32,
        body: Body,
        sync: bool,
    ) -> *const jabba_node {
        let cname = CString::new(name.replace('\0', "")).expect("no NUL");
        let mut node = Arc::new(ExportedNode {
            abi: jabba_node {
                abi: JABBA_FFI_ABI,
                size: std::mem::size_of::<jabba_node>() as u32,
                self_: std::ptr::null_mut(),
                name: cname.as_ptr(),
                output_kind: output,
                n_inputs: kinds.len(),
                input_kinds: kinds.as_ptr(),
                start: Some(start),
                cancel: Some(cancel),
                end: Some(end),
                release: Some(release),
            },
            _name: cname,
            kinds,
            body,
            sync,
            self_ref: Mutex::new(None),
        });
        // The struct does not move inside the Arc: point it at itself.
        let me = Arc::get_mut(&mut node).expect("a fresh Arc");
        me.abi.self_ = (me as *mut ExportedNode).cast();
        let abi: *const jabba_node = &me.abi;
        *lock(&node.self_ref) = Some(node.clone());
        abi
    }
}

/// The body's view of its host: cut before complete() and at end(), so that copies of the
/// Ctx outliving the call never reach it.
struct HostLink {
    host: Mutex<*const jabba_host>,
}

unsafe impl Send for HostLink {}
unsafe impl Sync for HostLink {}

impl HostLink {
    fn cut(&self) {
        *lock(&self.host) = std::ptr::null();
    }
}

const IDLE: u8 = 0;
const SCHEDULED: u8 = 1;
const RUNNING: u8 = 2;
const NOTIFIED: u8 = 3;
const DONE: u8 = 4;

/// One call of an exported node: drives its future. References: the host's (until end) and
/// whatever is scheduled or polling.
struct Task {
    call: *mut jabba_call,
    post: Option<PostFn>,
    post_self: *mut c_void,
    future: Mutex<Option<BoxFuture>>,
    state: AtomicU8,
    cancelled: AtomicBool,
    cancel: CancelToken,
    link: Arc<HostLink>,
    _node: Arc<ExportedNode>,
}

unsafe impl Send for Task {}
unsafe impl Sync for Task {}

impl Wake for Task {
    fn wake(self: Arc<Self>) {
        self.schedule();
    }
    fn wake_by_ref(self: &Arc<Self>) {
        self.schedule();
    }
}

impl Task {
    /// Make sure a poll follows. Never after complete (DONE): no post after it.
    fn schedule(self: &Arc<Self>) {
        loop {
            match self.state.load(SeqCst) {
                IDLE => {
                    if self
                        .state
                        .compare_exchange(IDLE, SCHEDULED, SeqCst, SeqCst)
                        .is_ok()
                    {
                        self.post_poll();
                        return;
                    }
                }
                RUNNING => {
                    if self
                        .state
                        .compare_exchange(RUNNING, NOTIFIED, SeqCst, SeqCst)
                        .is_ok()
                    {
                        return;
                    }
                }
                _ => return, // SCHEDULED, NOTIFIED, DONE
            }
        }
    }

    fn post_poll(self: &Arc<Self>) {
        match self.post {
            Some(post) => {
                unsafe extern "C" fn run_posted(arg: *mut c_void) {
                    let task = Arc::from_raw(arg as *const Task);
                    quietly(|| task.run());
                }
                let arg = Arc::into_raw(self.clone()) as *mut c_void;
                unsafe { post(self.post_self, Some(run_posted), arg) };
            }
            None => {
                let task = self.clone();
                pool::post(Box::new(move || quietly(|| task.run())));
            }
        }
    }

    /// Poll until the future is pending with nobody having woken it, or done.
    fn run(self: Arc<Self>) {
        self.state.store(RUNNING, SeqCst);
        let waker = Waker::from(self.clone());
        let mut cx = Context::from_waker(&waker);
        loop {
            let outcome = {
                let mut slot = lock(&self.future);
                if self.cancelled.load(SeqCst) {
                    // cancellation drops the body (on this thread, not the host's)
                    let body = slot.take();
                    quietly(move || drop(body));
                    Some(Err(None))
                } else if let Some(fut) = slot.as_mut() {
                    let polled = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                        fut.as_mut().poll(&mut cx)
                    }));
                    match polled {
                        Ok(Poll::Pending) => None,
                        Ok(Poll::Ready(r)) => {
                            let body = slot.take();
                            quietly(move || drop(body));
                            Some(r.map_err(Some))
                        }
                        Err(_) => {
                            let body = slot.take();
                            quietly(move || drop(body));
                            Some(Err(Some("the node panicked".to_owned())))
                        }
                    }
                } else {
                    return; // already completed
                }
            };
            if let Some(result) = outcome {
                self.finish(result);
                return;
            }
            if self
                .state
                .compare_exchange(RUNNING, IDLE, SeqCst, SeqCst)
                .is_ok()
            {
                return;
            }
            self.state.store(RUNNING, SeqCst); // woken while polling: go again
        }
    }

    /// Err(None) = cancelled.
    fn finish(&self, result: Result<jabba_value, Option<String>>) {
        self.state.store(DONE, SeqCst); // first: nothing may be posted after complete
        self.link.cut(); // nor any set_field
        let mut r = jabba_result {
            size: std::mem::size_of::<jabba_result>() as u32,
            status: JABBA_OK,
            value: jabba_value::null(),
            error: std::ptr::null(),
            error_len: 0,
        };
        let message;
        match result {
            Ok(v) => r.value = v,
            Err(None) => r.status = JABBA_CANCELLED,
            Err(Some(e)) if self.cancelled.load(SeqCst) || self.cancel.is_cancelled() => {
                drop(e);
                r.status = JABBA_CANCELLED; // it failed because it was asked to stop
            }
            Err(Some(e)) => {
                message = e;
                r.status = JABBA_FAILED;
                r.error = message.as_ptr().cast();
                r.error_len = message.len();
            }
        }
        unsafe {
            let call = self.call;
            (*call).complete.expect("complete")(call, &r); // after this, no touching the call
        }
    }
}

fn failed(call: *mut jabba_call, message: &str) {
    let r = jabba_result {
        size: std::mem::size_of::<jabba_result>() as u32,
        status: JABBA_FAILED,
        value: jabba_value::null(),
        error: message.as_ptr().cast(),
        error_len: message.len(),
    };
    unsafe { (*call).complete.expect("complete")(call, &r) };
}

unsafe extern "C" fn start(node: *mut c_void, call: *mut jabba_call) {
    (*call).node_data = std::ptr::null_mut(); // set once the call is handed to a task
    let started = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let node = &*(node as *const ExportedNode);
        let node_arc = lock(&node.self_ref)
            .clone()
            .expect("start() after release()");
        let c = &*call;
        let host = &*c.host;
        let inputs = items(c.inputs, c.n_inputs).unwrap_or_default();
        if inputs.len() != node.kinds.len() {
            return failed(
                call,
                &format!("{} inputs, expected {}", inputs.len(), node.kinds.len()),
            );
        }
        for (i, (v, &k)) in inputs.iter().zip(&node.kinds).enumerate() {
            if !value_fits(v.kind, k) {
                return failed(
                    call,
                    &format!(
                        "input {i} is {}, expected {}",
                        kind_name(v.kind),
                        kind_name(k)
                    ),
                );
            }
        }
        let request_id = items(host.request_id.cast::<u8>(), host.request_id_len)
            .map(|b| String::from_utf8_lossy(b).into_owned())
            .unwrap_or_default();
        let link = Arc::new(HostLink {
            host: Mutex::new(c.host),
        });
        let forward_link = link.clone();
        let shared = Arc::new(RunShared::forwarding(
            request_id,
            Box::new(move |key: &str, value: &Json| {
                // Held while calling the host: end() may wait for an in-flight set_field.
                let host = lock(&forward_link.host);
                if host.is_null() {
                    return;
                }
                let h = &**host;
                if let Some(set_field) = h.set_field {
                    let text = value.to_string();
                    let v = jabba_value::buffer(JABBA_JSON, text.as_ptr().cast(), text.len());
                    set_field(h.self_, key.as_ptr().cast(), key.len(), &v);
                }
            }),
        ));
        let cancel = CancelToken::default();
        let ctx = Ctx::new(shared, cancel.clone());
        let future = match (node.body)(inputs, ctx) {
            Ok(f) => f,
            Err(e) => {
                return failed(call, &format!("bad input: {e}"));
            }
        };
        let task = Arc::new(Task {
            call,
            post: host.post,
            post_self: host.self_,
            future: Mutex::new(Some(future)),
            state: AtomicU8::new(IDLE),
            cancelled: AtomicBool::new(false),
            cancel,
            link,
            _node: node_arc,
        });
        (*call).node_data = Arc::into_raw(task.clone()) as *mut c_void; // the host's reference, until end()
        if node.sync {
            task.schedule(); // a sync body never runs inside start()
        } else {
            task.state.store(SCHEDULED, SeqCst);
            task.run(); // the first poll, inline: cheap, and completes at once when it can
        }
    }));
    // A panic before the task took the call (in the body's synchronous part): nobody
    // else will complete it. After that, the task completes it whatever happens.
    if started.is_err() && (*call).node_data.is_null() {
        quietly(|| failed(call, "the node panicked"));
    }
}

unsafe extern "C" fn cancel(_node: *mut c_void, call: *mut jabba_call) {
    quietly(|| do_cancel(call));
}

unsafe fn do_cancel(call: *mut jabba_call) {
    let data = (*call).node_data;
    if data.is_null() {
        return;
    }
    let task = &*(data as *const Task);
    task.cancelled.store(true, SeqCst);
    task.cancel.cancel();
    // A poll follows (via the host's post, which is valid until end); it drops the body
    // and completes. After complete this is a no-op: schedule() sees DONE.
    Arc::increment_strong_count(data as *const Task);
    let task = Arc::from_raw(data as *const Task);
    task.schedule();
}

unsafe extern "C" fn end(_node: *mut c_void, call: *mut jabba_call) {
    let data = (*call).node_data;
    if data.is_null() {
        return;
    }
    let task = Arc::from_raw(data as *const Task);
    task.link.cut();
    quietly(move || drop(task)); // may drop the node and its body
}

unsafe extern "C" fn release(node: *mut c_void) {
    let node = &*(node as *const ExportedNode);
    let me = lock(&node.self_ref).take();
    quietly(move || drop(me)); // the node lives on while calls hold it
}

/// Run `f`, swallowing a panic: nothing may unwind into the host or kill a pool thread.
fn quietly(f: impl FnOnce()) {
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(f));
}
