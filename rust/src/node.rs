//! Nodes: typed handles, the builder and type-erased node bodies.

use std::any::{Any, TypeId};
use std::future::Future;
use std::marker::PhantomData;
use std::pin::Pin;
use std::sync::Arc;
use std::time::Duration;

use crate::ctx::Ctx;
use crate::error::NodeError;

pub(crate) type Value = Arc<dyn Any + Send + Sync>;
pub(crate) type BoxFuture = Pin<Box<dyn Future<Output = Result<Value, NodeError>> + Send>>;
pub(crate) type AsyncFn = Arc<dyn Fn(Vec<Value>, Ctx) -> BoxFuture + Send + Sync>;
pub(crate) type SyncFn = Arc<dyn Fn(Vec<Value>, &Ctx) -> Result<Value, NodeError> + Send + Sync>;

#[derive(Clone)]
pub(crate) enum Body {
    Async(AsyncFn),
    Sync(SyncFn),
}

#[doc(hidden)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TypeTag {
    pub(crate) id: TypeId,
    pub(crate) name: &'static str,
}

impl TypeTag {
    pub(crate) fn of<T: 'static>() -> Self {
        TypeTag {
            id: TypeId::of::<T>(),
            name: std::any::type_name::<T>(),
        }
    }
}

/// A typed handle of a node in a graph under construction. Passing handles to
/// `deps` makes the node's input types known to the compiler.
pub struct NodeRef<T> {
    name: Arc<str>,
    _type: PhantomData<fn() -> T>,
}

impl<T> NodeRef<T> {
    pub(crate) fn new(name: Arc<str>) -> Self {
        NodeRef {
            name,
            _type: PhantomData,
        }
    }

    pub fn name(&self) -> &str {
        &self.name
    }
}

impl<T> Clone for NodeRef<T> {
    fn clone(&self) -> Self {
        NodeRef::new(self.name.clone())
    }
}

mod sealed {
    pub trait Sealed {}
}

/// A list of data dependencies: `()`, or a tuple of `&NodeRef<_>` (up to 12).
pub trait IntoDeps: sealed::Sealed {
    #[doc(hidden)]
    type Inputs: Inputs;
    #[doc(hidden)]
    fn slots(&self) -> Vec<(Arc<str>, TypeTag)>;
}

/// How a node body receives its inputs. Implemented by jabba only.
pub trait Inputs: sealed::Sealed + 'static {
    /// What the node function receives.
    type Args: Send + 'static;
    #[doc(hidden)]
    fn extract(values: Vec<Value>, names: &[Arc<str>]) -> Self::Args;
}

impl sealed::Sealed for () {}

impl IntoDeps for () {
    type Inputs = ();
    fn slots(&self) -> Vec<(Arc<str>, TypeTag)> {
        Vec::new()
    }
}

impl Inputs for () {
    type Args = ();
    fn extract(_: Vec<Value>, _: &[Arc<str>]) {}
}

#[doc(hidden)]
pub struct Typed<T>(PhantomData<fn() -> T>);

fn downcast<T: Send + Sync + 'static>(value: Value) -> Arc<T> {
    value
        .downcast::<T>()
        .unwrap_or_else(|_| unreachable!("input types are checked at compile"))
}

