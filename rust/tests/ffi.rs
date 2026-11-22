//! The foreign-node ABI from both sides: the Rust importer against adversarial C nodes,
//! and Rust exported nodes against a C host — the protocol kit (ffi/testing), loaded from
//! JABBA_FFI_TESTING_LIB.

use std::ffi::{c_char, c_int, c_long, c_void, CStr, CString};
use std::future::Future;
use std::pin::Pin;
use std::sync::{Arc, Mutex, MutexGuard, OnceLock};
use std::task::{Context, Poll, Waker};
use std::thread;
use std::time::{Duration, Instant};

use jabba::ffi::sys::{jabba_node, Factory, JABBA_CANCELLED, JABBA_FAILED, JABBA_OK};
use jabba::ffi::{export, export_dyn, export_sync, Array, Foreign};
use jabba::{
    block_on, node, CompileError, CompileErrorKind, Ctx, ErrorKind, Graph, Json, NodeError,
    NodeStatus, RunError, RunOk, RunOptions,
};

// ---- the kit ------------------------------------------------------------------------

#[cfg_attr(target_os = "linux", link(name = "dl"))]
extern "C" {
    fn dlopen(path: *const c_char, flags: c_int) -> *mut c_void;
    fn dlsym(handle: *mut c_void, name: *const c_char) -> *mut c_void;
}

#[repr(C)]
#[derive(Default, Debug, Clone, Copy)]
struct Counters {
    starts: c_long,
    ends: c_long,
    cancels: c_long,
    instances_made: c_long,
    instances_released: c_long,
    values_made: c_long,
    values_released: c_long,
    violations: c_long,
}

#[repr(C)]
struct DriveResult {
    ok: c_int,
    error: [c_char; 256],
    status: u32,
    value_json: [c_char; 256],
    set_fields: c_long,
}

const PLAIN: c_int = 0;
const CANCEL_SOON: c_int = 1;
const CANCEL_RACE: c_int = 2;

struct Kit {
    lib: *mut c_void,
    counters_get: unsafe extern "C" fn(*mut Counters),
    counters_reset: unsafe extern "C" fn(),
    first_violation: unsafe extern "C" fn() -> *const c_char,
    drive: unsafe extern "C" fn(*const jabba_node, c_int, *mut DriveResult),
}

unsafe impl Send for Kit {}
unsafe impl Sync for Kit {}

impl Kit {
    fn sym(&self, name: &str) -> *mut c_void {
        let cname = CString::new(name).unwrap();
        let p = unsafe { dlsym(self.lib, cname.as_ptr()) };
        assert!(!p.is_null(), "the kit has no {name}");
        p
    }

    fn factory(&self, name: &str) -> Factory {
        unsafe { std::mem::transmute::<*mut c_void, Factory>(self.sym(name)) }
    }

    fn counters(&self) -> Counters {
        let mut c = Counters::default();
        unsafe { (self.counters_get)(&mut c) };
        c
    }
}

/// The kit, and a lock: its counters are global, so tests using it run one at a time.
fn kit() -> Option<(&'static Kit, MutexGuard<'static, ()>)> {
    static KIT: OnceLock<Option<Kit>> = OnceLock::new();
    static SERIAL: Mutex<()> = Mutex::new(());
    let kit =
        KIT.get_or_init(|| {
            let path = std::env::var("JABBA_FFI_TESTING_LIB").ok()?;
            let cpath = CString::new(path.clone()).unwrap();
            let lib = unsafe {
                dlopen(cpath.as_ptr(), 2 /* RTLD_NOW */)
            };
            assert!(!lib.is_null(), "cannot load {path}");
            let sym = |name: &str| {
                let cname = CString::new(name).unwrap();
                let p = unsafe { dlsym(lib, cname.as_ptr()) };
                assert!(!p.is_null(), "{path} has no {name}");
                p
            };
            let kit = unsafe {
                Kit {
                    lib,
                    counters_get: std::mem::transmute::<
                        *mut c_void,
                        unsafe extern "C" fn(*mut Counters),
                    >(sym("jt_counters_get")),
                    counters_reset: std::mem::transmute::<*mut c_void, unsafe extern "C" fn()>(
                        sym("jt_counters_reset"),
                    ),
                    first_violation: std::mem::transmute::<
                        *mut c_void,
                        unsafe extern "C" fn() -> *const c_char,
                    >(sym("jt_first_violation")),
                    drive: std::mem::transmute::<
                        *mut c_void,
                        unsafe extern "C" fn(*const jabba_node, c_int, *mut DriveResult),
                    >(sym("jt_drive")),
                }
            };
            Some(kit)
        });
    match kit {
        Some(kit) => Some((kit, SERIAL.lock().unwrap_or_else(|e| e.into_inner()))),
        None => {
            eprintln!("JABBA_FFI_TESTING_LIB is not set: skipped");
            None
        }
    }
}

