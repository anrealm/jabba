//! Behavior that conformance cases cannot express: types, hooks, API ergonomics.

use std::future::Future;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use jabba::{block_on, node, CompileErrorKind, Graph, NodeError, NodeStatus, RunError, RunFuture};

fn assert_send_static<T: Send + 'static>(_: &T) {}

#[test]
fn run_future_is_send_and_static() {
    let mut g = Graph::builder::<u32>("g");
    let req = g.request();
    let out = g.add(
        node("double")
            .deps((&req,))
            .run(|(x,), _| async move { Ok(*x * 2) }),
    );
    let graph = g.compile_with_output(&out).unwrap();
    let run: RunFuture<u32> = graph.run(21);
    assert_send_static(&run);
    assert_eq!(*block_on(run).unwrap().output.unwrap(), 42);
}

#[test]
fn dyn_dependency_of_the_wrong_type_is_a_compile_error() {
    let mut g = Graph::builder::<()>("g");
    g.add(node("a").run_sync(|(), _| Ok(1u32)));
    g.add(node("b").deps_dyn::<String>(["a"]).run_sync(|_, _| Ok(())));
    let error = g.compile().unwrap_err();
    assert_eq!(error.kind, CompileErrorKind::TypeMismatch);
    assert!(error.message.contains("u32"), "{}", error.message);
}

#[test]
fn output_of_the_wrong_type_is_a_compile_error() {
    let mut g = Graph::builder::<()>("g");
    g.add(node("a").run_sync(|(), _| Ok(1u32)));
    let error = g.compile_with_output_named::<String>("a").unwrap_err();
    assert_eq!(error.kind, CompileErrorKind::TypeMismatch);
}

#[test]
fn question_mark_works_inside_nodes() {
    let mut g = Graph::builder::<String>("g");
    let req = g.request();
    let parsed = g.add(
        node("parse")
            .deps((&req,))
            .run_sync(|(s,), _| Ok(s.parse::<i64>()?)),
    );
    let graph = g.compile_with_output(&parsed).unwrap();
    assert_eq!(*block_on(graph.run("7".into())).unwrap().output.unwrap(), 7);
    match block_on(graph.run("x".into())) {
        Err(RunError::NodeFailed { node, error, stat }) => {
            assert_eq!(node, "parse");
            assert!(error.contains("invalid digit"), "{error}");
            assert_eq!(stat.node("parse").unwrap().status, NodeStatus::Failed);
        }
        other => panic!("expected a node failure, got {other:?}"),
    }
}

#[test]
fn on_run_done_is_called_once_per_recorded_run() {
    let calls = Arc::new(AtomicUsize::new(0));
    let seen = calls.clone();
    let mut g = Graph::builder::<bool>("g");
    let req = g.request();
    g.add(node("maybe_fail").deps((&req,)).run_sync(|(fail,), _| {
        if *fail {
            Err(NodeError::msg("boom"))
        } else {
            Ok(())
        }
    }));
    let graph = g.compile().unwrap().on_run_done(move |_| {
        seen.fetch_add(1, Ordering::SeqCst);
    });
    block_on(graph.run(false)).unwrap();
    block_on(graph.run(true)).unwrap_err();
    assert_eq!(calls.load(Ordering::SeqCst), 2);
}

#[test]
fn dropping_a_run_produces_no_record() {
    let calls = Arc::new(AtomicUsize::new(0));
    let seen = calls.clone();
    let mut g = Graph::builder::<()>("g");
    g.add(node("never").run(|(), _| std::future::pending::<Result<(), NodeError>>()));
    let graph = g.compile().unwrap().on_run_done(move |_| {
        seen.fetch_add(1, Ordering::SeqCst);
    });
    struct Noop;
    impl std::task::Wake for Noop {
        fn wake(self: Arc<Self>) {}
    }
    let waker = std::task::Waker::from(Arc::new(Noop));
    let mut run = Box::pin(graph.run(()));
    let _ = run
        .as_mut()
        .poll(&mut std::task::Context::from_waker(&waker));
    drop(run);
    assert_eq!(calls.load(Ordering::SeqCst), 0);
}

#[test]
fn a_timed_out_node_sees_its_cancel_token() {
    let token = Arc::new(Mutex::new(None));
    let slot = token.clone();
    let mut g = Graph::builder::<()>("g");
    g.add(
        node("slow")
            .run(move |(), ctx| {
                *slot.lock().unwrap() = Some(ctx.cancel_token().clone());
                std::future::pending::<Result<u8, NodeError>>()
            })
            .optional()
            .default(0)
            .timeout(Duration::from_millis(20)),
    );
    let started = Instant::now();
    let run = block_on(g.compile().unwrap().run(())).unwrap();
    assert!(started.elapsed() < Duration::from_millis(500));
    assert_eq!(run.stat.node("slow").unwrap().status, NodeStatus::Timeout);
    assert!(token.lock().unwrap().as_ref().unwrap().is_cancelled());
}

