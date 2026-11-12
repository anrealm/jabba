//! Runs the language-neutral conformance cases from spec/cases against the Rust core.

use std::collections::{HashMap, HashSet};
use std::future::Future;
use std::path::PathBuf;
use std::pin::Pin;
use std::sync::{Arc, Mutex};
use std::task::{Context, Poll, Waker};
use std::thread;
use std::time::{Duration, Instant};

use jabba::{
    block_on, node, CompileError, Ctx, DynInputs, Graph, GraphBuilder, Json, NodeError, RunError,
    RunOptions,
};
use serde_json::Value as J;

const LANGUAGE: &str = "rust";
const CAPABILITIES: &[&str] = &["ignore_cancel"];

/// spec/ next to rust/ in a checkout; JABBA_SPEC_DIR overrides it when vendored elsewhere.
fn spec_dir() -> PathBuf {
    std::env::var_os("JABBA_SPEC_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../spec"))
}

fn load_cases() -> Vec<(String, J)> {
    let mut paths: Vec<PathBuf> = std::fs::read_dir(spec_dir().join("cases"))
        .expect("spec/cases exists")
        .map(|e| e.expect("readable dir entry").path())
        .filter(|p| p.extension().is_some_and(|e| e == "json"))
        .collect();
    paths.sort();
    let mut cases = Vec::new();
    for path in paths {
        let file = path.file_name().unwrap().to_string_lossy().to_string();
        let content: J = serde_json::from_str(&std::fs::read_to_string(&path).unwrap()).unwrap();
        for case in content["cases"].as_array().expect("a cases array") {
            cases.push((file.clone(), case.clone()));
        }
    }
    cases
}

// ---- what scripted nodes did ------------------------------------------------

#[derive(Default)]
struct Trace {
    events: Mutex<Vec<String>>,
    signals: Mutex<HashMap<String, (bool, Vec<Waker>)>>,
}

impl Trace {
    fn event(&self, e: String) {
        self.events.lock().unwrap().push(e);
    }

    fn signal(&self, name: &str) {
        let wakers = {
            let mut signals = self.signals.lock().unwrap();
            let entry = signals.entry(name.to_owned()).or_default();
            entry.0 = true;
            std::mem::take(&mut entry.1)
        };
        wakers.into_iter().for_each(Waker::wake);
    }

    fn wait(self: &Arc<Self>, name: &str) -> WaitSignal {
        WaitSignal {
            trace: self.clone(),
            name: name.to_owned(),
        }
    }
}

struct WaitSignal {
    trace: Arc<Trace>,
    name: String,
}

impl Future for WaitSignal {
    type Output = ();
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        let mut signals = self.trace.signals.lock().unwrap();
        let entry = signals.entry(self.name.clone()).or_default();
        if entry.0 {
            return Poll::Ready(());
        }
        entry.1.push(cx.waker().clone());
        Poll::Pending
    }
}

/// Completes when a background thread finishes; dropping it does not stop the thread.
struct Thread<T> {
    slot: Arc<Mutex<(Option<T>, Option<Waker>)>>,
}

fn on_thread<T: Send + 'static>(work: impl FnOnce() -> T + Send + 'static) -> Thread<T> {
    let slot = Arc::new(Mutex::new((None, None::<Waker>)));
    let shared = slot.clone();
    thread::spawn(move || {
        let value = work();
        let waker = {
            let mut s = shared.lock().unwrap();
            s.0 = Some(value);
            s.1.take()
        };
        if let Some(w) = waker {
            w.wake();
        }
    });
    Thread { slot }
}

impl<T> Future for Thread<T> {
    type Output = T;
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<T> {
        let mut s = self.slot.lock().unwrap();
        match s.0.take() {
            Some(v) => Poll::Ready(v),
            None => {
                s.1 = Some(cx.waker().clone());
                Poll::Pending
            }
        }
    }
}

fn sleep(d: Duration) -> Thread<()> {
    on_thread(move || thread::sleep(d))
}

