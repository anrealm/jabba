use std::future::Future;
use std::pin::Pin;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::task::{Context, Poll, Waker};

use crate::json::Json;

pub(crate) fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    // Poisoning is irrelevant here: every critical section leaves the data consistent.
    m.lock().unwrap_or_else(|e| e.into_inner())
}

/// State shared by every node of one run.
pub(crate) struct RunShared {
    pub(crate) request_id: String,
    fields: Mutex<Fields>,
}

#[derive(Default)]
struct Fields {
    frozen: bool,
    items: Vec<(String, Json)>,
}

impl RunShared {
    pub(crate) fn new(request_id: String) -> Self {
        RunShared {
            request_id,
            fields: Mutex::new(Fields::default()),
        }
    }

    /// Freeze the fields and return their final value; later writes are dropped.
    pub(crate) fn freeze(&self) -> Vec<(String, Json)> {
        let mut fields = lock(&self.fields);
        fields.frozen = true;
        fields.items.clone()
    }
}

/// Request-scoped context passed to every node.
#[derive(Clone)]
pub struct Ctx {
    shared: Arc<RunShared>,
    cancel: CancelToken,
}

impl Ctx {
    pub(crate) fn new(shared: Arc<RunShared>, cancel: CancelToken) -> Self {
        Ctx { shared, cancel }
    }

    pub fn request_id(&self) -> &str {
        &self.shared.request_id
    }

    /// Write a field into the run's record. Last write wins; writes after the
    /// run has returned are dropped.
    pub fn set(&self, key: impl Into<String>, value: impl Into<Json>) {
        let mut fields = lock(&self.shared.fields);
        if fields.frozen {
            return;
        }
        let (key, value) = (key.into(), value.into());
        match fields.items.iter_mut().find(|(k, _)| *k == key) {
            Some(slot) => slot.1 = value,
            None => fields.items.push((key, value)),
        }
    }

    /// Cancelled when jabba stops waiting for this node. Work the node handed
    /// to other threads should watch it.
    pub fn cancel_token(&self) -> &CancelToken {
        &self.cancel
    }
}

/// A cooperative cancellation signal.
#[derive(Clone, Default)]
pub struct CancelToken {
    inner: Arc<CancelInner>,
}

#[derive(Default)]
struct CancelInner {
    cancelled: AtomicBool,
    wakers: Mutex<Vec<Waker>>,
}

impl CancelToken {
    pub fn is_cancelled(&self) -> bool {
        self.inner.cancelled.load(Ordering::Acquire)
    }

    /// Resolves once the token is cancelled.
    pub fn cancelled(&self) -> Cancelled {
        Cancelled {
            token: self.clone(),
        }
    }

    pub(crate) fn cancel(&self) {
        self.inner.cancelled.store(true, Ordering::Release);
        let wakers = std::mem::take(&mut *lock(&self.inner.wakers));
        for waker in wakers {
            waker.wake();
        }
    }
}

/// Future returned by [`CancelToken::cancelled`].
#[must_use = "futures do nothing unless awaited"]
pub struct Cancelled {
    token: CancelToken,
}

impl Future for Cancelled {
    type Output = ();

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        if self.token.is_cancelled() {
            return Poll::Ready(());
        }
        let mut wakers = lock(&self.token.inner.wakers);
        if self.token.is_cancelled() {
            return Poll::Ready(());
        }
        if !wakers.iter().any(|w| w.will_wake(cx.waker())) {
            wakers.push(cx.waker().clone());
        }
        Poll::Pending
    }
}

impl std::fmt::Debug for Ctx {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Ctx")
            .field("request_id", &self.shared.request_id)
            .field("cancelled", &self.cancel.is_cancelled())
            .finish()
    }
}

impl std::fmt::Debug for CancelToken {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("CancelToken")
            .field("cancelled", &self.is_cancelled())
            .finish()
    }
}

impl std::fmt::Debug for Cancelled {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_tuple("Cancelled").field(&self.token).finish()
    }
}
