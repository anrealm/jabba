//! Executing one run (spec/semantics.md §3–§5).

use std::future::Future;
use std::marker::PhantomData;
use std::pin::{pin, Pin};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::task::{Context, Poll, Wake, Waker};
use std::thread::{self, Thread};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use crate::ctx::{CancelToken, Ctx, RunShared};
use crate::error::{NodeError, RunError};
use crate::graph::{Inner, Source};
use crate::node::{Body, BoxFuture, Value};
use crate::stat::{ErrorKind, NodeStat, NodeStatus, RequestStat, RunStatus};
use crate::timer::{self, TimerKey};

/// Per-run options: `RunOptions::default().deadline(Duration::from_millis(300))`.
#[derive(Debug, Clone, Default)]
#[non_exhaustive]
pub struct RunOptions {
    /// Whole-graph deadline, measured from the first poll.
    pub deadline: Option<Duration>,
    /// Defaults to a generated unique id.
    pub request_id: Option<String>,
}

impl RunOptions {
    pub fn deadline(mut self, deadline: Duration) -> Self {
        self.deadline = Some(deadline);
        self
    }

    pub fn request_id(mut self, request_id: impl Into<String>) -> Self {
        self.request_id = Some(request_id.into());
        self
    }
}

/// A successful run.
#[derive(Debug)]
pub struct RunOk<O> {
    /// The output node's value; `None` for graphs compiled without an output.
    pub output: Option<Arc<O>>,
    pub stat: RequestStat,
}

/// Future of one graph run. Dropping it cancels the run: running nodes are
/// dropped and no record is produced.
#[must_use = "a run does nothing unless it is awaited or polled"]
pub struct RunFuture<O> {
    state: Option<Box<RunState>>,
    _output: PhantomData<fn() -> O>,
}

struct Running {
    idx: usize,
    future: BoxFuture,
    deadline: Option<Instant>,
    cancel: CancelToken,
}

struct RunState {
    graph: Arc<Inner>,
    request: Value,
    shared: Arc<RunShared>,
    deadline: Option<Duration>,
    /// Set on the first poll.
    clock: Option<(Instant, Option<Instant>)>,
    results: Vec<Option<Value>>,
    pending: Vec<usize>,
    started: Vec<Option<Instant>>,
    stats: Vec<Option<NodeStat>>,
    running: Vec<Running>,
    failure: Option<(usize, String)>,
    timer: Option<TimerKey>,
    poisoned: bool,
}

struct Finished {
    status: RunStatus,
    stat: RequestStat,
    output: Option<Value>,
    failure: Option<(String, String)>,
}

impl<O> RunFuture<O> {
    pub(crate) fn new(graph: Arc<Inner>, request: Value, options: RunOptions) -> Self {
        let n = graph.nodes.len();
        let request_id = options.request_id.unwrap_or_else(new_request_id);
        let state = RunState {
            pending: graph.indegree.clone(),
            graph,
            request,
            shared: Arc::new(RunShared::new(request_id)),
            deadline: options.deadline,
            clock: None,
            results: vec![None; n],
            started: vec![None; n],
            stats: vec![None; n],
            running: Vec::new(),
            failure: None,
            timer: None,
            poisoned: false,
        };
        RunFuture {
            state: Some(Box::new(state)),
            _output: PhantomData,
        }
    }
}

impl<O: Send + Sync + 'static> Future for RunFuture<O> {
    type Output = Result<RunOk<O>, RunError>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let state = self
            .state
            .as_mut()
            .expect("RunFuture polled after completion");
        // A node body or a hook that panicked leaves the run half-updated: refuse to go on.
        assert!(!state.poisoned, "RunFuture polled after a panic");
        state.poisoned = true;
        let finished = state.drive(cx);
        state.poisoned = false;
        let Some(finished) = finished else {
            return Poll::Pending;
        };
        // Take the state before the hook runs: even if the hook panics, the run is over.
        let graph = self.state.take().expect("state present").graph;
        if let Some(hook) = &graph.on_run_done {
            hook(&finished.stat);
        }
        let graph = graph.name.to_string();
        let Finished {
            status,
            stat,
            output,
            failure,
        } = finished;
        Poll::Ready(match status {
            RunStatus::Ok => Ok(RunOk {
                output: output.map(|v| {
                    v.downcast::<O>()
                        .unwrap_or_else(|_| unreachable!("output type is checked"))
                }),
                stat,
            }),
            RunStatus::Failed => {
                let (node, error) = failure.expect("a failed run has a failed node");
                Err(RunError::NodeFailed {
                    node,
                    error,
                    stat: Box::new(stat),
                })
            }
            RunStatus::Timeout => Err(RunError::GraphTimeout {
                graph,
                stat: Box::new(stat),
            }),
        })
    }
}