fn to_jabba(v: &J) -> Json {
    match v {
        J::Null => Json::Null,
        J::Bool(b) => Json::Bool(*b),
        J::Number(n) => n
            .as_i64()
            .map_or_else(|| Json::Float(n.as_f64().unwrap()), Json::Int),
        J::String(s) => Json::Str(s.clone()),
        J::Array(a) => Json::Array(a.iter().map(to_jabba).collect()),
        J::Object(o) => Json::Object(o.iter().map(|(k, v)| (k.clone(), to_jabba(v))).collect()),
    }
}

// ---- scripted nodes -----------------------------------------------------------

#[derive(Clone)]
struct Behavior {
    name: String,
    raw: J,
}

impl Behavior {
    fn str(&self, key: &str) -> Option<String> {
        self.raw.get(key).and_then(J::as_str).map(str::to_owned)
    }

    fn flag(&self, key: &str) -> bool {
        self.raw.get(key).and_then(J::as_bool).unwrap_or(false)
    }

    fn sleep(&self) -> Option<Duration> {
        self.raw
            .get("sleep_ms")
            .and_then(J::as_f64)
            .map(|ms| Duration::from_secs_f64(ms / 1000.0))
    }

    /// The part after signal/wait_for: sleep, set_field, end event, fail or value.
    fn tail(&self, trace: &Trace, ctx: &Ctx, inputs: &DynInputs<J>) -> Result<J, NodeError> {
        if let Some(fields) = self.raw.get("set_field").and_then(J::as_object) {
            for (k, v) in fields {
                ctx.set(k.clone(), to_jabba(v));
            }
        }
        trace.event(format!("end:{}", self.name));
        if let Some(message) = self.str("fail") {
            return Err(NodeError::msg(message));
        }
        if self.flag("echo_inputs") {
            let data = inputs.iter().filter(|(n, _)| *n != jabba::REQUEST);
            return Ok(J::Object(
                data.map(|(n, v)| (n.to_owned(), (**v).clone())).collect(),
            ));
        }
        if self.flag("echo_request") {
            return Ok((**inputs.get(jabba::REQUEST).expect("request wired")).clone());
        }
        Ok(self.raw.get("value").cloned().unwrap_or(J::Null))
    }
}

async fn run_behavior(
    b: Behavior,
    trace: Arc<Trace>,
    inputs: DynInputs<J>,
    ctx: Ctx,
) -> Result<J, NodeError> {
    trace.event(format!("start:{}", b.name));
    if let Some(s) = b.str("signal") {
        trace.signal(&s);
    }
    if let Some(w) = b.str("wait_for") {
        trace.wait(&w).await;
    }
    if b.flag("ignore_cancel") {
        // Rust cancels by dropping the node future; work that ignores that lives on a thread.
        let d = b.sleep().unwrap_or_default();
        let (tb, tt) = (b.clone(), trace.clone());
        return on_thread(move || {
            thread::sleep(d);
            tb.tail(&tt, &ctx, &inputs)
        })
        .await;
    }
    if let Some(d) = b.sleep() {
        sleep(d).await;
    }
    b.tail(&trace, &ctx, &inputs)
}

// ---- running a case -----------------------------------------------------------

#[derive(Default, Debug)]
struct Outcome {
    status: String,
    compile_error: Option<String>,
    error_node: Option<String>,
    output: Option<J>,
    record: Option<J>,
    events: Vec<String>,
    duration_ms: f64,
}

fn add_nodes(g: &mut GraphBuilder<J>, case: &J, trace: &Arc<Trace>) {
    for n in case["graph"]["nodes"].as_array().unwrap() {
        let name = n["name"].as_str().unwrap().to_owned();
        let raw = n
            .get("behavior")
            .cloned()
            .unwrap_or(J::Object(Default::default()));
        let names = |key: &str| -> Vec<String> {
            n.get(key).and_then(J::as_array).map_or_else(Vec::new, |a| {
                a.iter().map(|v| v.as_str().unwrap().to_owned()).collect()
            })
        };
        let mut deps = names("deps");
        if raw
            .get("echo_request")
            .and_then(J::as_bool)
            .unwrap_or(false)
        {
            deps.push(jabba::REQUEST.to_owned());
        }
        let behavior = Behavior {
            name: name.clone(),
            raw,
        };
        let trace = trace.clone();
        let mut def = node(name)
            .deps_dyn::<J>(deps)
            .run(move |inputs, ctx| run_behavior(behavior.clone(), trace.clone(), inputs, ctx))
            .after(names("after"));
        if n.get("optional").and_then(J::as_bool).unwrap_or(false) {
            def = def.optional();
        }
        if let Some(d) = n.get("default") {
            def = def.default(d.clone());
        }
        if let Some(ms) = n.get("timeout_ms").and_then(J::as_f64) {
            def = def.timeout(Duration::from_secs_f64(ms / 1000.0));
        }
        g.add(def);
    }
}