macro_rules! tuple_deps {
    ($($t:ident),+) => {
        impl<'a, $($t: Send + Sync + 'static),+> sealed::Sealed for ($(&'a NodeRef<$t>,)+) {}
        impl<$($t: Send + Sync + 'static),+> sealed::Sealed for Typed<($($t,)+)> {}

        impl<'a, $($t: Send + Sync + 'static),+> IntoDeps for ($(&'a NodeRef<$t>,)+) {
            type Inputs = Typed<($($t,)+)>;
            #[allow(non_snake_case)]
            fn slots(&self) -> Vec<(Arc<str>, TypeTag)> {
                let ($($t,)+) = self;
                vec![$(($t.name.clone(), TypeTag::of::<$t>())),+]
            }
        }

        impl<$($t: Send + Sync + 'static),+> Inputs for Typed<($($t,)+)> {
            type Args = ($(Arc<$t>,)+);
            #[allow(non_snake_case)]
            fn extract(values: Vec<Value>, _: &[Arc<str>]) -> Self::Args {
                let mut values = values.into_iter();
                $(let $t = downcast::<$t>(values.next().expect("one value per input"));)+
                ($($t,)+)
            }
        }
    };
}

tuple_deps!(A);
tuple_deps!(A, B);
tuple_deps!(A, B, C);
tuple_deps!(A, B, C, D);
tuple_deps!(A, B, C, D, E);
tuple_deps!(A, B, C, D, E, F);
tuple_deps!(A, B, C, D, E, F, G);
tuple_deps!(A, B, C, D, E, F, G, H);
tuple_deps!(A, B, C, D, E, F, G, H, I);
tuple_deps!(A, B, C, D, E, F, G, H, I, J);
tuple_deps!(A, B, C, D, E, F, G, H, I, J, K);
tuple_deps!(A, B, C, D, E, F, G, H, I, J, K, L);

/// Inputs of a node whose dependencies are given by name, all of type `V`.
pub struct DynInputs<V> {
    items: Vec<(Arc<str>, Arc<V>)>,
}

impl<V> DynInputs<V> {
    pub fn get(&self, name: &str) -> Option<&Arc<V>> {
        self.items
            .iter()
            .find(|(n, _)| &**n == name)
            .map(|(_, v)| v)
    }

    /// `(dependency name, value)` in declaration order.
    pub fn iter(&self) -> impl Iterator<Item = (&str, &Arc<V>)> {
        self.items.iter().map(|(n, v)| (&**n, v))
    }

    pub fn len(&self) -> usize {
        self.items.len()
    }

    pub fn is_empty(&self) -> bool {
        self.items.is_empty()
    }
}

#[doc(hidden)]
pub struct Dyn<V>(PhantomData<fn() -> V>);

impl<V: Send + Sync + 'static> sealed::Sealed for Dyn<V> {}

impl<V: Send + Sync + 'static> Inputs for Dyn<V> {
    type Args = DynInputs<V>;
    fn extract(values: Vec<Value>, names: &[Arc<str>]) -> DynInputs<V> {
        DynInputs {
            items: names
                .iter()
                .cloned()
                .zip(values.into_iter().map(downcast::<V>))
                .collect(),
        }
    }
}

/// Start describing a node.
pub fn node(name: impl Into<String>) -> NodeBuilder<()> {
    NodeBuilder {
        name: Arc::from(name.into()),
        slots: Vec::new(),
        _inputs: PhantomData,
    }
}

/// A node with its dependencies chosen; finish it with `run` or `run_sync`.
#[must_use = "a node does nothing until it is finished with `run` and added to a graph"]
pub struct NodeBuilder<I> {
    name: Arc<str>,
    slots: Vec<(Arc<str>, TypeTag)>,
    _inputs: PhantomData<fn() -> I>,
}

impl NodeBuilder<()> {
    /// Typed data dependencies: `.deps((&parse, &features))`.
    pub fn deps<D: IntoDeps>(self, deps: D) -> NodeBuilder<D::Inputs> {
        NodeBuilder {
            name: self.name,
            slots: deps.slots(),
            _inputs: PhantomData,
        }
    }

    /// Data dependencies by name, all of type `V`; checked at compile.
    pub fn deps_dyn<V: Send + Sync + 'static>(
        self,
        names: impl IntoIterator<Item = impl AsRef<str>>,
    ) -> NodeBuilder<Dyn<V>> {
        let slots = names
            .into_iter()
            .map(|n| (Arc::from(n.as_ref()), TypeTag::of::<V>()))
            .collect();
        NodeBuilder {
            name: self.name,
            slots,
            _inputs: PhantomData,
        }
    }
}

