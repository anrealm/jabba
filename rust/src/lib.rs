//! # jabba
//!
//! Compiled graphs of async nodes. A request is served by a precompiled graph:
//! independent nodes run concurrently, timeouts and failures follow the
//! declared policy, and every run produces one schema-versioned record.
//!
//! The semantics are shared with the other jabba implementations and pinned by
//! the conformance cases in the repository's `spec/` directory.
//!
//! ```
//! use std::time::Duration;
//! use jabba::{block_on, node, Graph, NodeError};
//!
//! struct Request { user: String }
//!
//! let mut g = Graph::builder::<Request>("greeting");
//! let req = g.request();
//! let name = g.add(node("name").deps((&req,)).run_sync(|(r,), _ctx| Ok(r.user.clone())));
//! let extra = g.add(
//!     node("extra")
//!         .run(|(), _ctx| async { Err::<String, _>(NodeError::msg("source down")) })
//!         .optional()
//!         .default(String::new())
//!         .timeout(Duration::from_millis(100)),
//! );
//! let greeting = g.add(
//!     node("greeting")
//!         .deps((&name, &extra))
//!         .run(|(name, extra), ctx| async move {
//!             ctx.set("had_extra", !extra.is_empty());
//!             Ok(format!("hello, {name}{extra}"))
//!         }),
//! );
//! let graph = g.compile_with_output(&greeting).unwrap();
//!
//! let run = block_on(graph.run(Request { user: "alice".into() })).unwrap();
//! assert_eq!(run.output.as_deref().map(String::as_str), Some("hello, alice"));
//! assert_eq!(run.stat.node("extra").unwrap().status, jabba::NodeStatus::Failed);
//! ```
//!
//! The run future is executor-agnostic and `Send + 'static`: await it in
//! tokio, smol or anything else, or use [`block_on`].

mod ctx;
mod error;
mod graph;
mod json;
mod node;
mod run;
mod stat;
mod timer;

pub use ctx::{CancelToken, Cancelled, Ctx};
pub use error::{CompileError, CompileErrorKind, NodeError, RunError};
pub use graph::{CompiledGraph, Graph, GraphBuilder, CTX, REQUEST};
pub use json::Json;
pub use node::{node, DynInputs, Inputs, IntoDeps, NodeBuilder, NodeDef, NodeRef};
pub use run::{block_on, RunFuture, RunOk, RunOptions};
pub use stat::{ErrorKind, NodeStat, NodeStatus, RequestStat, RunStatus, RECORD_VERSION};

/// Compiles the README's examples as doc tests.
#[doc = include_str!("../README.md")]
#[cfg(doctest)]
pub struct ReadmeDoctests;