/// Wait until every started call has ended (calls may finish after their run), then check
/// that the nodes saw no protocol violation and nothing leaked.
fn settle_and_check(kit: &Kit, all_instances_released: bool) {
    let done = |c: &Counters| {
        c.starts == c.ends
            && c.values_made == c.values_released
            && (!all_instances_released || c.instances_made == c.instances_released)
    };
    for _ in 0..400 {
        if done(&kit.counters()) {
            break;
        }
        thread::sleep(Duration::from_millis(5));
    }
    let c = kit.counters();
    assert_eq!(
        c.violations,
        0,
        "protocol violation: {}",
        unsafe { CStr::from_ptr((kit.first_violation)()) }.to_string_lossy()
    );
    assert!(done(&c), "calls or values left over: {c:?}");
}

fn reset(kit: &Kit) {
    unsafe { (kit.counters_reset)() };
}

fn run_one(
    kit: &Kit,
    factory: &str,
    timeout: Option<Duration>,
    deadline: Option<Duration>,
    optional: bool,
) -> Result<RunOk<Json>, RunError> {
    let f = unsafe { Foreign::from_factory(kit.factory(factory)) };
    let mut g = Graph::builder::<i64>("g");
    let mut def = node("n").foreign::<Json>(&f);
    if let Some(t) = timeout {
        def = def.timeout(t);
    }
    if optional {
        def = def.optional().default(Json::Null);
    }
    let n = g.add(def);
    let graph = g.compile_with_output(&n).expect("compiles");
    let mut options = RunOptions::default();
    if let Some(d) = deadline {
        options = options.deadline(d);
    }
    block_on(graph.run_with(0, options))
}

// ---- importer: Rust host, adversarial C nodes -----------------------------------------

#[test]
fn import_a_node_that_completes_inside_start() {
    let Some((kit, _serial)) = kit() else { return };
    reset(kit);
    let r = run_one(kit, "jt_node_inline", None, None, false).unwrap();
    assert_eq!(*r.output.unwrap(), Json::from("inline"));
    settle_and_check(kit, true);
}

#[test]
fn import_a_node_completing_from_its_thread() {
    let Some((kit, _serial)) = kit() else { return };
    reset(kit);
    let r = run_one(kit, "jt_node_delay", None, None, false).unwrap();
    assert_eq!(*r.output.unwrap(), Json::from("delayed"));
    settle_and_check(kit, true);
}

#[test]
fn a_node_timeout_cancels_the_foreign_call() {
    let Some((kit, _serial)) = kit() else { return };
    reset(kit);
    let started = Instant::now();
    let r = run_one(
        kit,
        "jt_node_delay",
        Some(Duration::from_millis(3)),
        None,
        true,
    )
    .unwrap();
    assert!(started.elapsed() < Duration::from_millis(65));
    assert_eq!(r.stat.node("n").unwrap().status, NodeStatus::Timeout);
    settle_and_check(kit, true);
    assert_eq!(kit.counters().cancels, 1);
}

#[test]
fn a_node_that_completes_inside_cancel() {
    let Some((kit, _serial)) = kit() else { return };
    reset(kit);
    let r = run_one(
        kit,
        "jt_node_cancel_inline",
        None,
        Some(Duration::from_millis(10)),
        false,
    );
    let Err(RunError::GraphTimeout { stat, .. }) = r else {
        panic!("expected a graph timeout")
    };
    assert_eq!(
        stat.node("n").unwrap().error_kind,
        Some(ErrorKind::Deadline)
    );
    settle_and_check(kit, true);
}

