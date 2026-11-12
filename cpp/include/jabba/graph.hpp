// SPDX-License-Identifier: MIT
// Nodes, graphs and runs (spec/semantics.md §1–§6).
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <variant>
#include <vector>

#include "json.hpp"
#include "stat.hpp"
#include "stop.hpp"
#include "task.hpp"
#include "timer.hpp"

namespace jabba {

/// How long a deadline, a failure or an outer cancellation waits for cancelled nodes.
inline constexpr std::chrono::milliseconds cancel_grace{50};
/// Name of the pseudo-node that provides the run's request.
inline constexpr std::string_view request_name = "request";
/// Reserved as well, for parity with implementations that inject a context by name.
inline constexpr std::string_view ctx_name = "ctx";

/// Where jabba runs node code and run continuations. `post` must queue `fn`
/// and return; it must not run `fn` inline.
class Executor {
public:
    virtual ~Executor() = default;
    virtual void post(std::function<void()> fn) = 0;
};

// ---- errors -------------------------------------------------------------------

enum class CompileErrorKind {
    reserved_name,
    duplicate_name,
    unknown_dep,
    unknown_after,
    unknown_output,
    optional_without_default,
    optional_output_without_default,
    cycle,
    type_mismatch,
};

inline std::string_view to_string(CompileErrorKind k) {
    switch (k) {
        case CompileErrorKind::reserved_name: return "reserved_name";
        case CompileErrorKind::duplicate_name: return "duplicate_name";
        case CompileErrorKind::unknown_dep: return "unknown_dep";
        case CompileErrorKind::unknown_after: return "unknown_after";
        case CompileErrorKind::unknown_output: return "unknown_output";
        case CompileErrorKind::optional_without_default: return "optional_without_default";
        case CompileErrorKind::optional_output_without_default: return "optional_output_without_default";
        case CompileErrorKind::cycle: return "cycle";
        case CompileErrorKind::type_mismatch: return "type_mismatch";
    }
    return "?";
}

class CompileError : public std::runtime_error {
public:
    CompileError(CompileErrorKind kind, const std::string& message)
        : std::runtime_error(message), kind_(kind) {}
    CompileErrorKind kind() const { return kind_; }

private:
    CompileErrorKind kind_;
};

namespace detail {

using Value = std::shared_ptr<const void>;
using Inputs = std::vector<Value>;

/// A readable name of `T` for error messages (not mangled).
template <class T>
std::string_view type_name() {
#if defined(__clang__) || defined(__GNUC__)
    std::string_view f = __PRETTY_FUNCTION__;
    auto start = f.find("T = ");
    if (start == std::string_view::npos) return typeid(T).name();
    start += 4;
    return f.substr(start, f.find_first_of(";]", start) - start);
#else
    return typeid(T).name();
#endif
}

struct TypeTag {
    std::type_index id;
    std::string_view name;
    template <class T>
    static TypeTag of() {
        return {std::type_index(typeid(T)), type_name<T>()};
    }
};

template <class T>
Value make_value(T&& v) {
    return std::make_shared<const std::decay_t<T>>(std::forward<T>(v));
}

struct Shared {
    std::string request_id;
    std::mutex mutex;
    bool frozen = false;
    JsonObject fields;

    JsonObject freeze() {
        std::lock_guard lock(mutex);
        frozen = true;
        return fields;
    }
};

}  // namespace detail

// ---- context ------------------------------------------------------------------

/// Request-scoped context; a node receives it as an optional last parameter.
class Ctx {
public:
    const std::string& request_id() const { return shared_->request_id; }

    /// Write a field into the run's record. Last write wins; writes after the run
    /// has returned are dropped.
    void set(std::string key, Json value) {
        std::lock_guard lock(shared_->mutex);
        if (shared_->frozen) return;
        for (auto& [k, v] : shared_->fields) {
            if (k == key) {
                v = std::move(value);
                return;
            }
        }
        shared_->fields.emplace_back(std::move(key), std::move(value));
    }

    /// Stop is requested when jabba stops waiting for this node. Cancellation is
    /// cooperative: long work should check it or `co_await ctx.stopped()`.
    const jabba::stop_token& stop_token() const { return token_; }
    jabba::stopped stopped() const { return jabba::stopped(token_); }

    Ctx(std::shared_ptr<detail::Shared> shared, jabba::stop_token token)
        : shared_(std::move(shared)), token_(std::move(token)) {}

private:
    std::shared_ptr<detail::Shared> shared_;
    jabba::stop_token token_;
};

// ---- nodes --------------------------------------------------------------------

/// A typed handle of a node in a graph under construction.
template <class T>
class NodeRef {
public:
    using value_type = T;
    explicit NodeRef(std::string name) : name_(std::move(name)) {}
    const std::string& name() const { return name_; }

private:
    std::string name_;
};

template <class... Ts>
struct Deps {
    std::array<std::string, sizeof...(Ts)> names;
};

/// Typed data dependencies: `deps(parse, features)`.
template <class... Ts>
Deps<Ts...> deps(const NodeRef<Ts>&... refs) {
    return Deps<Ts...>{{refs.name()...}};
}

template <class V>
struct DynDeps {
    std::vector<std::string> names;
};

/// Data dependencies by name, all of type `V`; checked at compile.
template <class V>
DynDeps<V> deps_dyn(std::vector<std::string> names) {
    return DynDeps<V>{std::move(names)};
}

/// Inputs of a node whose dependencies are given by name.
template <class V>
class DynInputs {
public:
    const V* get(std::string_view name) const {
        for (auto& [n, v] : items_) {
            if (n == name) return v.get();
        }
        return nullptr;
    }
    /// (dependency name, value) in declaration order.
    const std::vector<std::pair<std::string, std::shared_ptr<const V>>>& items() const { return items_; }

