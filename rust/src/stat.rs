//! Per-run statistics and the request record (spec/semantics.md §5).

use crate::json::Json;

/// Bump when the record layout changes; consumers rely on it.
pub const RECORD_VERSION: i64 = 1;

/// Final status of a node.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NodeStatus {
    Ok,
    Failed,
    Timeout,
    Skipped,
}

impl NodeStatus {
    pub fn as_str(self) -> &'static str {
        match self {
            NodeStatus::Ok => "ok",
            NodeStatus::Failed => "failed",
            NodeStatus::Timeout => "timeout",
            NodeStatus::Skipped => "skipped",
        }
    }
}

/// Final status of a run that produced a record.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RunStatus {
    Ok,
    Failed,
    Timeout,
}

impl RunStatus {
    pub fn as_str(self) -> &'static str {
        match self {
            RunStatus::Ok => "ok",
            RunStatus::Failed => "failed",
            RunStatus::Timeout => "timeout",
        }
    }
}

/// Normative category of a node error; the error text itself is informative only.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[non_exhaustive]
pub enum ErrorKind {
    Failed,
    Timeout,
    Deadline,
    Cancelled,
}

impl ErrorKind {
    pub fn as_str(self) -> &'static str {
        match self {
            ErrorKind::Failed => "failed",
            ErrorKind::Timeout => "timeout",
            ErrorKind::Deadline => "deadline",
            ErrorKind::Cancelled => "cancelled",
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
#[non_exhaustive]
pub struct NodeStat {
    pub status: NodeStatus,
    pub error_kind: Option<ErrorKind>,
    pub error: Option<String>,
    /// Unix epoch seconds; `None` if the node never started.
    pub started_at: Option<f64>,
    /// From start until the outcome was recorded; `None` if the node never started.
    pub duration_ms: Option<f64>,
}

impl NodeStat {
    pub(crate) fn skipped() -> Self {
        NodeStat {
            status: NodeStatus::Skipped,
            error_kind: None,
            error: None,
            started_at: None,
            duration_ms: None,
        }
    }
}

/// The finalized record of one run.
#[derive(Debug, Clone, PartialEq)]
#[non_exhaustive]
pub struct RequestStat {
    pub request_id: String,
    pub graph: String,
    pub status: RunStatus,
    pub duration_ms: f64,
    /// In graph declaration order.
    pub nodes: Vec<(String, NodeStat)>,
    /// Written by nodes through `Ctx::set`; last write wins.
    pub fields: Vec<(String, Json)>,
}

impl RequestStat {
    pub fn node(&self, name: &str) -> Option<&NodeStat> {
        self.nodes.iter().find(|(n, _)| n == name).map(|(_, s)| s)
    }

    pub fn field(&self, key: &str) -> Option<&Json> {
        self.fields.iter().find(|(k, _)| k == key).map(|(_, v)| v)
    }

    /// The schema-versioned record (spec/record.schema.json).
    pub fn to_record(&self) -> Json {
        let opt_f = |v: Option<f64>| v.map_or(Json::Null, Json::Float);
        let nodes = self
            .nodes
            .iter()
            .map(|(name, s)| {
                let node = Json::Object(vec![
                    ("status".into(), Json::from(s.status.as_str())),
                    (
                        "error_kind".into(),
                        Json::from(s.error_kind.map(ErrorKind::as_str)),
                    ),
                    ("error".into(), Json::from(s.error.clone())),
                    ("started_at".into(), opt_f(s.started_at)),
                    ("duration_ms".into(), opt_f(s.duration_ms)),
                ]);
                (name.clone(), node)
            })
            .collect();
        Json::Object(vec![
            ("record_version".into(), Json::Int(RECORD_VERSION)),
            ("request_id".into(), Json::from(self.request_id.as_str())),
            ("graph".into(), Json::from(self.graph.as_str())),
            ("status".into(), Json::from(self.status.as_str())),
            ("duration_ms".into(), Json::Float(self.duration_ms)),
            ("nodes".into(), Json::Object(nodes)),
            ("fields".into(), Json::Object(self.fields.clone())),
        ])
    }
}