#[test]
fn a_node_holding_a_lock_across_complete_does_not_deadlock_the_host() {
    let Some((kit, _serial)) = kit() else { return };
    reset(kit);
    let (tx, rx) = std::sync::mpsc::channel();
    thread::spawn(move || {
        for i in 0..200u64 {
            // a timeout around the ~2 ms completion makes cancel race with complete
            let t = Duration::from_micros(1500 + (i % 7) * 150);
            if run_one(kit, "jt_node_locked", Some(t), None, true).is_err() {
                let _ = tx.send(false);
                return;
            }
        }
        let _ = tx.send(true);
    });
    assert!(rx
        .recv_timeout(Duration::from_secs(30))
        .expect("no deadlock"));
    settle_and_check(kit, true);
}

#[test]
fn cancel_racing_complete_on_many_threads() {
    let Some((kit, _serial)) = kit() else { return };
    reset(kit);
    let f = unsafe { Foreign::from_factory(kit.factory("jt_node_race")) };
    let mut g = Graph::builder::<i64>("g");
    let n = g.add(
        node("race")
            .foreign::<Json>(&f)
            .optional()
            .default(Json::from("timed out"))
            .timeout(Duration::from_micros(50)),
    );
    let graph = g.compile_with_output(&n).unwrap();
    let workers: Vec<_> = (0..4)
        .map(|_| {
            let graph = graph.clone();
            thread::spawn(move || {
                (0..500)
                    .filter(|&i| block_on(graph.run(i)).is_err())
                    .count()
            })
        })
        .collect();
    let bad: usize = workers.into_iter().map(|w| w.join().unwrap()).sum();
    assert_eq!(bad, 0);
    settle_and_check(kit, false); // the graph still holds its instance
}

#[test]
fn a_result_of_the_wrong_kind_fails_the_node() {
    let Some((kit, _serial)) = kit() else { return };
    reset(kit);
    let f = unsafe { Foreign::from_factory(kit.factory("jt_node_wrong_kind")) };
    let mut g = Graph::builder::<i64>("g");
    let n = g.add(node("n").foreign::<Vec<f64>>(&f));
    drop(f);
    let r = block_on(g.compile_with_output(&n).unwrap().run(0));
    let Err(RunError::NodeFailed { error, .. }) = r else {
        panic!("expected a failure")
    };
    assert!(error.contains("returned f32s"), "{error}");
    settle_and_check(kit, true);
}

#[test]
fn a_failing_node_reports_its_error() {
    let Some((kit, _serial)) = kit() else { return };
    reset(kit);
    let r = run_one(kit, "jt_node_failing", None, None, false);
    let Err(RunError::NodeFailed { error, .. }) = r else {
        panic!("expected a failure")
    };
    assert_eq!(error, "boom");
    settle_and_check(kit, true);
}

#[test]
fn abi_and_kind_mismatches_are_compile_errors() {
    let Some((kit, _serial)) = kit() else { return };
    reset(kit);
    let kind = |r: Result<_, CompileError>| r.map(|_| ()).expect_err("a compile error").kind;
    let compile = |factory: &str| {
        let f = unsafe { Foreign::from_factory(kit.factory(factory)) };
        let mut g = Graph::builder::<Vec<f64>>("g");
        let n = g.add(node("n").foreign::<Json>(&f));
        g.compile_with_output(&n)
    };
    assert_eq!(
        kind(compile("jt_node_bad_abi")),
        CompileErrorKind::AbiMismatch
    );
    // it takes an input the graph does not wire
    assert_eq!(
        kind(compile("jt_node_f64_scale")),
        CompileErrorKind::TypeMismatch
    );
    // it returns f64s, not json
    let f = unsafe { Foreign::from_factory(kit.factory("jt_node_f64_scale")) };
    let mut g = Graph::builder::<Vec<f64>>("g");
    let req = g.request();
    let n = g.add(node("n").deps((&req,)).foreign::<Json>(&f));
    assert_eq!(
        kind(g.compile_with_output(&n)),
        CompileErrorKind::TypeMismatch
    );
    drop(f);
    settle_and_check(kit, false);
    let c = kit.counters();
    assert_eq!(c.instances_made, 3);
    // the one with another ABI is never called into, not even released
    assert_eq!(c.instances_released, 2);
}