    DynInputs(const std::vector<std::string>& names, const detail::Inputs& values) {
        for (std::size_t i = 0; i < names.size(); ++i) {
            items_.emplace_back(names[i], std::static_pointer_cast<const V>(values[i]));
        }
    }

private:
    std::vector<std::pair<std::string, std::shared_ptr<const V>>> items_;
};

namespace detail {

using AsyncBody = std::function<task<Value>(const Inputs&, Ctx&)>;
using SyncBody = std::function<Value(const Inputs&, Ctx&)>;

struct NodeSpec {
    std::string name;
    std::vector<std::pair<std::string, TypeTag>> inputs;
    std::vector<std::string> after;
    std::variant<AsyncBody, SyncBody> body;
    TypeTag output = TypeTag::of<void>();
    bool optional = false;
    Value default_value;
    std::optional<std::chrono::nanoseconds> timeout;
};

/// The value type a node produces for a body result `R` (a task's value for async bodies).
template <class R>
using node_value_t = std::remove_cvref_t<std::conditional_t<std::is_void_v<R>, std::monostate, R>>;

template <class R>
using result_value_t = node_value_t<std::conditional_t<is_task_v<R>, typename task_value<R>::type, R>>;

template <class F, class... Args>
decltype(auto) call_maybe_ctx(const F& f, Ctx& ctx, Args&&... args) {
    if constexpr (std::is_invocable_v<const F&, Args..., Ctx&>) {
        return std::invoke(f, std::forward<Args>(args)..., ctx);
    } else {
        return std::invoke(f, std::forward<Args>(args)...);
    }
}

/// Wraps a body into the type-erased async or sync form. `Call` is a cheap,
/// const-callable `(inputs, ctx) -> Result`, where Result is `task<U>` or `U`.
template <class Result, class Call>
std::variant<AsyncBody, SyncBody> wrap_body(Call call) {
    if constexpr (is_task_v<Result>) {
        using U = typename Result::value_type;
        return AsyncBody([call = std::move(call)](const Inputs& in, Ctx& ctx) -> task<Value> {
            // Await named objects, not temporaries (some compilers mis-destroy those).
            auto t = call(in, ctx);
            if constexpr (std::is_void_v<U>) {
                co_await std::move(t);
                co_return make_value(std::monostate{});
            } else {
                auto v = co_await std::move(t);
                co_return make_value(std::move(v));
            }
        });
    } else {
        return SyncBody([call = std::move(call)](const Inputs& in, Ctx& ctx) -> Value {
            if constexpr (std::is_void_v<Result>) {
                call(in, ctx);
                return make_value(std::monostate{});
            } else {
                return make_value(call(in, ctx));
            }
        });
    }
}

}  // namespace detail

/// A complete node description, ready to be added to a graph. `T` is the value
/// type the node produces (`std::monostate` for bodies returning void).
template <class T>
class [[nodiscard]] NodeDef {
public:
    using value_type = T;

    /// Ordering-only dependencies: wait for these nodes, receive no data.
    NodeDef& after(std::vector<std::string> names) & {
        for (auto& n : names) spec_.after.push_back(std::move(n));
        return *this;
    }
    NodeDef&& after(std::vector<std::string> names) && { return std::move(after(std::move(names))); }

    /// A failure or timeout of this node does not fail the run: data dependents
    /// receive the default.
    NodeDef& optional() & {
        spec_.optional = true;
        return *this;
    }
    NodeDef&& optional() && { return std::move(optional()); }

    /// What data dependents receive if this optional node fails or times out.
    NodeDef& default_value(T value) & {
        spec_.default_value = detail::make_value(std::move(value));
        return *this;
    }
    NodeDef&& default_value(T value) && { return std::move(default_value(std::move(value))); }

    /// Per-node timeout, measured from the node's start.
    template <class Rep, class Period>
    NodeDef& timeout(std::chrono::duration<Rep, Period> t) & {
        spec_.timeout = std::chrono::duration_cast<std::chrono::nanoseconds>(t);
        return *this;
    }
    template <class Rep, class Period>
    NodeDef&& timeout(std::chrono::duration<Rep, Period> t) && {
        return std::move(timeout(t));
    }