impl<O> Drop for RunFuture<O> {
    fn drop(&mut self) {
        // Cancelled from outside: running nodes are dropped with the state, no record.
        if let Some(state) = self.state.take() {
            for r in &state.running {
                r.cancel.cancel();
            }
            if let Some(key) = state.timer {
                timer::cancel(key);
            }
            state.shared.freeze();
        }
    }
}

impl RunState {
    /// One poll: run ticks until nothing changes. Each tick records completions,
    /// then expired per-node timeouts, then the graph deadline.
    fn drive(&mut self, cx: &mut Context<'_>) -> Option<Finished> {
        if self.clock.is_none() {
            let now = Instant::now();
            // A deadline too far to represent (e.g. Duration::MAX) means no deadline.
            self.clock = Some((now, self.deadline.and_then(|d| now.checked_add(d))));
            self.start_ready();
            if self.failure.is_some() {
                return Some(self.fail_fast());
            }
        }
        loop {
            let mut progressed = false;

            let mut i = 0;
            while i < self.running.len() {
                match self.running[i].future.as_mut().poll(cx) {
                    Poll::Ready(result) => {
                        let r = self.running.swap_remove(i);
                        self.complete(r.idx, result);
                        progressed = true;
                    }
                    Poll::Pending => i += 1,
                }
            }
            if self.failure.is_some() {
                return Some(self.fail_fast());
            }

            let now = Instant::now();
            let mut i = 0;
            while i < self.running.len() {
                if self.running[i].deadline.map_or(true, |at| at > now) {
                    i += 1;
                    continue;
                }
                let r = self.running.swap_remove(i);
                r.cancel.cancel();
                drop(r.future);
                let node = &self.graph.nodes[r.idx];
                let after = secs(node.timeout.expect("a node with a deadline has a timeout"));
                let name = node.name.clone();
                self.record(
                    r.idx,
                    NodeStatus::Timeout,
                    ErrorKind::Timeout,
                    format!("node timeout after {after}s"),
                );
                self.settle_error(r.idx, format!("node '{name}' timed out after {after}s"));
                progressed = true;
            }
            if self.failure.is_some() {
                return Some(self.fail_fast());
            }

            if let Some((_, Some(at))) = self.clock {
                if now >= at {
                    return Some(self.time_out());
                }
            }

            progressed |= self.start_ready();
            if self.failure.is_some() {
                return Some(self.fail_fast());
            }
            if !progressed {
                break;
            }
        }
        if self.running.is_empty() {
            return Some(self.finish(RunStatus::Ok));
        }
        self.arm_timer(cx);
        None
    }

    /// Start every node whose dependencies are done. Synchronous nodes run inline.
    fn start_ready(&mut self) -> bool {
        let graph = self.graph.clone();
        let ready: Vec<usize> = (0..graph.nodes.len())
            .filter(|&i| self.started[i].is_none() && self.pending[i] == 0)
            .collect();
        for &i in &ready {
            let node = &graph.nodes[i];
            let now = Instant::now();
            self.started[i] = Some(now);
            self.stats[i] = Some(NodeStat {
                status: NodeStatus::Ok,
                error_kind: None,
                error: None,
                started_at: Some(wall_clock()),
                duration_ms: None,
            });
            let values = node
                .inputs
                .iter()
                .map(|source| match *source {
                    Source::Request => self.request.clone(),
                    Source::Node(j) => self.results[j]
                        .clone()
                        .expect("a provider finished with a value"),
                })
                .collect();
            let cancel = CancelToken::default();
            let ctx = Ctx::new(self.shared.clone(), cancel.clone());
            match &node.body {
                Body::Async(f) => self.running.push(Running {
                    idx: i,
                    future: f(values, ctx),
                    deadline: node.timeout.and_then(|t| now.checked_add(t)),
                    cancel,
                }),
                Body::Sync(f) => {
                    let result = f(values, &ctx);
                    match node.timeout {
                        // It could not be interrupted, but it did overrun: a timeout, result discarded.
                        Some(t) if now.elapsed() > t => {
                            cancel.cancel();
                            let after = secs(t);
                            let message = format!("node timeout after {after}s");
                            self.record(i, NodeStatus::Timeout, ErrorKind::Timeout, message);
                            let name = &node.name;
                            self.settle_error(i, format!("node '{name}' timed out after {after}s"));
                        }
                        _ => self.complete(i, result),
                    }
                }
            }
        }
        !ready.is_empty()
    }

    fn complete(&mut self, i: usize, result: Result<Value, NodeError>) {
        match result {
            Ok(value) => {
                self.record_ok(i);
                self.results[i] = Some(value);
                self.release(i);
            }
            Err(error) => {
                let message = error.message().to_owned();
                self.record(i, NodeStatus::Failed, ErrorKind::Failed, message.clone());
                self.settle_error(i, message);
            }
        }
    }