fn ms(v: Option<&J>) -> Option<Duration> {
    v.and_then(J::as_f64)
        .map(|ms| Duration::from_secs_f64(ms / 1000.0))
}

fn run_case(case: &J) -> Outcome {
    let trace = Arc::new(Trace::default());
    let mut options = RunOptions::default();
    if let Some(deadline) = ms(case.get("deadline_ms")) {
        options = options.deadline(deadline);
    }
    let request = case.get("request").cloned().unwrap_or(J::Null);
    let cancel_after = ms(case.get("cancel_after_ms"));

    let mut g = Graph::builder::<J>(case["name"].as_str().unwrap());
    add_nodes(&mut g, case, &trace);
    let started = Instant::now();
    let result = match case["graph"].get("output").and_then(J::as_str) {
        Some(out) => match g.compile_with_output_named::<J>(out) {
            Ok(graph) => run(graph.run_with(request, options), cancel_after)
                .map(|r| r.map(|ok| (ok.output.map(|o| (*o).clone()), ok.stat))),
            Err(e) => return compile_outcome(e),
        },
        None => match g.compile() {
            Ok(graph) => run(graph.run_with(request, options), cancel_after)
                .map(|r| r.map(|ok| (None, ok.stat))),
            Err(e) => return compile_outcome(e),
        },
    };
    let duration_ms = started.elapsed().as_secs_f64() * 1000.0;

    let mut outcome = Outcome {
        duration_ms,
        ..Default::default()
    };
    let stat = match result {
        None => {
            outcome.status = "cancelled".into();
            None
        }
        Some(Ok((output, stat))) => {
            outcome.status = "ok".into();
            outcome.output = output;
            Some(stat)
        }
        Some(Err(RunError::NodeFailed { node, stat, .. })) => {
            outcome.status = "failed".into();
            outcome.error_node = Some(node);
            Some(*stat)
        }
        Some(Err(RunError::GraphTimeout { stat, .. })) => {
            outcome.status = "timeout".into();
            Some(*stat)
        }
        Some(Err(other)) => panic!("unexpected run error: {other}"),
    };
    let settle = case.get("settle_ms").and_then(J::as_f64).unwrap_or(0.0);
    thread::sleep(Duration::from_secs_f64(settle / 1000.0));
    outcome.record = stat.map(|s| {
        serde_json::from_str(&s.to_record().to_string()).expect("the record is valid JSON")
    });
    outcome.events = trace.events.lock().unwrap().clone();
    outcome
}

fn compile_outcome(e: CompileError) -> Outcome {
    Outcome {
        status: "compile_error".into(),
        compile_error: Some(e.kind.as_str().into()),
        ..Default::default()
    }
}

/// Run to completion, or drop the run after `cancel_after` (outer cancellation): `None`.
fn run<F: Future + Unpin>(mut fut: F, cancel_after: Option<Duration>) -> Option<F::Output> {
    let Some(after) = cancel_after else {
        return Some(block_on(fut));
    };
    let mut timer = sleep(after);
    block_on(std::future::poll_fn(move |cx| {
        if let Poll::Ready(v) = Pin::new(&mut fut).poll(cx) {
            return Poll::Ready(Some(v));
        }
        if Pin::new(&mut timer).poll(cx).is_ready() {
            return Poll::Ready(None);
        }
        Poll::Pending
    }))
}

// ---- checking -----------------------------------------------------------------

fn eq(errors: &mut Vec<String>, what: &str, actual: &J, expected: &J) {
    if actual != expected {
        errors.push(format!("{what}: expected {expected}, got {actual}"));
    }
}