    explicit NodeDef(detail::NodeSpec spec) : spec_(std::move(spec)) {}
    detail::NodeSpec& spec() { return spec_; }

private:
    detail::NodeSpec spec_;
};

/// A node with typed data dependencies: `node("score", deps(parse, side), f)`.
/// `f` receives `const T&` per dependency, optionally followed by `jabba::Ctx&`.
/// Runs call it concurrently, so it is called as const: no mutable lambdas.
template <class... Ts, class F>
auto node(std::string name, Deps<Ts...> deps, F f) {
    constexpr bool with_ctx = std::is_invocable_v<const F&, const Ts&..., Ctx&>;
    constexpr bool without_ctx = std::is_invocable_v<const F&, const Ts&...>;
    static_assert(with_ctx || without_ctx,
                  "jabba::node: the body must be callable as f(const Ts&...) or f(const Ts&..., jabba::Ctx&), "
                  "where Ts are the dependencies' value types, on a const object: runs call it "
                  "concurrently, so it cannot be a mutable lambda");
    using Result = typename std::conditional_t<with_ctx, std::invoke_result<const F&, const Ts&..., Ctx&>,
                                               std::invoke_result<const F&, const Ts&...>>::type;
    using T = detail::result_value_t<Result>;
    // Shared, so bodies may own move-only state (a unique_ptr to a client, say).
    auto fn = std::make_shared<const F>(std::move(f));
    auto call = [fn](const detail::Inputs& in, Ctx& ctx) -> Result {
        return [&]<std::size_t... I>(std::index_sequence<I...>) -> Result {
            return detail::call_maybe_ctx(*fn, ctx, *static_cast<const Ts*>(in[I].get())...);
        }(std::index_sequence_for<Ts...>{});
    };
    detail::NodeSpec spec;
    spec.name = std::move(name);
    spec.body = detail::wrap_body<Result>(std::move(call));
    spec.output = detail::TypeTag::of<T>();
    const std::array<detail::TypeTag, sizeof...(Ts)> tags{detail::TypeTag::of<Ts>()...};
    for (std::size_t i = 0; i < sizeof...(Ts); ++i) spec.inputs.emplace_back(deps.names[i], tags[i]);
    return NodeDef<T>(std::move(spec));
}

/// A node without data dependencies: `node("warmup", f)`.
template <class F>
auto node(std::string name, F f) {
    return node(std::move(name), Deps<>{}, std::move(f));
}

/// A node with dependencies by name: the body receives `const DynInputs<V>&`,
/// optionally followed by `jabba::Ctx&`.
template <class V, class F>
auto node(std::string name, DynDeps<V> deps, F f) {
    constexpr bool with_ctx = std::is_invocable_v<const F&, const DynInputs<V>&, Ctx&>;
    constexpr bool without_ctx = std::is_invocable_v<const F&, const DynInputs<V>&>;
    static_assert(with_ctx || without_ctx,
                  "jabba::node: a deps_dyn<V> body must be callable as f(const DynInputs<V>&) or "
                  "f(const DynInputs<V>&, jabba::Ctx&) on a const object (no mutable lambdas)");
    using Result = typename std::conditional_t<with_ctx, std::invoke_result<const F&, const DynInputs<V>&, Ctx&>,
                                               std::invoke_result<const F&, const DynInputs<V>&>>::type;
    using T = detail::result_value_t<Result>;
    auto fn = std::make_shared<const F>(std::move(f));
    auto names = std::make_shared<const std::vector<std::string>>(deps.names);
    detail::NodeSpec spec;
    // DynInputs must outlive the user's coroutine, so for async bodies it lives
    // in the frame of the wrapper coroutine that awaits it.
    if constexpr (is_task_v<Result>) {
        using U = typename Result::value_type;
        spec.body = detail::AsyncBody([fn, names](const detail::Inputs& in, Ctx& ctx) -> task<detail::Value> {
            DynInputs<V> inputs(*names, in);
            auto t = detail::call_maybe_ctx(*fn, ctx, inputs);
            if constexpr (std::is_void_v<U>) {
                co_await std::move(t);
                co_return detail::make_value(std::monostate{});
            } else {
                auto v = co_await std::move(t);
                co_return detail::make_value(std::move(v));
            }
        });
    } else {
        spec.body = detail::SyncBody([fn, names](const detail::Inputs& in, Ctx& ctx) -> detail::Value {
            DynInputs<V> inputs(*names, in);
            if constexpr (std::is_void_v<Result>) {
                detail::call_maybe_ctx(*fn, ctx, inputs);
                return detail::make_value(std::monostate{});
            } else {
                return detail::make_value(detail::call_maybe_ctx(*fn, ctx, inputs));
            }
        });
    }
    spec.name = std::move(name);
    spec.output = detail::TypeTag::of<T>();
    for (auto& n : *names) spec.inputs.emplace_back(n, detail::TypeTag::of<V>());
    return NodeDef<T>(std::move(spec));
}

/// Start `t` now and let it run to completion on its own. An exception escaping
/// `t` terminates the program. Use it to start a top-level coroutine that awaits
/// `graph.run(...)`.
inline void spawn(task<void> t) {
    [](task<void> owned) -> detail::detached { co_await std::move(owned); }(std::move(t));
}

// ---- compiled graph and runs --------------------------------------------------

struct RunOptions {
    /// Whole-graph deadline, measured from the start of the run.
    std::optional<std::chrono::nanoseconds> deadline{};
    /// Defaults to a generated unique id.
    std::optional<std::string> request_id{};
    /// Cancels the run from outside: nodes are cancelled and no record is produced.
    jabba::stop_token stop{};
};

/// Outcome of a run. `stat` is present unless the run was cancelled from outside.
template <class O>
struct RunResult {
    RunStatus status = RunStatus::ok;
    /// The output node's value on success; empty for graphs without an output.
    std::shared_ptr<const O> output;
    std::optional<RequestStat> stat;
    /// For a failed run: the non-optional node that failed or timed out, and why.
    std::string failed_node;
    std::string error;