#[test]
fn concurrent_runs_of_one_graph_do_not_interfere() {
    let mut g = Graph::builder::<u64>("g");
    let req = g.request();
    let out = g.add(node("echo").deps((&req,)).run(|(x,), ctx| async move {
        ctx.set("x", *x as i64);
        Ok(*x)
    }));
    let graph = g.compile_with_output(&out).unwrap();
    let handles: Vec<_> = (0..8u64)
        .map(|i| {
            let graph = graph.clone();
            std::thread::spawn(move || block_on(graph.run(i)).unwrap())
        })
        .collect();
    for (i, h) in handles.into_iter().enumerate() {
        let run = h.join().unwrap();
        assert_eq!(*run.output.unwrap(), i as u64);
        assert_eq!(run.stat.field("x"), Some(&jabba::Json::Int(i as i64)));
    }
}

#[test]
fn request_ids_are_generated_and_unique() {
    let mut g = Graph::builder::<()>("g");
    g.add(node("a").run_sync(|(), _| Ok(())));
    let graph = g.compile().unwrap();
    let a = block_on(graph.run(())).unwrap().stat.request_id;
    let b = block_on(graph.run(())).unwrap().stat.request_id;
    assert_eq!(a.len(), 32);
    assert_ne!(a, b);
}

#[test]
fn huge_deadline_and_timeout_mean_no_limit() {
    let mut g = Graph::builder::<()>("g");
    g.add(
        node("a")
            .run(|(), _| async { Ok(1u8) })
            .timeout(Duration::MAX),
    );
    let options = jabba::RunOptions::default().deadline(Duration::MAX);
    let run = block_on(g.compile().unwrap().run_with((), options)).unwrap();
    assert_eq!(run.stat.node("a").unwrap().status, NodeStatus::Ok);
}

#[test]
fn a_panicking_hook_does_not_run_twice() {
    let calls = Arc::new(AtomicUsize::new(0));
    let seen = calls.clone();
    let mut g = Graph::builder::<()>("g");
    g.add(node("a").run_sync(|(), _| Ok(())));
    let graph = g.compile().unwrap().on_run_done(move |_| {
        seen.fetch_add(1, Ordering::SeqCst);
        panic!("hook bug");
    });
    let mut run = Box::pin(graph.run(()));
    let waker = noop_waker();
    let mut cx = std::task::Context::from_waker(&waker);
    let first =
        std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| run.as_mut().poll(&mut cx)));
    assert!(first.is_err());
    let second =
        std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| run.as_mut().poll(&mut cx)));
    assert!(second.is_err(), "polling a finished run panics");
    assert_eq!(calls.load(Ordering::SeqCst), 1);
}

#[test]
fn a_run_is_not_resumed_after_a_node_panicked() {
    let mut g = Graph::builder::<()>("g");
    let a = g.add(node("a").run_sync(|(), _| -> Result<u8, NodeError> { panic!("node bug") }));
    let graph = g.compile_with_output(&a).unwrap();
    let mut run = Box::pin(graph.run(()));
    let waker = noop_waker();
    let mut cx = std::task::Context::from_waker(&waker);
    let first =
        std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| run.as_mut().poll(&mut cx)));
    assert!(first.is_err());
    let second =
        std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| run.as_mut().poll(&mut cx)));
    assert!(
        second.is_err(),
        "a poisoned run refuses to continue instead of finishing as ok"
    );
}

fn noop_waker() -> std::task::Waker {
    struct Noop;
    impl std::task::Wake for Noop {
        fn wake(self: Arc<Self>) {}
    }
    std::task::Waker::from(Arc::new(Noop))
}

#[test]
fn a_sync_node_that_overran_its_timeout_is_a_timeout() {
    let mut g = Graph::builder::<()>("g");
    let slow = g.add(
        node("slow")
            .run_sync(|(), _| {
                std::thread::sleep(Duration::from_millis(60));
                Ok(1u8)
            })
            .optional()
            .default(0)
            .timeout(Duration::from_millis(10)),
    );
    let graph = g.compile_with_output(&slow).unwrap();
    let run = block_on(graph.run(())).unwrap();
    assert_eq!(*run.output.unwrap(), 0, "the late result is discarded");
    let stat = run.stat.node("slow").unwrap();
    assert_eq!(stat.status, NodeStatus::Timeout);
    assert_eq!(stat.error_kind, Some(jabba::ErrorKind::Timeout));
}

#[test]
fn record_floats_keep_their_fraction_and_extremes() {
    use jabba::Json;
    let record = Json::Object(vec![
        ("one".into(), Json::Float(1.0)),
        ("huge".into(), Json::Float(1e300)),
        ("tiny".into(), Json::Float(5e-324)),
        ("neg_zero".into(), Json::Float(-0.0)),
        ("big_u64".into(), Json::from(u64::MAX)),
        ("len".into(), Json::from(3usize)),
    ]);
    let text = record.to_string();
    let parsed: serde_json::Value = serde_json::from_str(&text).unwrap();
    assert!(parsed["one"].is_f64(), "{text}");
    assert_eq!(parsed["huge"].as_f64(), Some(1e300));
    assert_eq!(parsed["tiny"].as_f64(), Some(5e-324));
    assert_eq!(parsed["len"].as_i64(), Some(3));
    assert!(text.len() < 200, "no 300-digit integers: {text}");
}
