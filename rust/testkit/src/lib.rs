//! The Rust testkit (ffi/testing/jabba_testkit.h): scripted conformance nodes, exported
//! over the ABI with `jabba::ffi`.

#![allow(clippy::missing_safety_doc)]

use std::collections::HashMap;
use std::future::Future;
use std::pin::Pin;
use std::sync::{Arc, Mutex, MutexGuard};
use std::task::{Context, Poll, Waker};
use std::thread;
use std::time::Duration;

use jabba::ffi::sys::jabba_node;
use jabba::{Ctx, DynInputs, Json, NodeError};

fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|e| e.into_inner())
}

#[derive(Default)]
struct Session {
    events: Mutex<Vec<String>>,
    signals: Mutex<HashMap<String, (bool, Vec<Waker>)>>,
}

impl Session {
    fn event(&self, e: String) {
        lock(&self.events).push(e);
    }

    fn signal(&self, name: &str) {
        let wakers = {
            let mut signals = lock(&self.signals);
            let entry = signals.entry(name.to_owned()).or_default();
            entry.0 = true;
            std::mem::take(&mut entry.1)
        };
        wakers.into_iter().for_each(Waker::wake);
    }
}

struct WaitSignal {
    session: Arc<Session>,
    name: String,
}

impl Future for WaitSignal {
    type Output = ();
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        let mut signals = lock(&self.session.signals);
        let entry = signals.entry(self.name.clone()).or_default();
        if entry.0 {
            return Poll::Ready(());
        }
        entry.1.push(cx.waker().clone());
        Poll::Pending
    }
}

/// Completes when a background thread finishes; dropping it does not stop the thread.
struct OnThread<T> {
    slot: Arc<Mutex<(Option<T>, Option<Waker>)>>,
}

fn on_thread<T: Send + 'static>(work: impl FnOnce() -> T + Send + 'static) -> OnThread<T> {
    let slot = Arc::new(Mutex::new((None, None::<Waker>)));
    let shared = slot.clone();
    thread::spawn(move || {
        let value = work();
        let waker = {
            let mut s = lock(&shared);
            s.0 = Some(value);
            s.1.take()
        };
        if let Some(w) = waker {
            w.wake();
        }
    });
    OnThread { slot }
}

impl<T> Future for OnThread<T> {
    type Output = T;
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<T> {
        let mut s = lock(&self.slot);
        match s.0.take() {
            Some(v) => Poll::Ready(v),
            None => {
                s.1 = Some(cx.waker().clone());
                Poll::Pending
            }
        }
    }
}

struct Behavior {
    name: String,
    raw: Json,
}

impl Behavior {
    fn str(&self, key: &str) -> Option<String> {
        match self.raw.get(key) {
            Some(Json::Str(s)) => Some(s.clone()),
            _ => None,
        }
    }

    fn flag(&self, key: &str) -> bool {
        matches!(self.raw.get(key), Some(Json::Bool(true)))
    }

    fn sleep(&self) -> Option<Duration> {
        let ms = match self.raw.get("sleep_ms")? {
            Json::Int(i) => *i as f64,
            Json::Float(f) => *f,
            _ => return None,
        };
        Some(Duration::from_secs_f64(ms / 1000.0))
    }

    /// The part after signal/wait_for and the sleep: set_field, end event, fail or value.
    fn tail(
        &self,
        session: &Session,
        ctx: &Ctx,
        inputs: &DynInputs<Json>,
    ) -> Result<Json, NodeError> {
        if let Some(Json::Object(fields)) = self.raw.get("set_field") {
            for (k, v) in fields {
                ctx.set(k.clone(), v.clone());
            }
        }
        session.event(format!("end:{}", self.name));
        if let Some(message) = self.str("fail") {
            return Err(NodeError::msg(message));
        }
        if self.flag("echo_inputs") {
            let data = inputs.iter().filter(|(n, _)| *n != jabba::REQUEST);
            return Ok(Json::Object(
                data.map(|(n, v)| (n.to_owned(), (**v).clone())).collect(),
            ));
        }
        if self.flag("echo_request") {
            return Ok(inputs
                .get(jabba::REQUEST)
                .map_or(Json::Null, |v| (**v).clone()));
        }
        Ok(self.raw.get("value").cloned().unwrap_or(Json::Null))
    }
}