#[test]
fn arrays_cross_without_copies_and_are_released_once() {
    let Some((kit, _serial)) = kit() else { return };
    reset(kit);
    {
        let f = unsafe { Foreign::from_factory(kit.factory("jt_node_f64_scale")) };
        let mut g = Graph::builder::<Vec<f64>>("g");
        let req = g.request();
        let n = g.add(node("scale").deps((&req,)).foreign::<Array<f64>>(&f));
        let graph = g.compile_with_output(&n).unwrap();
        drop(f);
        let r = block_on(graph.run(vec![1.0, 2.5, 4.0])).unwrap();
        assert_eq!(&r.output.as_ref().unwrap()[..], &[2.0, 5.0, 8.0]);
        // still owned by the zero-copy Array in the result
        assert_eq!(kit.counters().values_released, 0);
    }
    settle_and_check(kit, true);
}

#[test]
fn a_borrowed_result_buffer_is_copied_during_complete() {
    let Some((kit, _serial)) = kit() else { return };
    reset(kit);
    let f = unsafe { Foreign::from_factory(kit.factory("jt_node_borrowed_str")) };
    let mut g = Graph::builder::<i64>("g");
    let n = g.add(node("n").foreign::<String>(&f));
    let graph = g.compile_with_output(&n).unwrap();
    drop(f);
    let r = block_on(graph.run(0)).unwrap();
    assert_eq!(r.output.unwrap().as_str(), "borrowed");
    drop(graph);
    settle_and_check(kit, true);
}

#[test]
fn a_failed_result_carrying_a_value_still_releases_it() {
    let Some((kit, _serial)) = kit() else { return };
    reset(kit);
    let r = run_one(kit, "jt_node_failed_with_value", None, None, false);
    let Err(RunError::NodeFailed { error, .. }) = r else {
        panic!("expected a failure")
    };
    assert_eq!(error, "bad");
    settle_and_check(kit, true);
}

// ---- exporter: C host, Rust exported nodes ----------------------------------------------

/// Completes once a thread of its own has slept `d`; dropping it does not stop the thread.
struct Later {
    slot: Arc<Mutex<(bool, Option<Waker>)>>,
}

fn later(d: Duration) -> Later {
    let slot = Arc::new(Mutex::new((false, None::<Waker>)));
    let shared = slot.clone();
    thread::spawn(move || {
        thread::sleep(d);
        let waker = {
            let mut s = shared.lock().unwrap();
            s.0 = true;
            s.1.take()
        };
        if let Some(w) = waker {
            w.wake();
        }
    });
    Later { slot }
}

impl Future for Later {
    type Output = ();
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        let mut s = self.slot.lock().unwrap();
        if s.0 {
            return Poll::Ready(());
        }
        s.1 = Some(cx.waker().clone());
        Poll::Pending
    }
}

extern "C" fn exported_twice() -> *const jabba_node {
    export_sync("twice", |(x,): (i64,), _: &Ctx| Ok(x * 2))
}

extern "C" fn exported_async_fields() -> *const jabba_node {
    export("fields", |(input,): (Json,), ctx: Ctx| async move {
        ctx.set("seen", input.clone());
        Ok(Json::Object(vec![("echo".into(), input)]))
    })
}

extern "C" fn exported_fails() -> *const jabba_node {
    export_sync("fails", |(_,): (f64,), _: &Ctx| -> Result<f64, NodeError> {
        Err(NodeError::msg("bad luck"))
    })
}

extern "C" fn exported_panics() -> *const jabba_node {
    export_sync(
        "panics",
        |(_,): (f64,), _: &Ctx| -> Result<f64, NodeError> { panic!("deliberately") },
    )
}

