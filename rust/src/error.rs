use std::fmt;

use crate::stat::RequestStat;

/// An error returned by a node.
///
/// Deliberately does not implement `std::error::Error`: that keeps the blanket
/// `From<E: Error>` conversion possible, so `?` works inside nodes.
pub struct NodeError {
    message: String,
}

impl NodeError {
    pub fn msg(message: impl Into<String>) -> Self {
        NodeError {
            message: message.into(),
        }
    }

    pub fn message(&self) -> &str {
        &self.message
    }
}

impl<E: std::error::Error + Send + Sync + 'static> From<E> for NodeError {
    fn from(error: E) -> Self {
        NodeError {
            message: error.to_string(),
        }
    }
}

impl fmt::Debug for NodeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "NodeError({:?})", self.message)
    }
}

impl fmt::Display for NodeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

/// Category of a compile error (spec/semantics.md §2).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[non_exhaustive]
pub enum CompileErrorKind {
    ReservedName,
    DuplicateName,
    UnknownDep,
    UnknownAfter,
    UnknownOutput,
    OptionalWithoutDefault,
    OptionalOutputWithoutDefault,
    Cycle,
    TypeMismatch,
}

impl CompileErrorKind {
    pub fn as_str(self) -> &'static str {
        match self {
            CompileErrorKind::ReservedName => "reserved_name",
            CompileErrorKind::DuplicateName => "duplicate_name",
            CompileErrorKind::UnknownDep => "unknown_dep",
            CompileErrorKind::UnknownAfter => "unknown_after",
            CompileErrorKind::UnknownOutput => "unknown_output",
            CompileErrorKind::OptionalWithoutDefault => "optional_without_default",
            CompileErrorKind::OptionalOutputWithoutDefault => "optional_output_without_default",
            CompileErrorKind::Cycle => "cycle",
            CompileErrorKind::TypeMismatch => "type_mismatch",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
#[non_exhaustive]
pub struct CompileError {
    pub kind: CompileErrorKind,
    pub message: String,
}

impl fmt::Display for CompileError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

impl std::error::Error for CompileError {}

/// A run that ended without success but with a record.
#[derive(Debug, Clone)]
#[non_exhaustive]
pub enum RunError {
    /// A non-optional node failed or timed out: the run failed fast.
    NodeFailed {
        node: String,
        error: String,
        stat: Box<RequestStat>,
    },
    /// The graph deadline passed.
    GraphTimeout {
        graph: String,
        stat: Box<RequestStat>,
    },
}

impl RunError {
    pub fn stat(&self) -> &RequestStat {
        match self {
            RunError::NodeFailed { stat, .. } | RunError::GraphTimeout { stat, .. } => stat,
        }
    }
}

impl fmt::Display for RunError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            RunError::NodeFailed { node, error, .. } => write!(f, "node '{node}' failed: {error}"),
            RunError::GraphTimeout { graph, .. } => write!(f, "graph '{graph}' timed out"),
        }
    }
}

impl std::error::Error for RunError {}
