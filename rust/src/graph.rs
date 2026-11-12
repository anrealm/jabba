//! Graph assembly and compilation (spec/semantics.md §1–§2).

use std::collections::HashMap;
use std::marker::PhantomData;
use std::sync::Arc;
use std::time::Duration;

use crate::error::{CompileError, CompileErrorKind};
use crate::node::{Body, NodeDef, NodeRef, TypeTag, Value};
use crate::run::{RunFuture, RunOptions};
use crate::stat::RequestStat;

/// Name of the pseudo-node that provides the run's request.
pub const REQUEST: &str = "request";
/// Reserved as well, for parity with implementations that inject a context by name.
pub const CTX: &str = "ctx";

pub(crate) type RunHook = Arc<dyn Fn(&RequestStat) + Send + Sync>;

/// Where a node input comes from.
#[derive(Clone, Copy)]
pub(crate) enum Source {
    Request,
    Node(usize),
}

#[derive(Clone)]
pub(crate) struct CompiledNode {
    pub(crate) name: Arc<str>,
    pub(crate) inputs: Vec<Source>,
    pub(crate) body: Body,
    pub(crate) optional: bool,
    pub(crate) default: Option<Value>,
    pub(crate) timeout: Option<Duration>,
}

#[derive(Clone)]
pub(crate) struct Inner {
    pub(crate) name: Arc<str>,
    pub(crate) nodes: Vec<CompiledNode>,
    /// Data and ordering dependents of each node.
    pub(crate) children: Vec<Vec<usize>>,
    /// Number of data and ordering dependencies of each node.
    pub(crate) indegree: Vec<usize>,
    pub(crate) output: Option<usize>,
    pub(crate) on_run_done: Option<RunHook>,
}

/// Mutable, build-time graph assembly. Compile it once into a [`CompiledGraph`].
pub struct GraphBuilder<R> {
    name: Arc<str>,
    nodes: Vec<Entry>,
    errors: Vec<CompileError>,
    _request: PhantomData<fn() -> R>,
}

struct Entry {
    name: Arc<str>,
    inputs: Vec<(Arc<str>, TypeTag)>,
    after: Vec<Arc<str>>,
    body: Body,
    output: TypeTag,
    optional: bool,
    default: Option<Value>,
    timeout: Option<Duration>,
}

/// Entry point: `Graph::builder::<Request>("scoring")`.
#[derive(Debug)]
pub struct Graph;

impl Graph {
    pub fn builder<R: Send + Sync + 'static>(name: impl Into<String>) -> GraphBuilder<R> {
        GraphBuilder {
            name: Arc::from(name.into()),
            nodes: Vec::new(),
            errors: Vec::new(),
            _request: PhantomData,
        }
    }
}