extern "C" fn exported_waits_for_cancel() -> *const jabba_node {
    export("waits", |(): (), ctx: Ctx| async move {
        ctx.cancel_token().cancelled().await;
        Err::<Json, _>(NodeError::msg("stopped"))
    })
}

extern "C" fn exported_racy() -> *const jabba_node {
    // completes on a thread of its own after a random moment
    export("racy", |(): (), _: Ctx| async move {
        let jitter = Instant::now().elapsed().subsec_nanos() % 200;
        later(Duration::from_micros(u64::from(jitter))).await;
        Ok(Json::from("done"))
    })
}

extern "C" fn exported_scale() -> *const jabba_node {
    export_sync("scale", |(xs,): (Vec<f64>,), _: &Ctx| {
        Ok(xs.iter().map(|x| x * 3.0).collect::<Vec<f64>>())
    })
}

extern "C" fn exported_dyn() -> *const jabba_node {
    export_dyn::<Json, Json, _, _>("dyn", &["a"], |inputs, _| async move {
        Ok((**inputs.get("a").unwrap()).clone())
    })
}

extern "C" fn exported_stray_ctx() -> *const jabba_node {
    // The body hands a copy of its Ctx to work that outlives the call.
    export_sync("stray", |(): (), ctx: &Ctx| {
        let ctx = ctx.clone();
        thread::spawn(move || {
            thread::sleep(Duration::from_millis(30));
            ctx.set("late", true); // after complete and end: must be dropped
        });
        Ok(Json::from("now"))
    })
}

extern "C" fn exported_inline_async() -> *const jabba_node {
    export("inline_async", |(): (), _: Ctx| async {
        Ok(Json::from("at once"))
    })
}

struct Driven {
    status: u32,
    value: String,
    set_fields: c_long,
}

fn drive(kit: &Kit, factory: Factory, mode: c_int) -> Driven {
    let mut r = DriveResult {
        ok: 0,
        error: [0; 256],
        status: 99,
        value_json: [0; 256],
        set_fields: 0,
    };
    unsafe { (kit.drive)(factory(), mode, &mut r) };
    let text = |b: &[c_char; 256]| {
        unsafe { CStr::from_ptr(b.as_ptr()) }
            .to_string_lossy()
            .into_owned()
    };
    assert!(r.ok != 0, "protocol: {}", text(&r.error));
    Driven {
        status: r.status,
        value: text(&r.value_json),
        set_fields: r.set_fields,
    }
}

#[test]
fn export_a_sync_node() {
    let Some((kit, _serial)) = kit() else { return };
    let r = drive(kit, exported_twice, PLAIN);
    assert_eq!(r.status, JABBA_OK);
    assert_eq!(r.value, "42");
}