    bool ok() const { return status == RunStatus::ok; }
};

namespace detail {

struct CompiledNode {
    std::string name;
    /// -1 is the request; otherwise a node index.
    std::vector<int> sources;
    std::variant<AsyncBody, SyncBody> body;
    bool optional = false;
    Value default_value;
    std::optional<std::chrono::nanoseconds> timeout;
};

struct GraphImpl {
    std::string name;
    std::vector<CompiledNode> nodes;
    std::vector<std::vector<std::size_t>> children;
    std::vector<std::size_t> indegree;
    int output = -1;
    std::function<void(const RequestStat&)> on_run_done;
};

struct Finished {
    RunStatus status = RunStatus::ok;
    Value output;
    std::optional<RequestStat> stat;
    std::string failed_node;
    std::string error;
};

inline double wall_clock() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

inline std::string new_request_id() {
    static std::atomic<std::uint64_t> counter{0};
    auto nanos = static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
    auto n = counter.fetch_add(1, std::memory_order_relaxed);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%016llx%016llx", static_cast<unsigned long long>(nanos),
                  static_cast<unsigned long long>(n));
    return buf;
}

inline std::string seconds(std::chrono::nanoseconds d) {
    Json value(std::chrono::duration<double>(d).count());
    return value.dump();
}

/// The state of one run. Event handlers change it under `mutex` and collect
/// what to do afterwards; node code, stop requests and callbacks run unlocked.
class RunState : public std::enable_shared_from_this<RunState> {
public:
    using Done = std::function<void(Finished)>;

    RunState(std::shared_ptr<const GraphImpl> graph, Executor& exec, Value request, RunOptions options,
             Done done)
        : graph_(std::move(graph)),
          exec_(&exec),
          request_(std::move(request)),
          options_(std::move(options)),
          done_(std::move(done)),
          shared_(std::make_shared<Shared>()) {
        auto n = graph_->nodes.size();
        shared_->request_id = options_.request_id ? *options_.request_id : new_request_id();
        results_.resize(n);
        pending_ = graph_->indegree;
        started_.assign(n, false);
        running_.assign(n, false);
        waiting_for_.assign(n, false);
        stats_.resize(n);
        start_times_.resize(n);
        inputs_.resize(n);
        ctxs_.resize(n);
        stops_.resize(n);
        node_timers_.resize(n);
    }

    void begin() {
        // Registered before the lock: it may fire right away, but its handler is
        // only posted. If that handler already finished the run, nothing starts.
        std::unique_ptr<stop_callback> outer;
        if (options_.stop.stop_possible()) {
            outer = std::make_unique<stop_callback>(options_.stop, signal([](RunState& s) { s.on_outer_stop(); }));
        }
        Actions actions;
        {
            std::lock_guard lock(mutex_);
            if (phase_ != Phase::running) return;
            outer_ = std::move(outer);
            start_ = Clock::now();
            if (options_.stop.stop_requested()) {
                // Cancelled before it started: start nothing.
                begin_finish(RunStatus::cancelled, actions);
            } else {
                if (options_.deadline && *options_.deadline < (Clock::time_point::max)() - start_) {
                    // (A deadline too far to represent means no deadline.)
                    deadline_at_ = start_ + std::chrono::duration_cast<Clock::duration>(*options_.deadline);
                    deadline_timer_ = Timer::instance().add(*deadline_at_, signal([](RunState& s) { s.on_deadline(); }));
                }
                if (!collect_ready() && active_ == 0) finalize(RunStatus::ok, actions);
            }
        }
        apply(std::move(actions));
    }

private:
    using Clock = detail::Clock;
    enum class Phase { running, finishing, done };

    struct Actions {
        std::vector<std::shared_ptr<stop_source>> stops;
        std::optional<Finished> finished;
        std::unique_ptr<stop_callback> outer;
    };

    /// A callback for the timer thread or a stop callback: post `handler` to the
    /// executor unless the run is already over. Never runs node code itself.
    std::function<void()> signal(void (*handler)(RunState&)) {
        std::weak_ptr<RunState> weak = weak_from_this();
        return [weak, handler] {
            auto self = weak.lock();
            if (!self) return;
            std::lock_guard lock(self->mutex_);
            if (self->phase_ == Phase::done) return;
            self->exec_->post([self, handler] { handler(*self); });
        };
    }

    /// What a handler decided, done outside the lock. The result goes out first:
    /// stop callbacks (and code resumed by `ctx.stopped()`) run on this thread and
    /// must not delay it. Hooks and callbacks must not throw; if they do, terminate.
    void apply(Actions actions) {
        if (actions.finished) {
            auto& f = *actions.finished;
            if (f.stat && graph_->on_run_done) [&]() noexcept { graph_->on_run_done(*f.stat); }();
            [&]() noexcept { done_(std::move(f)); }();
        }
        actions.outer.reset();
        for (auto& s : actions.stops) s->request_stop();
    }