impl<R: Send + Sync + 'static> GraphBuilder<R> {
    /// Handle of the run's request, usable as a dependency.
    pub fn request(&self) -> NodeRef<R> {
        NodeRef::new(Arc::from(REQUEST))
    }

    /// Add a node. Problems (reserved or duplicate names) are reported by `compile`.
    pub fn add<T: Send + Sync + 'static>(&mut self, def: NodeDef<T>) -> NodeRef<T> {
        let name = def.name.clone();
        if &*name == REQUEST || &*name == CTX {
            self.fail(
                CompileErrorKind::ReservedName,
                format!("node name '{name}' is reserved"),
            );
        } else if self.nodes.iter().any(|e| e.name == name) {
            self.fail(
                CompileErrorKind::DuplicateName,
                format!("duplicate node name '{name}'"),
            );
        }
        self.nodes.push(Entry {
            name: def.name,
            inputs: def.inputs,
            after: def.after,
            body: def.body,
            output: def.output,
            optional: def.optional,
            default: def.default,
            timeout: def.timeout,
        });
        NodeRef::new(name)
    }

    fn fail(&mut self, kind: CompileErrorKind, message: String) {
        let message = format!("graph '{}': {message}", self.name);
        self.errors.push(CompileError { kind, message });
    }

    /// Compile a graph without an output node.
    pub fn compile(self) -> Result<CompiledGraph<R, ()>, CompileError> {
        self.build(None, TypeTag::of::<()>())
    }

    /// Compile a graph whose result is the value of `output`.
    pub fn compile_with_output<O: Send + Sync + 'static>(
        self,
        output: &NodeRef<O>,
    ) -> Result<CompiledGraph<R, O>, CompileError> {
        let name = output.name().to_owned();
        self.build(Some(name), TypeTag::of::<O>())
    }

    /// Like `compile_with_output`, with the output node given by name; its type is checked.
    pub fn compile_with_output_named<O: Send + Sync + 'static>(
        self,
        output: &str,
    ) -> Result<CompiledGraph<R, O>, CompileError> {
        self.build(Some(output.to_owned()), TypeTag::of::<O>())
    }

    fn build<O>(
        self,
        output: Option<String>,
        output_type: TypeTag,
    ) -> Result<CompiledGraph<R, O>, CompileError> {
        if let Some(error) = self.errors.first() {
            return Err(error.clone());
        }
        let err = |kind, message: String| {
            Err(CompileError {
                kind,
                message: format!("graph '{}': {message}", self.name),
            })
        };
        let index: HashMap<&str, usize> = self
            .nodes
            .iter()
            .enumerate()
            .map(|(i, e)| (&*e.name, i))
            .collect();
        let request_type = TypeTag::of::<R>();

        // Unknown references and input types.
        let mut inputs = Vec::with_capacity(self.nodes.len());
        for e in &self.nodes {
            let mut sources = Vec::with_capacity(e.inputs.len());
            for (dep, expected) in &e.inputs {
                let (source, provided) = if &**dep == REQUEST {
                    (Source::Request, request_type)
                } else if let Some(&i) = index.get(&**dep) {
                    (Source::Node(i), self.nodes[i].output)
                } else {
                    return err(
                        CompileErrorKind::UnknownDep,
                        format!("node '{}' has dependency on unknown node '{dep}'", e.name),
                    );
                };
                if provided.id != expected.id {
                    return err(
                        CompileErrorKind::TypeMismatch,
                        format!(
                            "node '{}' expects '{dep}' as {}, but it provides {}",
                            e.name, expected.name, provided.name
                        ),
                    );
                }
                sources.push(source);
            }
            for dep in &e.after {
                if !index.contains_key(&**dep) {
                    return err(
                        CompileErrorKind::UnknownAfter,
                        format!("node '{}' has after on unknown node '{dep}'", e.name),
                    );
                }
            }
            inputs.push(sources);
        }
        let output = match output {
            None => None,
            Some(name) => match index.get(name.as_str()) {
                Some(&i) => Some(i),
                None => {
                    return err(
                        CompileErrorKind::UnknownOutput,
                        format!("output points to unknown node '{name}'"),
                    )
                }
            },
        };
        if let Some(i) = output {
            let provided = self.nodes[i].output;
            if provided.id != output_type.id {
                return err(
                    CompileErrorKind::TypeMismatch,
                    format!(
                        "output node '{}' returns {}, but the graph output is {}",
                        self.nodes[i].name, provided.name, output_type.name
                    ),
                );
            }
        }

        // Optional nodes must declare what their data dependents receive.
        for (e, sources) in self.nodes.iter().zip(&inputs) {
            for source in sources {
                if let Source::Node(i) = *source {
                    let provider = &self.nodes[i];
                    if provider.optional && provider.default.is_none() {
                        return err(
                            CompileErrorKind::OptionalWithoutDefault,
                            format!(
                                "optional node '{}' has data dependents but declares no default \
                                 (needed by '{}')",
                                provider.name, e.name
                            ),
                        );
                    }
                }
            }
        }
        if let Some(i) = output {
            let out = &self.nodes[i];
            if out.optional && out.default.is_none() {
                return err(
                    CompileErrorKind::OptionalOutputWithoutDefault,
                    format!("optional output node '{}' declares no default", out.name),
                );
            }
        }

        // Edges, then cycle detection (Kahn).
        let n = self.nodes.len();
        let mut children = vec![Vec::new(); n];
        let mut indegree = vec![0; n];
        for (child, (e, sources)) in self.nodes.iter().zip(&inputs).enumerate() {
            let data = sources.iter().filter_map(|s| match s {
                Source::Node(i) => Some(*i),
                Source::Request => None,
            });
            let ordering = e.after.iter().map(|dep| index[&**dep]);
            for parent in data.chain(ordering) {
                children[parent].push(child);
                indegree[child] += 1;
            }
        }
        let mut remaining = indegree.clone();
        let mut ready: Vec<usize> = (0..n).filter(|&i| remaining[i] == 0).collect();
        let mut visited = 0;
        while let Some(i) = ready.pop() {
            visited += 1;
            for &c in &children[i] {
                remaining[c] -= 1;
                if remaining[c] == 0 {
                    ready.push(c);
                }
            }
        }
        if visited != n {
            let stuck: Vec<&str> = (0..n)
                .filter(|&i| remaining[i] > 0)
                .map(|i| &*self.nodes[i].name)
                .collect();
            return err(
                CompileErrorKind::Cycle,
                format!("cycle involving nodes {stuck:?}"),
            );
        }

        let nodes = self
            .nodes
            .into_iter()
            .zip(inputs)
            .map(|(e, inputs)| CompiledNode {
                name: e.name,
                inputs,
                body: e.body,
                optional: e.optional,
                default: e.default,
                timeout: e.timeout,
            })
            .collect();
        let inner = Inner {
            name: self.name,
            nodes,
            children,
            indegree,
            output,
            on_run_done: None,
        };
        Ok(CompiledGraph {
            inner: Arc::new(inner),
            _types: PhantomData,
        })
    }
}