fn check(case: &J, o: &Outcome) -> Vec<String> {
    let expect = &case["expect"];
    let mut errors = Vec::new();
    let opt = |v: &Option<String>| v.clone().map_or(J::Null, J::String);
    eq(
        &mut errors,
        "status",
        &J::String(o.status.clone()),
        &expect["status"],
    );
    if let Some(e) = expect.get("compile_error") {
        eq(&mut errors, "compile_error", &opt(&o.compile_error), e);
    }
    if let Some(e) = expect.get("error_node") {
        eq(&mut errors, "error_node", &opt(&o.error_node), e);
    }
    if let Some(e) = expect.get("output") {
        eq(
            &mut errors,
            "output",
            o.output.as_ref().unwrap_or(&J::Null),
            e,
        );
    }
    let status = expect["status"].as_str().unwrap();
    match (&o.record, status) {
        (Some(_), "cancelled" | "compile_error") => errors.push("record: expected none".into()),
        (None, "cancelled" | "compile_error") => {}
        (None, _) => errors.push("record: expected one, got none".into()),
        (Some(record), _) => {
            if let Some(nodes) = expect.get("nodes").and_then(J::as_object) {
                for (name, node_expect) in nodes {
                    let Some(actual) = record["nodes"].get(name) else {
                        errors.push(format!("node '{name}': missing from the record"));
                        continue;
                    };
                    for (key, value) in node_expect.as_object().unwrap() {
                        eq(
                            &mut errors,
                            &format!("node '{name}' {key}"),
                            &actual[key],
                            value,
                        );
                    }
                }
            }
            if let Some(fields) = expect.get("fields") {
                eq(&mut errors, "fields", &record["fields"], fields);
            }
        }
    }
    let pos = |e: &str| o.events.iter().position(|x| x == e);
    if let Some(order) = expect.get("order").and_then(J::as_array) {
        for pair in order {
            let (before, after) = (pair[0].as_str().unwrap(), pair[1].as_str().unwrap());
            match (
                pos(&format!("end:{before}")),
                pos(&format!("start:{after}")),
            ) {
                (Some(e), Some(s)) if e < s => {}
                _ => errors.push(format!(
                    "order: '{before}' must finish before '{after}' starts; events {:?}",
                    o.events
                )),
            }
        }
    }
    if let Some(stopped) = expect.get("stopped").and_then(J::as_array) {
        for name in stopped {
            if pos(&format!("end:{}", name.as_str().unwrap())).is_some() {
                errors.push(format!("stopped: node {name} ran to the end of its body"));
            }
        }
    }
    if let Some(max) = expect.get("max_duration_ms").and_then(J::as_f64) {
        if o.duration_ms > max {
            errors.push(format!("duration: {:.0}ms > max {max}ms", o.duration_ms));
        }
    }
    errors
}

#[test]
fn conformance() {
    let mut failures = Vec::new();
    let mut ran = 0;
    for (file, case) in load_cases() {
        let id = format!("{file}::{}", case["name"].as_str().unwrap());
        let requires: HashSet<&str> = case
            .get("requires")
            .and_then(J::as_array)
            .map_or_else(HashSet::new, |a| {
                a.iter().map(|v| v.as_str().unwrap()).collect()
            });
        if let Some(missing) = requires.iter().find(|r| !CAPABILITIES.contains(r)) {
            println!("skip {id}: driver does not support {missing}");
            continue;
        }
        let pending = case.get("pending").and_then(|p| p.get(LANGUAGE)).is_some();
        let errors = check(&case, &run_case(&case));
        ran += 1;
        match (pending, errors.is_empty()) {
            (false, true) => println!("ok   {id}"),
            (true, false) => println!("xfail {id}"),
            (false, false) => failures.push(format!("{id}\n  {}", errors.join("\n  "))),
            (true, true) => failures.push(format!(
                "{id}\n  passes but is marked pending for {LANGUAGE}"
            )),
        }
    }
    assert!(ran > 0, "no cases under {:?}", spec_dir());
    assert!(
        failures.is_empty(),
        "{} case(s) failed:\n{}",
        failures.len(),
        failures.join("\n")
    );
}

#[test]
fn case_names_are_unique() {
    let cases = load_cases();
    let names: HashSet<&str> = cases
        .iter()
        .map(|(_, c)| c["name"].as_str().unwrap())
        .collect();
    assert_eq!(names.len(), cases.len());
}