    /// Start every node whose dependencies are done; returns whether any started.
    /// Called under the lock, and posts under it: once the run is finished nothing
    /// may reach the executor, which the caller is free to destroy by then.
    bool collect_ready() {
        std::vector<std::size_t> launch;
        for (std::size_t i = 0; i < graph_->nodes.size(); ++i) {
            if (started_[i] || pending_[i] != 0) continue;
            auto& node = graph_->nodes[i];
            started_[i] = true;
            running_[i] = true;
            ++active_;
            start_times_[i] = Clock::now();
            NodeStat stat;
            stat.status = NodeStatus::ok;
            stat.started_at = wall_clock();
            stats_[i] = stat;
            Inputs in;
            for (int src : node.sources) in.push_back(src < 0 ? request_ : results_[static_cast<std::size_t>(src)]);
            inputs_[i] = std::move(in);
            stops_[i] = std::make_shared<stop_source>();
            ctxs_[i] = std::make_unique<Ctx>(shared_, stops_[i]->get_token());
            if (node.timeout && *node.timeout < (Clock::time_point::max)() - start_times_[i]) {
                auto at = start_times_[i] + std::chrono::duration_cast<Clock::duration>(*node.timeout);
                std::weak_ptr<RunState> weak = weak_from_this();
                node_timers_[i] = Timer::instance().add(at, [weak, i] {
                    auto self = weak.lock();
                    if (!self) return;
                    std::lock_guard lock(self->mutex_);
                    if (self->phase_ == Phase::done) return;
                    self->exec_->post([self, i] { self->on_node_timeout(i); });
                });
            }
            launch.push_back(i);
        }
        if (launch.empty()) return false;
        exec_->post([self = shared_from_this(), launch] {
            for (auto i : launch) self->launch(i);
        });
        return true;
    }

    void launch(std::size_t i) {
        {
            // Cancelled between being collected and being run: no user code then.
            std::unique_lock lock(mutex_);
            if (phase_ == Phase::running && running_[i]) {
                lock.unlock();
            } else {
                Actions actions;
                settle_cancelled(i, actions);
                lock.unlock();
                apply(std::move(actions));
                return;
            }
        }
        auto& node = graph_->nodes[i];
        if (auto* sync = std::get_if<SyncBody>(&node.body)) {
            Value value;
            std::exception_ptr error;
            try {
                value = (*sync)(inputs_[i], *ctxs_[i]);
            } catch (...) {
                error = std::current_exception();
            }
            bool overran = node.timeout && Clock::now() - start_times_[i] > *node.timeout;
            on_node_done(i, std::move(value), error, overran);
        } else {
            drive(shared_from_this(), i);
        }
    }

    /// Runs one async node; owns itself and the run state until the node finishes.
    static detached drive(std::shared_ptr<RunState> self, std::size_t i) {
        Value value;
        std::exception_ptr error;
        try {
            auto& body = std::get<AsyncBody>(self->graph_->nodes[i].body);
            auto t = body(self->inputs_[i], *self->ctxs_[i]);
            value = co_await std::move(t);
        } catch (...) {
            error = std::current_exception();
        }
        self->on_node_done(i, std::move(value), error, false);
    }

    void on_node_done(std::size_t i, Value value, std::exception_ptr error, bool overran) {
        Actions actions;
        {
            std::lock_guard lock(mutex_);
            if (phase_ == Phase::done) return;
            if (!running_[i]) {
                // Already timed out or cancelled: the result is discarded.
                settle_cancelled(i, actions);
            } else {
                stop_node(i, actions);
                if (overran) {
                    // A sync node could not be interrupted, but it did overrun.
                    auto t = *graph_->nodes[i].timeout;
                    record(i, NodeStatus::timeout, ErrorKind::timeout, "node timeout after " + seconds(t) + "s");
                    settle_error(i, "node '" + graph_->nodes[i].name + "' timed out after " + seconds(t) + "s");
                } else if (error) {
                    auto message = describe(error);
                    record(i, NodeStatus::failed, ErrorKind::failed, message);
                    settle_error(i, message);
                } else {
                    record_ok(i);
                    results_[i] = std::move(value);
                    release(i);
                }
                advance(actions);
            }
        }
        apply(std::move(actions));
    }

    void on_node_timeout(std::size_t i) {
        Actions actions;
        {
            std::lock_guard lock(mutex_);
            if (phase_ != Phase::running || !running_[i]) return;
            stop_node(i, actions);
            actions.stops.push_back(stops_[i]);
            auto t = *graph_->nodes[i].timeout;
            record(i, NodeStatus::timeout, ErrorKind::timeout, "node timeout after " + seconds(t) + "s");
            settle_error(i, "node '" + graph_->nodes[i].name + "' timed out after " + seconds(t) + "s");
            advance(actions);
        }
        apply(std::move(actions));
    }

    void on_deadline() {
        Actions actions;
        {
            std::lock_guard lock(mutex_);
            if (phase_ != Phase::running) return;
            deadline_reached(actions);
        }
        apply(std::move(actions));
    }

    void on_outer_stop() {
        Actions actions;
        {
            std::lock_guard lock(mutex_);
            if (phase_ != Phase::running) return;
            begin_finish(RunStatus::cancelled, actions);
        }
        apply(std::move(actions));
    }

    void on_grace() {
        Actions actions;
        {
            std::lock_guard lock(mutex_);
            if (phase_ != Phase::finishing) return;
            finalize(finish_status_, actions);
        }
        apply(std::move(actions));
    }

