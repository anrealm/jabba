//! Foreign nodes over the C ABI (`ffi/jabba_ffi.h`, rules in `spec/ffi.md`), behind the
//! `ffi` feature: use a node implemented in another language in a Rust graph
//! ([`Foreign`], [`NodeBuilder::foreign`](crate::NodeBuilder::foreign)), or export a Rust
//! node for graphs in other languages ([`export`], [`export_sync`], [`export_dyn`]).
//!
//! Values cross by [`FfiValue`]: `bool`, `i64`, `f64`, `String`, `Vec<u8>` (bytes),
//! [`Json`](crate::Json), `Vec<f64|f32|i64>`, [`Array`] (zero-copy results), `()`, `Option<T>`.

mod export;
mod import;
mod pool;
pub mod sys;
mod value;

pub use export::{export, export_dyn, export_sync, ExportArgs};
pub use import::{foreign_name, Foreign, ForeignInputs};
pub use value::{Array, Element, FfiValue, Scratch};