/// An immutable, validated graph. Cheap to clone; serves concurrent runs.
pub struct CompiledGraph<R, O = ()> {
    pub(crate) inner: Arc<Inner>,
    _types: PhantomData<fn(R) -> O>,
}

impl<R, O> Clone for CompiledGraph<R, O> {
    fn clone(&self) -> Self {
        CompiledGraph {
            inner: self.inner.clone(),
            _types: PhantomData,
        }
    }
}

impl<R: Send + Sync + 'static, O: Send + Sync + 'static> CompiledGraph<R, O> {
    pub fn name(&self) -> &str {
        &self.inner.name
    }

    pub fn node_names(&self) -> impl Iterator<Item = &str> {
        self.inner.nodes.iter().map(|n| &*n.name)
    }

    /// Called once per run that produces a record, after the record is frozen,
    /// on the thread where the run completed. Must not panic.
    pub fn on_run_done(mut self, hook: impl Fn(&RequestStat) + Send + Sync + 'static) -> Self {
        Arc::make_mut(&mut self.inner).on_run_done = Some(Arc::new(hook));
        self
    }

    /// Run the graph for one request. Drive the returned future with any executor.
    pub fn run(&self, request: R) -> RunFuture<O> {
        self.run_with(request, RunOptions::default())
    }

    pub fn run_with(&self, request: R, options: RunOptions) -> RunFuture<O> {
        RunFuture::new(self.inner.clone(), Arc::new(request), options)
    }
}

impl<R> std::fmt::Debug for GraphBuilder<R> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let nodes: Vec<&str> = self.nodes.iter().map(|e| &*e.name).collect();
        f.debug_struct("GraphBuilder")
            .field("name", &self.name)
            .field("nodes", &nodes)
            .finish()
    }
}

impl<R, O> std::fmt::Debug for CompiledGraph<R, O> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let nodes: Vec<&str> = self.inner.nodes.iter().map(|n| &*n.name).collect();
        f.debug_struct("CompiledGraph")
            .field("name", &self.inner.name)
            .field("nodes", &nodes)
            .finish()
    }
}