    /// After a node settled: fail fast, time out, start what is ready, or finish.
    void advance(Actions& actions) {
        if (failure_) {
            for (std::size_t j = 0; j < running_.size(); ++j) {
                if (running_[j]) record(j, NodeStatus::skipped, ErrorKind::cancelled, "cancelled");
            }
            begin_finish(RunStatus::failed, actions);
            return;
        }
        if (deadline_at_ && Clock::now() >= *deadline_at_) {
            deadline_reached(actions);
            return;
        }
        if (!collect_ready() && active_ == 0) finalize(RunStatus::ok, actions);
    }

    /// A cancelled node has stopped: once all have, a finishing run is over.
    void settle_cancelled(std::size_t i, Actions& actions) {
        if (!waiting_for_[i]) return;
        waiting_for_[i] = false;
        if (--waiting_ == 0 && phase_ == Phase::finishing) finalize(finish_status_, actions);
    }

    void deadline_reached(Actions& actions) {
        auto message = "graph deadline after " + seconds(*options_.deadline) + "s";
        for (std::size_t j = 0; j < running_.size(); ++j) {
            if (running_[j]) record(j, NodeStatus::timeout, ErrorKind::deadline, message);
        }
        begin_finish(RunStatus::timeout, actions);
    }

    /// Cancel every running node and wait for them at most `cancel_grace`.
    void begin_finish(RunStatus status, Actions& actions) {
        phase_ = Phase::finishing;
        finish_status_ = status;
        for (std::size_t j = 0; j < running_.size(); ++j) {
            if (!running_[j]) continue;
            stop_node(j, actions);
            actions.stops.push_back(stops_[j]);
            waiting_for_[j] = true;
            ++waiting_;
        }
        cancel_timer(deadline_timer_);
        if (waiting_ == 0) {
            finalize(status, actions);
        } else {
            grace_timer_ = Timer::instance().add(Clock::now() + cancel_grace,
                                                 signal([](RunState& s) { s.on_grace(); }));
        }
    }

    void finalize(RunStatus status, Actions& actions) {
        phase_ = Phase::done;
        cancel_timer(deadline_timer_);
        cancel_timer(grace_timer_);
        for (auto& t : node_timers_) cancel_timer(t);
        Finished f;
        f.status = status;
        if (status != RunStatus::cancelled) {
            RequestStat stat;
            stat.request_id = shared_->request_id;
            stat.graph = graph_->name;
            stat.status = status;
            stat.duration_ms = std::chrono::duration<double, std::milli>(Clock::now() - start_).count();
            for (std::size_t j = 0; j < graph_->nodes.size(); ++j) {
                stat.nodes.emplace_back(graph_->nodes[j].name, stats_[j] ? *stats_[j] : NodeStat{});
            }
            stat.fields = shared_->freeze();
            f.stat = std::move(stat);
        } else {
            shared_->freeze();
        }
        if (status == RunStatus::ok && graph_->output >= 0) f.output = results_[static_cast<std::size_t>(graph_->output)];
        if (failure_) {
            f.failed_node = graph_->nodes[failure_->first].name;
            f.error = failure_->second;
        }
        actions.finished = std::move(f);
        actions.outer = std::move(outer_);
    }

    void stop_node(std::size_t i, Actions&) {
        running_[i] = false;
        --active_;
        cancel_timer(node_timers_[i]);
    }

    void settle_error(std::size_t i, std::string message) {
        auto& node = graph_->nodes[i];
        if (node.optional) {
            results_[i] = node.default_value;
            release(i);
        } else if (!failure_) {
            failure_ = std::make_pair(i, std::move(message));
        }
    }

    void release(std::size_t i) {
        for (auto child : graph_->children[i]) --pending_[child];
    }

    void record_ok(std::size_t i) { stats_[i]->duration_ms = elapsed_ms(i); }

    void record(std::size_t i, NodeStatus status, ErrorKind kind, std::string error) {
        auto& s = *stats_[i];
        s.status = status;
        s.error_kind = kind;
        s.error = std::move(error);
        s.duration_ms = elapsed_ms(i);
    }

    double elapsed_ms(std::size_t i) const {
        return std::chrono::duration<double, std::milli>(Clock::now() - start_times_[i]).count();
    }

    static void cancel_timer(std::optional<TimerKey>& key) {
        if (key) {
            Timer::instance().cancel(*key);
            key.reset();
        }
    }

    static std::string describe(const std::exception_ptr& error) {
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& e) {
            return e.what();
        } catch (...) {
            return "unknown exception";
        }
    }

    std::shared_ptr<const GraphImpl> graph_;
    Executor* exec_;
    Value request_;
    RunOptions options_;
    Done done_;
    std::shared_ptr<Shared> shared_;

    std::mutex mutex_;
    Phase phase_ = Phase::running;
    Clock::time_point start_;
    std::optional<Clock::time_point> deadline_at_;
    std::vector<Value> results_;
    std::vector<std::size_t> pending_;
    std::vector<bool> started_, running_, waiting_for_;
    std::vector<std::optional<NodeStat>> stats_;
    std::vector<Clock::time_point> start_times_;
    std::vector<Inputs> inputs_;
    std::vector<std::unique_ptr<Ctx>> ctxs_;
    std::vector<std::shared_ptr<stop_source>> stops_;
    std::vector<std::optional<TimerKey>> node_timers_;
    std::optional<TimerKey> deadline_timer_, grace_timer_;
    std::unique_ptr<stop_callback> outer_;
    std::optional<std::pair<std::size_t, std::string>> failure_;
    RunStatus finish_status_ = RunStatus::ok;
    std::size_t active_ = 0, waiting_ = 0;
};