async fn run_behavior(
    b: Arc<Behavior>,
    session: Arc<Session>,
    inputs: DynInputs<Json>,
    ctx: Ctx,
) -> Result<Json, NodeError> {
    session.event(format!("start:{}", b.name));
    if let Some(s) = b.str("signal") {
        session.signal(&s);
    }
    if let Some(name) = b.str("wait_for") {
        WaitSignal {
            session: session.clone(),
            name,
        }
        .await;
    }
    let d = b.sleep().unwrap_or_default();
    if b.flag("ignore_cancel") {
        // Cancellation drops the body; work that ignores it lives on a thread.
        return on_thread(move || {
            thread::sleep(d);
            b.tail(&session, &ctx, &inputs)
        })
        .await;
    }
    if !d.is_zero() {
        on_thread(move || thread::sleep(d)).await;
    }
    b.tail(&session, &ctx, &inputs)
}

/// `jabba_testkit_session` in C.
pub struct TestkitSession {
    session: Arc<Session>,
}

fn node(session: &Arc<Session>, spec: &str) -> Option<*const jabba_node> {
    let spec = Json::parse(spec).ok()?;
    let name = match spec.get("name")? {
        Json::Str(s) => s.clone(),
        _ => return None,
    };
    let deps: Vec<String> = match spec.get("deps") {
        Some(Json::Array(a)) => a
            .iter()
            .map(|d| match d {
                Json::Str(s) => Some(s.clone()),
                _ => None,
            })
            .collect::<Option<_>>()?,
        _ => Vec::new(),
    };
    let behavior = Arc::new(Behavior {
        name: name.clone(),
        raw: spec
            .get("behavior")
            .cloned()
            .unwrap_or(Json::Object(Vec::new())),
    });
    let session = session.clone();
    let deps: Vec<&str> = deps.iter().map(String::as_str).collect();
    Some(jabba::ffi::export_dyn::<Json, Json, _, _>(
        &name,
        &deps,
        move |inputs, ctx| run_behavior(behavior.clone(), session.clone(), inputs, ctx),
    ))
}

#[no_mangle]
pub extern "C" fn jabba_testkit_session_new() -> *mut TestkitSession {
    Box::into_raw(Box::new(TestkitSession {
        session: Arc::default(),
    }))
}

#[no_mangle]
pub unsafe extern "C" fn jabba_testkit_session_free(session: *mut TestkitSession) {
    if !session.is_null() {
        drop(Box::from_raw(session));
    }
}

#[no_mangle]
pub unsafe extern "C" fn jabba_testkit_node(
    session: *mut TestkitSession,
    spec: *const std::ffi::c_char,
    spec_len: usize,
) -> *const jabba_node {
    let bytes: &[u8] = if spec_len == 0 || spec.is_null() {
        &[]
    } else {
        std::slice::from_raw_parts(spec.cast::<u8>(), spec_len)
    };
    let made = std::panic::catch_unwind(|| {
        let spec = std::str::from_utf8(bytes).ok()?;
        let session = &*session;
        node(&session.session, spec)
    });
    made.ok().flatten().unwrap_or(std::ptr::null())
}

#[no_mangle]
pub unsafe extern "C" fn jabba_testkit_events(
    session: *mut TestkitSession,
    out: *mut std::ffi::c_char,
    cap: usize,
) -> usize {
    let session = &*session;
    let events = lock(&session.session.events).clone();
    let text = Json::Array(events.into_iter().map(Json::Str).collect()).to_string();
    if cap > 0 {
        std::ptr::copy_nonoverlapping(text.as_ptr(), out.cast::<u8>(), cap.min(text.len()));
    }
    text.len()
}