impl<I: Inputs> NodeBuilder<I> {
    /// An async node body.
    pub fn run<F, Fut, T>(self, f: F) -> NodeDef<T>
    where
        F: Fn(I::Args, Ctx) -> Fut + Send + Sync + 'static,
        Fut: Future<Output = Result<T, NodeError>> + Send + 'static,
        T: Send + Sync + 'static,
    {
        let names: Arc<[Arc<str>]> = self.slots.iter().map(|(n, _)| n.clone()).collect();
        let body: AsyncFn = Arc::new(move |values, ctx| {
            let fut = f(I::extract(values, &names), ctx);
            Box::pin(async move { fut.await.map(|v| Arc::new(v) as Value) })
        });
        NodeDef::new(self.name, self.slots, Body::Async(body))
    }

    /// A synchronous node body. It runs inline on the thread that polls the run,
    /// to completion, and cannot be interrupted: meant for short CPU work. If it
    /// overruns its `timeout`, it is recorded as timed out and its result discarded.
    pub fn run_sync<F, T>(self, f: F) -> NodeDef<T>
    where
        F: Fn(I::Args, &Ctx) -> Result<T, NodeError> + Send + Sync + 'static,
        T: Send + Sync + 'static,
    {
        let names: Arc<[Arc<str>]> = self.slots.iter().map(|(n, _)| n.clone()).collect();
        let body: SyncFn = Arc::new(move |values, ctx| {
            f(I::extract(values, &names), ctx).map(|v| Arc::new(v) as Value)
        });
        NodeDef::new(self.name, self.slots, Body::Sync(body))
    }
}

/// A complete node description, ready to be added to a graph.
#[must_use = "a node does nothing until it is added to a graph"]
pub struct NodeDef<T> {
    pub(crate) name: Arc<str>,
    pub(crate) inputs: Vec<(Arc<str>, TypeTag)>,
    pub(crate) after: Vec<Arc<str>>,
    pub(crate) body: Body,
    pub(crate) output: TypeTag,
    pub(crate) optional: bool,
    pub(crate) default: Option<Value>,
    pub(crate) timeout: Option<Duration>,
    _type: PhantomData<fn() -> T>,
}

impl<T: Send + Sync + 'static> NodeDef<T> {
    fn new(name: Arc<str>, inputs: Vec<(Arc<str>, TypeTag)>, body: Body) -> Self {
        NodeDef {
            name,
            inputs,
            after: Vec::new(),
            body,
            output: TypeTag::of::<T>(),
            optional: false,
            default: None,
            timeout: None,
            _type: PhantomData,
        }
    }

    /// Ordering-only dependencies: wait for these nodes, receive no data.
    pub fn after<S: AsRef<str>>(mut self, names: impl IntoIterator<Item = S>) -> Self {
        self.after
            .extend(names.into_iter().map(|n| Arc::from(n.as_ref())));
        self
    }

    /// A failure or timeout of this node does not fail the run: data dependents
    /// receive the `default`.
    pub fn optional(mut self) -> Self {
        self.optional = true;
        self
    }

    /// What data dependents receive if this optional node fails or times out.
    pub fn default(mut self, value: T) -> Self {
        self.default = Some(Arc::new(value));
        self
    }

    /// Per-node timeout, measured from the node's start.
    pub fn timeout(mut self, timeout: Duration) -> Self {
        self.timeout = Some(timeout);
        self
    }
}

impl<T> std::fmt::Debug for NodeRef<T> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_tuple("NodeRef").field(&self.name).finish()
    }
}

impl<T> std::fmt::Debug for Typed<T> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("Typed")
    }
}

impl<V> std::fmt::Debug for Dyn<V> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("Dyn")
    }
}

impl<V: std::fmt::Debug> std::fmt::Debug for DynInputs<V> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_map().entries(self.iter()).finish()
    }
}

impl<I> std::fmt::Debug for NodeBuilder<I> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("NodeBuilder")
            .field("name", &self.name)
            .finish_non_exhaustive()
    }
}

impl<T> std::fmt::Debug for NodeDef<T> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("NodeDef")
            .field("name", &self.name)
            .field("optional", &self.optional)
            .field("timeout", &self.timeout)
            .finish_non_exhaustive()
    }
}