/// The executor behind run_blocking: a queue drained by the calling thread.
class QueueExecutor : public Executor {
public:
    // Notify under the lock: once `finished_` is visible, run_blocking may return and
    // destroy this queue, so nothing may touch it after the lock is released.
    void post(std::function<void()> fn) override {
        std::lock_guard lock(mutex_);
        tasks_.push_back(std::move(fn));
        changed_.notify_one();
    }

    void finish() {
        std::lock_guard lock(mutex_);
        finished_ = true;
        changed_.notify_one();
    }

    void run_until_finished() {
        std::unique_lock lock(mutex_);
        while (true) {
            changed_.wait(lock, [&] { return !tasks_.empty() || finished_; });
            if (finished_) return;
            auto fn = std::move(tasks_.front());
            tasks_.pop_front();
            lock.unlock();
            fn();
            lock.lock();
        }
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::function<void()>> tasks_;
    bool finished_ = false;
};

}  // namespace detail

/// An immutable, validated graph. Cheap to copy; serves concurrent runs.
/// `O` is the output type (`std::monostate` for graphs without an output).
template <class R, class O = std::monostate>
class CompiledGraph {
public:
    const std::string& name() const { return impl_->name; }

    /// Called once per run that produces a record, after the record is frozen,
    /// on the thread where the run completed. Must not throw.
    CompiledGraph on_run_done(std::function<void(const RequestStat&)> hook) const {
        auto impl = std::make_shared<detail::GraphImpl>(*impl_);
        impl->on_run_done = std::move(hook);
        return CompiledGraph(std::move(impl));
    }

    /// Start a run on `exec`; `done` is called once, on the thread where the run completed.
    /// `exec` must outlive the run.
    void run_async(R request, Executor& exec, RunOptions options, std::function<void(RunResult<O>)> done) const {
        auto state = std::make_shared<detail::RunState>(
            impl_, exec, detail::make_value(std::move(request)), std::move(options),
            [done = std::move(done)](detail::Finished f) mutable { done(convert(std::move(f))); });
        state->begin();
    }

    /// `co_await graph.run(request, exec)`: the awaiting coroutine resumes on `exec`.
    auto run(R request, Executor& exec, RunOptions options = {}) const {
        struct Awaiter {
            CompiledGraph graph;
            R request;
            Executor* exec;
            RunOptions options;
            std::optional<RunResult<O>> result;

            bool await_ready() const noexcept { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                graph.run_async(std::move(request), *exec, std::move(options), [this, h](RunResult<O> r) {
                    result = std::move(r);
                    exec->post([h] { h.resume(); });
                });
            }
            RunResult<O> await_resume() { return std::move(*result); }
        };
        return Awaiter{*this, std::move(request), &exec, std::move(options), std::nullopt};
    }

    /// Run on the calling thread until the run finishes. Do not call it from a
    /// thread whose event loop the nodes need: that deadlocks.
    RunResult<O> run_blocking(R request, RunOptions options = {}) const {
        detail::QueueExecutor queue;
        std::optional<RunResult<O>> result;
        std::mutex m;
        run_async(std::move(request), queue, std::move(options), [&](RunResult<O> r) {
            {
                std::lock_guard lock(m);
                result = std::move(r);
            }
            queue.finish();
        });
        queue.run_until_finished();
        std::lock_guard lock(m);
        return std::move(*result);
    }

private:
    template <class>
    friend class GraphBuilder;
    explicit CompiledGraph(std::shared_ptr<const detail::GraphImpl> impl) : impl_(std::move(impl)) {}

    static RunResult<O> convert(detail::Finished f) {
        RunResult<O> r;
        r.status = f.status;
        if (f.output) r.output = std::static_pointer_cast<const O>(f.output);
        r.stat = std::move(f.stat);
        r.failed_node = std::move(f.failed_node);
        r.error = std::move(f.error);
        return r;
    }

    std::shared_ptr<const detail::GraphImpl> impl_;
};

/// Mutable, build-time graph assembly; compile it once into a CompiledGraph.
template <class R>
class GraphBuilder {
public:
    explicit GraphBuilder(std::string name) : name_(std::move(name)) {}

    /// Handle of the run's request, usable as a dependency.
    NodeRef<R> request() const { return NodeRef<R>(std::string(request_name)); }

    /// Add a node. Problems (reserved or duplicate names) are reported by compile.
    template <class T>
    NodeRef<T> add(NodeDef<T> def) {
        auto& spec = def.spec();
        std::string name = spec.name;
        if (name == request_name || name == ctx_name) {
            fail(CompileErrorKind::reserved_name, "node name '" + name + "' is reserved");
        } else {
            for (auto& n : nodes_) {
                if (n.name == name) fail(CompileErrorKind::duplicate_name, "duplicate node name '" + name + "'");
            }
        }
        nodes_.push_back(std::move(spec));
        return NodeRef<T>(std::move(name));
    }

    /// Compile a graph without an output node.
    CompiledGraph<R> compile() const { return CompiledGraph<R>(build("", detail::TypeTag::of<std::monostate>())); }