    /// A failed or timed-out node: optional ones degrade to their default, others fail the run.
    fn settle_error(&mut self, i: usize, message: String) {
        let node = &self.graph.nodes[i];
        if node.optional {
            self.results[i] = node.default.clone();
            self.release(i);
        } else if self.failure.is_none() {
            self.failure = Some((i, message));
        }
    }

    fn release(&mut self, i: usize) {
        let graph = self.graph.clone();
        for &child in &graph.children[i] {
            self.pending[child] -= 1;
        }
    }

    fn record_ok(&mut self, i: usize) {
        let duration = self.elapsed_ms(i);
        let stat = self.stats[i].as_mut().expect("a started node has a stat");
        stat.duration_ms = Some(duration);
    }

    fn record(&mut self, i: usize, status: NodeStatus, kind: ErrorKind, error: String) {
        let duration = self.elapsed_ms(i);
        let stat = self.stats[i].as_mut().expect("a started node has a stat");
        stat.status = status;
        stat.error_kind = Some(kind);
        stat.error = Some(error);
        stat.duration_ms = Some(duration);
    }

    fn elapsed_ms(&self, i: usize) -> f64 {
        self.started[i].map_or(0.0, |t| t.elapsed().as_secs_f64() * 1000.0)
    }

    fn fail_fast(&mut self) -> Finished {
        for r in std::mem::take(&mut self.running) {
            r.cancel.cancel();
            self.record(
                r.idx,
                NodeStatus::Skipped,
                ErrorKind::Cancelled,
                "cancelled".into(),
            );
        }
        self.finish(RunStatus::Failed)
    }

    fn time_out(&mut self) -> Finished {
        let after = secs(self.deadline.expect("a run with a deadline"));
        for r in std::mem::take(&mut self.running) {
            r.cancel.cancel();
            self.record(
                r.idx,
                NodeStatus::Timeout,
                ErrorKind::Deadline,
                format!("graph deadline after {after}s"),
            );
        }
        self.finish(RunStatus::Timeout)
    }

    fn finish(&mut self, status: RunStatus) -> Finished {
        if let Some(key) = self.timer.take() {
            timer::cancel(key);
        }
        let started = self.clock.expect("finished after the first poll").0;
        let graph = self.graph.clone();
        let nodes = graph
            .nodes
            .iter()
            .zip(&mut self.stats)
            .map(|(node, stat)| {
                (
                    node.name.to_string(),
                    stat.take().unwrap_or_else(NodeStat::skipped),
                )
            })
            .collect();
        let stat = RequestStat {
            request_id: self.shared.request_id.clone(),
            graph: graph.name.to_string(),
            status,
            duration_ms: started.elapsed().as_secs_f64() * 1000.0,
            nodes,
            fields: self.shared.freeze(),
        };
        let output = match status {
            RunStatus::Ok => graph.output.and_then(|i| self.results[i].clone()),
            _ => None,
        };
        let failure = self
            .failure
            .take()
            .map(|(i, error)| (graph.nodes[i].name.to_string(), error));
        Finished {
            status,
            stat,
            output,
            failure,
        }
    }

    /// One timer registration per run, at the nearest node timeout or the deadline.
    fn arm_timer(&mut self, cx: &Context<'_>) {
        let graph_deadline = self.clock.and_then(|(_, at)| at);
        let wake_at = self
            .running
            .iter()
            .filter_map(|r| r.deadline)
            .chain(graph_deadline)
            .min();
        if let Some(key) = self.timer.take() {
            timer::cancel(key);
        }
        if let Some(at) = wake_at {
            self.timer = Some(timer::register(at, cx.waker().clone()));
        }
    }
}

fn secs(d: Duration) -> f64 {
    d.as_secs_f64()
}

fn wall_clock() -> f64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0.0, |d| d.as_secs_f64())
}

fn new_request_id() -> String {
    static COUNTER: AtomicU64 = AtomicU64::new(0);
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.as_nanos() as u64);
    let n = COUNTER.fetch_add(1, Ordering::Relaxed);
    format!("{nanos:016x}{:08x}{:08x}", std::process::id(), n as u32)
}

/// Run a future to completion on the current thread. A minimal executor for
/// programs (and tests) that have no async runtime of their own.
pub fn block_on<F: Future>(future: F) -> F::Output {
    struct Unpark(Thread);
    impl Wake for Unpark {
        fn wake(self: Arc<Self>) {
            self.0.unpark();
        }
    }
    let waker = Waker::from(Arc::new(Unpark(thread::current())));
    let mut cx = Context::from_waker(&waker);
    let mut future = pin!(future);
    loop {
        if let Poll::Ready(output) = future.as_mut().poll(&mut cx) {
            return output;
        }
        thread::park();
    }
}

impl<O> std::fmt::Debug for RunFuture<O> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let graph = self.state.as_ref().map(|s| s.graph.name.clone());
        f.debug_struct("RunFuture")
            .field("graph", &graph)
            .field("finished", &self.state.is_none())
            .finish()
    }
}