#[test]
fn export_an_async_node_that_writes_fields() {
    let Some((kit, _serial)) = kit() else { return };
    let r = drive(kit, exported_async_fields, PLAIN);
    assert_eq!(r.status, JABBA_OK);
    assert_eq!(r.value, r#"{"echo":{"a":1}}"#);
    assert_eq!(r.set_fields, 1);
}

#[test]
fn export_a_node_that_fails_or_panics() {
    let Some((kit, _serial)) = kit() else { return };
    let r = drive(kit, exported_fails, PLAIN);
    assert_eq!(r.status, JABBA_FAILED);
    assert_eq!(r.value, "bad luck");
    let r = drive(kit, exported_panics, PLAIN);
    assert_eq!(r.status, JABBA_FAILED);
    assert_eq!(r.value, "the node panicked");
}

extern "C" fn exported_panics_before_its_future() -> *const jabba_node {
    export(
        "eager",
        |(): (), _: Ctx| -> std::future::Ready<Result<Json, NodeError>> {
            panic!("deliberately, before returning the future")
        },
    )
}

#[test]
fn a_panic_while_starting_still_completes_the_call() {
    let Some((kit, _serial)) = kit() else { return };
    let r = drive(kit, exported_panics_before_its_future, PLAIN);
    assert_eq!(r.status, JABBA_FAILED);
    assert_eq!(r.value, "the node panicked");
}

#[test]
fn a_node_without_cancel_is_a_compile_error() {
    let mut copy = unsafe { std::ptr::read(exported_twice()) }; // the original is released through the copy
    copy.cancel = None;
    let copy = Box::into_raw(Box::new(copy));
    let f = unsafe { Foreign::adopt(copy) };
    let mut g = Graph::builder::<i64>("g");
    let req = g.request();
    let n = g.add(node("twice").deps((&req,)).foreign::<i64>(&f));
    let e = g
        .compile_with_output(&n)
        .map(|_| ())
        .expect_err("a compile error");
    assert_eq!(e.kind, CompileErrorKind::AbiMismatch);
    drop((f, n)); // the graph went with compile
    drop(unsafe { Box::from_raw(copy) });
}

#[test]
fn an_exported_node_stops_when_cancelled() {
    let Some((kit, _serial)) = kit() else { return };
    let r = drive(kit, exported_waits_for_cancel, CANCEL_SOON);
    assert_eq!(r.status, JABBA_CANCELLED);
}

#[test]
fn an_exported_node_survives_cancel_racing_its_completion() {
    let Some((kit, _serial)) = kit() else { return };
    for _ in 0..300 {
        let r = drive(kit, exported_racy, CANCEL_RACE);
        assert!(r.status == JABBA_OK || r.status == JABBA_CANCELLED);
    }
}

#[test]
fn export_arrays_and_dyn_inputs() {
    let Some((kit, _serial)) = kit() else { return };
    let r = drive(kit, exported_scale, PLAIN);
    assert_eq!(r.status, JABBA_OK);
    assert_eq!(r.value, "[3,6,9]");
    let r = drive(kit, exported_dyn, PLAIN);
    assert_eq!(r.value, r#"{"a":1}"#);
}

#[test]
fn late_ctx_writes_of_an_exported_node_are_dropped() {
    let Some((kit, _serial)) = kit() else { return };
    let r = drive(kit, exported_stray_ctx, PLAIN);
    assert_eq!(r.status, JABBA_OK);
    thread::sleep(Duration::from_millis(60)); // let the stray write happen
}

#[test]
fn cancel_after_an_exported_node_completed_does_not_touch_the_host() {
    let Some((kit, _serial)) = kit() else { return };
    let r = drive(kit, exported_inline_async, CANCEL_SOON);
    assert_eq!(r.status, JABBA_OK);
}

// ---- Rust to Rust through the ABI (no kit) ------------------------------------------------

#[test]
fn rust_to_rust_through_the_abi() {
    let f = unsafe { Foreign::from_factory(exported_twice) };
    let mut g = Graph::builder::<i64>("g");
    let req = g.request();
    let n = g.add(node("twice").deps((&req,)).foreign::<i64>(&f));
    let r = block_on(g.compile_with_output(&n).unwrap().run(21)).unwrap();
    assert_eq!(*r.output.unwrap(), 42);
}

#[test]
fn a_sync_body_never_runs_inside_start() {
    static SEEN: Mutex<Option<thread::ThreadId>> = Mutex::new(None);
    extern "C" fn exported_where() -> *const jabba_node {
        export_sync("where", |(): (), _: &Ctx| {
            *SEEN.lock().unwrap() = Some(thread::current().id());
            Ok(Json::Null)
        })
    }
    let f = unsafe { Foreign::from_factory(exported_where) };
    let mut g = Graph::builder::<i64>("g");
    let n = g.add(node("where").foreign::<Json>(&f));
    block_on(g.compile_with_output(&n).unwrap().run(0)).unwrap();
    let seen = SEEN.lock().unwrap().expect("the body ran");
    assert_ne!(seen, thread::current().id());
}

#[test]
fn fields_written_by_an_exported_node_reach_the_host_record() {
    let f = unsafe { Foreign::from_factory(exported_async_fields) };
    let mut g = Graph::builder::<Json>("g");
    let req = g.request();
    let n = g.add(node("fields").deps((&req,)).foreign::<Json>(&f));
    let r = block_on(g.compile_with_output(&n).unwrap().run(Json::from(7i64))).unwrap();
    let record = r.stat.to_record().to_string();
    assert!(record.contains(r#""seen":7"#), "{record}");
}