    /// Compile a graph whose result is the value of `output`.
    template <class O>
    CompiledGraph<R, O> compile_with_output(const NodeRef<O>& output) const {
        return CompiledGraph<R, O>(build(output.name(), detail::TypeTag::of<O>()));
    }

    /// Like compile_with_output, with the output node given by name; its type is checked.
    template <class O>
    CompiledGraph<R, O> compile_with_output_named(const std::string& output) const {
        return CompiledGraph<R, O>(build(output, detail::TypeTag::of<O>()));
    }

private:
    void fail(CompileErrorKind kind, std::string message) {
        if (!first_error_) first_error_ = std::make_pair(kind, "graph '" + name_ + "': " + message);
    }

    [[noreturn]] void error(CompileErrorKind kind, const std::string& message) const {
        throw CompileError(kind, "graph '" + name_ + "': " + message);
    }

    std::shared_ptr<const detail::GraphImpl> build(const std::string& output, detail::TypeTag output_type) const {
        if (first_error_) throw CompileError(first_error_->first, first_error_->second);
        std::map<std::string, std::size_t, std::less<>> index;
        for (std::size_t i = 0; i < nodes_.size(); ++i) index.emplace(nodes_[i].name, i);
        auto request_type = detail::TypeTag::of<R>();

        auto impl = std::make_shared<detail::GraphImpl>();
        impl->name = name_;
        for (auto& e : nodes_) {
            detail::CompiledNode node{e.name, {}, e.body, e.optional, e.default_value, e.timeout};
            for (auto& [dep, expected] : e.inputs) {
                int source;
                detail::TypeTag provided = request_type;
                if (dep == request_name) {
                    source = -1;
                } else if (auto it = index.find(dep); it != index.end()) {
                    source = static_cast<int>(it->second);
                    provided = nodes_[it->second].output;
                } else {
                    error(CompileErrorKind::unknown_dep, "node '" + e.name + "' has dependency on unknown node '" + dep + "'");
                }
                if (provided.id != expected.id) {
                    error(CompileErrorKind::type_mismatch, "node '" + e.name + "' expects '" + dep + "' as " +
                                                               std::string(expected.name) + ", but it provides " +
                                                               std::string(provided.name));
                }
                node.sources.push_back(source);
            }
            for (auto& dep : e.after) {
                if (!index.count(dep)) {
                    error(CompileErrorKind::unknown_after, "node '" + e.name + "' has after on unknown node '" + dep + "'");
                }
            }
            impl->nodes.push_back(std::move(node));
        }
        if (!output.empty()) {
            auto it = index.find(output);
            if (it == index.end()) error(CompileErrorKind::unknown_output, "output points to unknown node '" + output + "'");
            if (nodes_[it->second].output.id != output_type.id) {
                error(CompileErrorKind::type_mismatch, "output node '" + output + "' returns " +
                                                           std::string(nodes_[it->second].output.name) +
                                                           ", but the graph output is " + std::string(output_type.name));
            }
            impl->output = static_cast<int>(it->second);
        }

        // Optional nodes must declare what their data dependents receive.
        for (std::size_t c = 0; c < impl->nodes.size(); ++c) {
            for (int src : impl->nodes[c].sources) {
                if (src < 0) continue;
                auto& provider = nodes_[static_cast<std::size_t>(src)];
                if (provider.optional && !provider.default_value) {
                    error(CompileErrorKind::optional_without_default,
                          "optional node '" + provider.name + "' has data dependents but declares no default");
                }
            }
        }
        if (impl->output >= 0) {
            auto& out = nodes_[static_cast<std::size_t>(impl->output)];
            if (out.optional && !out.default_value) {
                error(CompileErrorKind::optional_output_without_default,
                      "optional output node '" + out.name + "' declares no default");
            }
        }

        // Edges, then cycle detection (Kahn).
        auto n = nodes_.size();
        impl->children.assign(n, {});
        impl->indegree.assign(n, 0);
        for (std::size_t c = 0; c < n; ++c) {
            std::vector<std::size_t> parents;
            for (int src : impl->nodes[c].sources) {
                if (src >= 0) parents.push_back(static_cast<std::size_t>(src));
            }
            for (auto& dep : nodes_[c].after) parents.push_back(index.find(dep)->second);
            for (auto p : parents) {
                impl->children[p].push_back(c);
                ++impl->indegree[c];
            }
        }
        auto remaining = impl->indegree;
        std::vector<std::size_t> ready;
        for (std::size_t i = 0; i < n; ++i) {
            if (remaining[i] == 0) ready.push_back(i);
        }
        std::size_t visited = 0;
        while (!ready.empty()) {
            auto i = ready.back();
            ready.pop_back();
            ++visited;
            for (auto c : impl->children[i]) {
                if (--remaining[c] == 0) ready.push_back(c);
            }
        }
        if (visited != n) {
            std::string stuck;
            for (std::size_t i = 0; i < n; ++i) {
                if (remaining[i] > 0) stuck += (stuck.empty() ? "" : ", ") + nodes_[i].name;
            }
            error(CompileErrorKind::cycle, "cycle involving nodes [" + stuck + "]");
        }
        return impl;
    }

    std::string name_;
    std::vector<detail::NodeSpec> nodes_;
    std::optional<std::pair<CompileErrorKind, std::string>> first_error_;
};

}  // namespace jabba
