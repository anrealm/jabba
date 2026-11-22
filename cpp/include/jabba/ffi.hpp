// SPDX-License-Identifier: MIT
// Foreign nodes over the C ABI (jabba_ffi.h, spec/ffi.md): use a node implemented in
// another language in a C++ graph (`foreign`), or export a C++ node (`export_node`).
#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "graph.hpp"
#include "jabba_ffi.h"
#include "json.hpp"

namespace jabba::ffi {

// ---- the adapter's executor ------------------------------------------------------

/// A small process-wide thread pool that outlives every run. Foreign calls continue
/// on it, and exported nodes run on it when their host provides no `post`.
/// Deliberately leaked: calls may still use it while static objects are destroyed.
class Host final : public Executor {
public:
    static Host& instance() {
        static Host* host = new Host(std::clamp<unsigned>(std::thread::hardware_concurrency(), 2u, 8u));
        return *host;
    }

    void post(std::function<void()> fn) override {
        std::lock_guard lock(mutex_);
        tasks_.push_back(std::move(fn));
        changed_.notify_one();
    }

    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

private:
    explicit Host(unsigned threads) {
        for (unsigned i = 0; i < threads; ++i) {
            std::thread([this] {
                std::unique_lock lock(mutex_);
                while (true) {
                    changed_.wait(lock, [&] { return !tasks_.empty(); });
                    auto fn = std::move(tasks_.front());
                    tasks_.pop_front();
                    lock.unlock();
                    fn();
                    lock.lock();
                }
            }).detach();
        }
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::function<void()>> tasks_;
};

// ---- values ----------------------------------------------------------------------

/// A numeric array received over the ABI without copying; keeps the foreign owner's
/// memory alive until the last copy of the Array is gone.
template <class E>
class Array {
public:
    Array() = default;
    explicit Array(std::vector<E> owned) {
        auto v = std::make_shared<std::vector<E>>(std::move(owned));
        data_ = v->data();
        size_ = v->size();
        keep_ = std::move(v);
    }
    Array(const E* data, std::size_t size, std::shared_ptr<const void> keep)
        : keep_(std::move(keep)), data_(data), size_(size) {}

    const E* data() const { return data_; }
    std::size_t size() const { return size_; }
    const E* begin() const { return data_; }
    const E* end() const { return data_ + size_; }
    const E& operator[](std::size_t i) const { return data_[i]; }
    std::vector<E> to_vector() const { return std::vector<E>(begin(), end()); }

private:
    std::shared_ptr<const void> keep_;
    const E* data_ = nullptr;
    std::size_t size_ = 0;
};

namespace detail {

inline void release(const jabba_value& v) {
    if (v.release) v.release(v.owner);
}

inline jabba_value scalar(uint32_t kind) {
    jabba_value v;
    std::memset(&v, 0, sizeof v);
    v.kind = kind;
    return v;
}

inline jabba_value buffer(uint32_t kind, const void* data, std::size_t len) {
    auto v = scalar(kind);
    v.as.buf.data = data;
    v.as.buf.len = len;
    return v;
}

/// An owned value: `holder` keeps the memory and is deleted by release.
template <class H>
jabba_value owned(uint32_t kind, H* holder, const void* data, std::size_t len) {
    auto v = buffer(kind, data, len);
    v.owner = holder;
    v.release = [](void* owner) { delete static_cast<H*>(owner); };
    return v;
}

inline std::string kind_name(uint32_t k) {
    static const char* names[] = {"null", "bool", "int", "float", "str", "bytes", "json", "f64s", "f32s", "i64s"};
    auto base = k & JABBA_KIND_MASK;
    std::string n = base < sizeof names / sizeof *names ? names[base] : "kind " + std::to_string(base);
    return (k & JABBA_KIND_NULLABLE) ? n + "?" : n;
}

/// A value of any kind as JSON (what set_field writes into the record).
inline Json to_json(const jabba_value& v) {
    switch (v.kind & JABBA_KIND_MASK) {
        case JABBA_NULL: return Json();
        case JABBA_BOOL: return Json(v.as.i != 0);
        case JABBA_INT: return Json(v.as.i);
        case JABBA_FLOAT: return Json(v.as.f);
        case JABBA_STR:
            return Json(std::string(static_cast<const char*>(v.as.buf.data), v.as.buf.len));
        case JABBA_BYTES: {  // not text: a list of byte values
            JsonArray a;
            auto* d = static_cast<const std::uint8_t*>(v.as.buf.data);
            for (std::size_t i = 0; i < v.as.buf.len; ++i) a.emplace_back(d[i]);
            return a;
        }
        case JABBA_JSON:
            return parse_json(std::string_view(static_cast<const char*>(v.as.buf.data), v.as.buf.len));
        case JABBA_F64S: {
            JsonArray a;
            auto* d = static_cast<const double*>(v.as.buf.data);
            for (std::size_t i = 0; i < v.as.buf.len; ++i) a.emplace_back(d[i]);
            return a;
        }
        case JABBA_F32S: {
            JsonArray a;
            auto* d = static_cast<const float*>(v.as.buf.data);
            for (std::size_t i = 0; i < v.as.buf.len; ++i) a.emplace_back(d[i]);
            return a;
        }
        case JABBA_I64S: {
            JsonArray a;
            auto* d = static_cast<const std::int64_t*>(v.as.buf.data);
            for (std::size_t i = 0; i < v.as.buf.len; ++i) a.emplace_back(d[i]);
            return a;
        }
        default: return Json();
    }
}

template <class E>
constexpr uint32_t array_kind() {
    if constexpr (std::is_same_v<E, double>) return JABBA_F64S;
    else if constexpr (std::is_same_v<E, float>) return JABBA_F32S;
    else return JABBA_I64S;
}

}  // namespace detail

/// How a C++ type crosses the ABI. Specializations: bool, std::int64_t, int, double,
/// std::string, std::vector<std::uint8_t> (bytes), Json, std::vector<double|float|int64_t>,
/// Array<double|float|int64_t> (zero-copy outputs), std::monostate (null), std::optional<T>.
///
///   kind        — the declared jabba_kind
///   view(v, s)  — a borrowed value pointing into `v` (scratch `s` may hold temporaries)
///   own(v)      — an owned value that its receiver releases
///   read(v)     — decode a borrowed value (copies)
///   take(v)     — decode a value and release it (zero-copy where the type allows)
template <class T, class = void>
struct Codec;

using Scratch = std::deque<std::string>;

template <>
struct Codec<bool> {
    static constexpr uint32_t kind = JABBA_BOOL;
    static jabba_value view(const bool& b, Scratch&) { return own(b); }
    static jabba_value own(bool b) {
        auto v = detail::scalar(kind);
        v.as.i = b ? 1 : 0;
        return v;
    }
    static bool read(const jabba_value& v) { return v.as.i != 0; }
    static bool take(const jabba_value& v) { return read(v); }
};

template <class I>
struct Codec<I, std::enable_if_t<std::is_same_v<I, std::int64_t> || std::is_same_v<I, int>>> {
    static constexpr uint32_t kind = JABBA_INT;
    static jabba_value view(const I& i, Scratch&) { return own(i); }
    static jabba_value own(I i) {
        auto v = detail::scalar(kind);
        v.as.i = i;
        return v;
    }
    static I read(const jabba_value& v) { return static_cast<I>(v.as.i); }
    static I take(const jabba_value& v) { return read(v); }
};

template <>
struct Codec<double> {
    static constexpr uint32_t kind = JABBA_FLOAT;
    static jabba_value view(const double& d, Scratch&) { return own(d); }
    static jabba_value own(double d) {
        auto v = detail::scalar(kind);
        v.as.f = d;
        return v;
    }
    static double read(const jabba_value& v) { return v.as.f; }
    static double take(const jabba_value& v) { return read(v); }
};

template <>
struct Codec<std::string> {
    static constexpr uint32_t kind = JABBA_STR;
    static jabba_value view(const std::string& s, Scratch&) { return detail::buffer(kind, s.data(), s.size()); }
    static jabba_value own(std::string s) {
        auto* h = new std::string(std::move(s));
        return detail::owned(kind, h, h->data(), h->size());
    }
    static std::string read(const jabba_value& v) {
        return std::string(static_cast<const char*>(v.as.buf.data), v.as.buf.len);
    }
    static std::string take(const jabba_value& v) {
        auto s = read(v);
        detail::release(v);
        return s;
    }
};

template <>
struct Codec<std::vector<std::uint8_t>> {
    static constexpr uint32_t kind = JABBA_BYTES;
    static jabba_value view(const std::vector<std::uint8_t>& b, Scratch&) {
        return detail::buffer(kind, b.data(), b.size());
    }
    static jabba_value own(std::vector<std::uint8_t> b) {
        auto* h = new std::vector<std::uint8_t>(std::move(b));
        return detail::owned(kind, h, h->data(), h->size());
    }
    static std::vector<std::uint8_t> read(const jabba_value& v) {
        auto* d = static_cast<const std::uint8_t*>(v.as.buf.data);
        return std::vector<std::uint8_t>(d, d + v.as.buf.len);
    }
    static std::vector<std::uint8_t> take(const jabba_value& v) {
        auto b = read(v);
        detail::release(v);
        return b;
    }
};

template <>
struct Codec<Json> {
    static constexpr uint32_t kind = JABBA_JSON;
    static jabba_value view(const Json& j, Scratch& s) {
        auto& text = s.emplace_back(j.dump());
        return detail::buffer(kind, text.data(), text.size());
    }
    static jabba_value own(const Json& j) {
        auto* h = new std::string(j.dump());
        return detail::owned(kind, h, h->data(), h->size());
    }
    static Json read(const jabba_value& v) {
        return parse_json(std::string_view(static_cast<const char*>(v.as.buf.data), v.as.buf.len));
    }
    static Json take(const jabba_value& v) {
        struct Guard {
            const jabba_value& v;
            ~Guard() { detail::release(v); }
        } guard{v};
        return read(v);
    }
};

template <class E>
struct Codec<std::vector<E>, std::enable_if_t<std::is_same_v<E, double> || std::is_same_v<E, float> ||
                                              std::is_same_v<E, std::int64_t>>> {
    static constexpr uint32_t kind = detail::array_kind<E>();
    static jabba_value view(const std::vector<E>& a, Scratch&) { return detail::buffer(kind, a.data(), a.size()); }
    static jabba_value own(std::vector<E> a) {
        auto* h = new std::vector<E>(std::move(a));
        return detail::owned(kind, h, h->data(), h->size());
    }
    static std::vector<E> read(const jabba_value& v) {
        auto* d = static_cast<const E*>(v.as.buf.data);
        return std::vector<E>(d, d + v.as.buf.len);
    }
    static std::vector<E> take(const jabba_value& v) {
        auto a = read(v);
        detail::release(v);
        return a;
    }
};

template <class E>
struct Codec<Array<E>> {
    static constexpr uint32_t kind = detail::array_kind<E>();
    static jabba_value view(const Array<E>& a, Scratch&) { return detail::buffer(kind, a.data(), a.size()); }
    static jabba_value own(Array<E> a) {
        auto* h = new Array<E>(std::move(a));
        return detail::owned(kind, h, h->data(), h->size());
    }
    static Array<E> read(const jabba_value& v) { return Array<E>(Codec<std::vector<E>>::read(v)); }
    /// Zero-copy: the Array keeps the foreign memory and releases it when gone.
    static Array<E> take(const jabba_value& v) {
        std::shared_ptr<const void> keep;
        if (v.release) {
            keep = std::shared_ptr<const void>(v.owner, [release = v.release](const void* owner) {
                release(const_cast<void*>(owner));
            });
        }
        return Array<E>(static_cast<const E*>(v.as.buf.data), v.as.buf.len, std::move(keep));
    }
};

template <>
struct Codec<std::monostate> {
    static constexpr uint32_t kind = JABBA_NULL;
    static jabba_value view(const std::monostate&, Scratch&) { return detail::scalar(kind); }
    static jabba_value own(std::monostate) { return detail::scalar(kind); }
    static std::monostate read(const jabba_value&) { return {}; }
    static std::monostate take(const jabba_value& v) {
        detail::release(v);
        return {};
    }
};

template <class T>
struct Codec<std::optional<T>> {
    static constexpr uint32_t kind = Codec<T>::kind | JABBA_KIND_NULLABLE;
    static jabba_value view(const std::optional<T>& o, Scratch& s) {
        return o ? Codec<T>::view(*o, s) : detail::scalar(JABBA_NULL);
    }
    static jabba_value own(std::optional<T> o) {
        return o ? Codec<T>::own(std::move(*o)) : detail::scalar(JABBA_NULL);
    }
    static std::optional<T> read(const jabba_value& v) {
        if ((v.kind & JABBA_KIND_MASK) == JABBA_NULL) return std::nullopt;
        return Codec<T>::read(v);
    }
    static std::optional<T> take(const jabba_value& v) {
        if ((v.kind & JABBA_KIND_MASK) == JABBA_NULL) {
            detail::release(v);
            return std::nullopt;
        }
        return Codec<T>::take(v);
    }
};

namespace detail {

/// Is a value of kind `actual` acceptable where `declared` is expected?
inline bool value_fits(uint32_t actual, uint32_t declared) {
    auto base = actual & JABBA_KIND_MASK;
    if (base == JABBA_NULL) return (declared & JABBA_KIND_NULLABLE) || (declared & JABBA_KIND_MASK) == JABBA_NULL;
    return base == (declared & JABBA_KIND_MASK);
}

/// Can a producer declaring `from` feed a consumer declaring `to`?
inline bool kind_feeds(uint32_t from, uint32_t to) {
    if ((from & JABBA_KIND_MASK) != (to & JABBA_KIND_MASK)) return false;
    return !(from & JABBA_KIND_NULLABLE) || (to & JABBA_KIND_NULLABLE);
}

inline std::size_t node_struct_end() { return offsetof(jabba_node, release) + sizeof(void (*)(void*)); }

}  // namespace detail

// ---- import: a foreign node in a C++ graph ---------------------------------------------

/// A node instance from another language. Owns it: `release` is called once, after
/// the graph and every call that used it are gone — unless it declares another ABI: then
/// it is never called into at all, and leaks (spec/ffi.md).
class Foreign {
public:
    /// Take ownership of an instance (from a factory, a capsule, a symbol...).
    static Foreign adopt(const jabba_node* node) {
        if (!node) throw std::invalid_argument("jabba::ffi::Foreign: null node");
        return Foreign(std::shared_ptr<Instance>(new Instance{node, compatible(*node)}));
    }
    static Foreign from_factory(jabba_node_factory factory) { return adopt(factory()); }

    const jabba_node& node() const { return *inst_->node; }

    struct Instance {
        const jabba_node* node;
        bool compatible;
        ~Instance() {
            if (compatible) node->release(node->self);
        }
    };
    std::shared_ptr<Instance> instance() const { return inst_; }

private:
    static bool compatible(const jabba_node& n) {
        return n.abi == JABBA_FFI_ABI && n.size >= offsetof(jabba_node, release) + sizeof(void (*)(void*));
    }
    explicit Foreign(std::shared_ptr<Instance> inst) : inst_(std::move(inst)) {}
    std::shared_ptr<Instance> inst_;
};

namespace detail {

/// Validate a foreign node against the kinds the graph will exchange with it.
inline void check_foreign(const std::string& name, const jabba_node& n, const std::vector<uint32_t>& inputs,
                          uint32_t output) {
    auto fail = [&](CompileErrorKind kind, const std::string& why) {
        throw CompileError(kind, "foreign node '" + name + "': " + why);
    };
    if (n.abi != JABBA_FFI_ABI) fail(CompileErrorKind::abi_mismatch, "ABI version " + std::to_string(n.abi) + ", expected " + std::to_string(JABBA_FFI_ABI));
    if (n.size < node_struct_end()) fail(CompileErrorKind::abi_mismatch, "node struct too small");
    if (!n.start || !n.cancel || !n.end) fail(CompileErrorKind::abi_mismatch, "start, cancel and end are required");
    if (n.n_inputs != inputs.size()) {
        fail(CompileErrorKind::type_mismatch,
             "takes " + std::to_string(n.n_inputs) + " inputs, the graph wires " + std::to_string(inputs.size()));
    }
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        if (!kind_feeds(inputs[i], n.input_kinds[i])) {
            fail(CompileErrorKind::type_mismatch, "input " + std::to_string(i) + " is " + kind_name(n.input_kinds[i]) +
                                                      ", the graph passes " + kind_name(inputs[i]));
        }
    }
    if (!kind_feeds(n.output_kind, output)) {
        fail(CompileErrorKind::type_mismatch,
             "returns " + kind_name(n.output_kind) + ", the graph expects " + kind_name(output));
    }
}

/// One call of a foreign node: the slot the ABI calls `jabba_call`, owned by the host.
/// Lives until end(); its state machine is the reference one from spec/ffi.md.
class Call : public std::enable_shared_from_this<Call> {
public:
    Call(std::shared_ptr<Foreign::Instance> inst, Ctx ctx) : inst_(std::move(inst)), ctx_(std::move(ctx)) {
        declared_output = inst_->node->output_kind;
    }

    std::vector<jabba_value> inputs;
    Scratch scratch;

    /// Start the call. Returns true if the result is already here (no need to suspend).
    bool begin(std::coroutine_handle<> waiter) {
        waiter_ = waiter;
        self_ = shared_from_this();  // the node may use the call until end()
        request_id_ = ctx_.request_id();
        host_.size = sizeof host_;
        host_.self = this;
        host_.post = &Call::post;
        host_.set_field = &Call::set_field;
        host_.request_id = request_id_.data();
        host_.request_id_len = request_id_.size();
        call_.size = sizeof call_;
        call_.inputs = inputs.data();
        call_.n_inputs = inputs.size();
        call_.host = &host_;
        call_.complete = &Call::complete;
        call_.host_data = this;
        call_.node_data = nullptr;
        auto& n = *inst_->node;
        n.start(n.self, &call_);
        start_returned_ = true;
        if (cancel_wanted_) do_cancel();
        unref();
        return ready_.exchange(true);
    }

    /// From the node's stop token: cancel now, or as soon as start() has returned.
    void request_cancel() {
        cancel_wanted_ = true;
        if (start_returned_) do_cancel();
    }

    std::uint32_t status = JABBA_FAILED;
    jabba_value value = detail::scalar(JABBA_NULL);
    std::string error;
    uint32_t declared_output = JABBA_NULL;

private:
    static void post(void*, void (*fn)(void*), void* arg) {
        Host::instance().post([fn, arg] { fn(arg); });
    }

    static void set_field(void* self, const char* key, std::size_t key_len, const jabba_value* value) {
        auto* c = static_cast<Call*>(self);
        try {
            c->ctx_.set(std::string(key, key_len), to_json(*value));
        } catch (...) {
            // A malformed value is dropped: the record is best-effort for foreign writes.
        }
    }

    static void complete(jabba_call* call, const jabba_result* r) {
        auto* c = static_cast<Call*>(call->host_data);
        auto keep = c->self_;  // unref() may drop the node's reference to us
        c->status = r->status;
        if (r->status == JABBA_OK) {
            c->value = adopt(r->value);
        } else {
            if (r->value.release) r->value.release(r->value.owner);  // misbehaving node: still no leak
            if (r->status == JABBA_FAILED && r->error) c->error.assign(r->error, r->error_len);
        }
        auto waiter = c->waiter_;
        bool suspended = c->ready_.exchange(true);
        c->unref();
        // Never resume inline: the node may hold locks of its own while it completes.
        if (suspended) Host::instance().post([waiter] { waiter.resume(); });
    }

    /// A buffer without release is valid only during complete(): copy it now.
    static jabba_value adopt(const jabba_value& v) {
        switch (v.kind & JABBA_KIND_MASK) {
            case JABBA_STR: case JABBA_BYTES: case JABBA_JSON: case JABBA_F64S: case JABBA_F32S: case JABBA_I64S: break;
            default: return v;
        }
        if (v.release) return v;
        std::size_t elem = 1;
        switch (v.kind & JABBA_KIND_MASK) {
            case JABBA_F64S: elem = sizeof(double); break;
            case JABBA_F32S: elem = sizeof(float); break;
            case JABBA_I64S: elem = sizeof(std::int64_t); break;
            default: break;
        }
        // std::max_align_t storage keeps the copy aligned for any element type.
        auto bytes = v.as.buf.len * elem;
        auto* copy = new std::vector<std::max_align_t>((bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t) + 1);
        if (bytes) std::memcpy(copy->data(), v.as.buf.data, bytes);
        auto out = owned(v.kind, copy, copy->data(), v.as.buf.len);
        return out;
    }

    void do_cancel() {
        if (cancel_issued_.exchange(true)) return;
        int r = refs_.load();
        while (r > 0 && !refs_.compare_exchange_weak(r, r + 1)) {
        }
        if (r == 0) return;  // already ended
        auto& n = *inst_->node;
        n.cancel(n.self, &call_);
        unref();
    }

    void unref() {
        if (refs_.fetch_sub(1) == 1) {
            auto keep = std::move(self_);
            auto& n = *inst_->node;
            n.end(n.self, &call_);
        }
    }

    std::shared_ptr<Foreign::Instance> inst_;
    Ctx ctx_;
    std::string request_id_;
    jabba_host host_{};
    jabba_call call_{};
    std::coroutine_handle<> waiter_;
    std::shared_ptr<Call> self_;
    std::atomic<int> refs_{2};  // "start not returned" + "complete not received"
    std::atomic<bool> start_returned_{false}, cancel_wanted_{false}, cancel_issued_{false}, ready_{false};
};

/// co_await a foreign call: suspends until complete(), cancels on the node's stop token.
struct CallAwaiter {
    std::shared_ptr<Call> call;
    const jabba::stop_token& token;
    std::unique_ptr<stop_callback> on_stop{};

    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> h) {
        auto c = call;
        on_stop = std::make_unique<stop_callback>(token, [c] { c->request_cancel(); });
        return !call->begin(h);
    }
    void await_resume() { on_stop.reset(); }
};

/// Turn a completed call into T, or throw (the node then fails).
template <class T>
T finish(Call& c, const std::string& name) {
    if (c.status == JABBA_OK) {
        if (!value_fits(c.value.kind, Codec<T>::kind) || !value_fits(c.value.kind, c.declared_output)) {
            auto got = c.value.kind;
            release(c.value);
            throw std::runtime_error("foreign node '" + name + "' returned " + kind_name(got) + ", expected " +
                                     kind_name(Codec<T>::kind));
        }
        return Codec<T>::take(c.value);
    }
    if (c.status == JABBA_CANCELLED) throw std::runtime_error("foreign node '" + name + "' was cancelled");
    throw std::runtime_error(c.error.empty() ? "foreign node '" + name + "' failed" : c.error);
}

}  // namespace detail

/// A foreign node with typed dependencies: `foreign<double>("score", deps(features), f)`.
/// Its declared kinds are checked against Codec<Ts>/Codec<T> here (CompileError).
template <class T, class... Ts>
NodeDef<T> foreign(std::string name, Deps<Ts...> deps, const Foreign& f) {
    detail::check_foreign(name, f.node(), {Codec<Ts>::kind...}, Codec<T>::kind);
    auto inst = f.instance();
    return node(name, deps, [inst, name](const Ts&... args, Ctx& ctx) -> task<T> {
        auto c = std::make_shared<detail::Call>(inst, ctx);
        (c->inputs.push_back(Codec<Ts>::view(args, c->scratch)), ...);
        detail::CallAwaiter wait{c, ctx.stop_token()};
        co_await wait;
        co_return detail::finish<T>(*c, name);
    });
}

/// A foreign node with dependencies by name, all of type V.
template <class T, class V>
NodeDef<T> foreign(std::string name, DynDeps<V> deps, const Foreign& f) {
    detail::check_foreign(name, f.node(), std::vector<uint32_t>(deps.names.size(), Codec<V>::kind), Codec<T>::kind);
    auto inst = f.instance();
    return node(name, deps, [inst, name](const DynInputs<V>& in, Ctx& ctx) -> task<T> {
        auto c = std::make_shared<detail::Call>(inst, ctx);
        for (auto& [n, v] : in.items()) c->inputs.push_back(Codec<V>::view(*v, c->scratch));
        detail::CallAwaiter wait{c, ctx.stop_token()};
        co_await wait;
        co_return detail::finish<T>(*c, name);
    });
}

// ---- export: a C++ node for another language ---------------------------------------

namespace detail {

/// Runs posted work through the host's `post`, or on our Host when it has none.
class PostExecutor final : public Executor {
public:
    explicit PostExecutor(const jabba_host* host) : post_(host->post), self_(host->self) {}
    void post(std::function<void()> fn) override {
        if (!post_) {
            Host::instance().post(std::move(fn));
            return;
        }
        auto* boxed = new std::function<void()>(std::move(fn));
        post_(self_, [](void* arg) {
            std::unique_ptr<std::function<void()>> f(static_cast<std::function<void()>*>(arg));
            (*f)();
        }, boxed);
    }

private:
    void (*post_)(void*, void (*)(void*), void*);
    void* self_;
};

/// Per call: the host must pass what the node declared (null only where nullable).
inline void check_inputs(const jabba_call& call, const std::vector<uint32_t>& kinds) {
    if (call.n_inputs != kinds.size()) {
        throw std::invalid_argument(std::to_string(call.n_inputs) + " inputs, expected " + std::to_string(kinds.size()));
    }
    for (std::size_t i = 0; i < kinds.size(); ++i) {
        if (!value_fits(call.inputs[i].kind, kinds[i])) {
            throw std::invalid_argument("input " + std::to_string(i) + " is " + kind_name(call.inputs[i].kind) +
                                        ", expected " + kind_name(kinds[i]));
        }
    }
}

inline jabba_result ok_result(jabba_value v) {
    jabba_result r;
    std::memset(&r, 0, sizeof r);
    r.size = sizeof r;
    r.status = JABBA_OK;
    r.value = v;
    return r;
}

inline jabba_result failed_result(const std::string& message) {
    jabba_result r;
    std::memset(&r, 0, sizeof r);
    r.size = sizeof r;
    r.status = JABBA_FAILED;
    r.value = scalar(JABBA_NULL);
    r.error = message.data();
    r.error_len = message.size();
    return r;
}

inline std::string describe(const std::exception_ptr& e) {
    try {
        std::rethrow_exception(e);
    } catch (const std::exception& x) {
        return x.what();
    } catch (...) {
        return "unknown exception";
    }
}

struct ExportedNode;

/// One call of an exported node. Two references: the body's driver and the host's end().
struct ExportedCall {
    std::shared_ptr<ExportedNode> node;
    jabba_call* call;
    std::shared_ptr<PostExecutor> exec;
    std::shared_ptr<stop_source> stop = std::make_shared<stop_source>();
    std::shared_ptr<jabba::detail::Shared> shared = std::make_shared<jabba::detail::Shared>();
    std::unique_ptr<Ctx> ctx;
    /// The body's view of its host. Ctx copies may outlive the call, so this is shared,
    /// and cut before complete(): no set_field after it.
    struct HostLink {
        std::mutex mutex;
        const jabba_host* host = nullptr;
    };
    std::shared_ptr<HostLink> link = std::make_shared<HostLink>();
    std::atomic<int> refs{2};

    void cut_host() {
        std::lock_guard lock(link->mutex);
        link->host = nullptr;
    }

    ExportedCall(std::shared_ptr<ExportedNode> n, jabba_call* c, std::shared_ptr<PostExecutor> e)
        : node(std::move(n)), call(c), exec(std::move(e)) {}

    void unref() {
        if (refs.fetch_sub(1) == 1) delete this;
    }
};

struct ExportedNode : std::enable_shared_from_this<ExportedNode> {
    jabba_node abi{};
    std::string name;
    std::vector<uint32_t> input_kinds;
    uint32_t output_kind = JABBA_NULL;
    /// Decodes the inputs, runs the body, encodes the result; calls `done` exactly once.
    std::function<void(ExportedCall&, std::function<void(jabba_result)> done)> run;
    std::shared_ptr<ExportedNode> self_ref;  // the instance reference held by its taker

    const jabba_node* expose() {
        abi.abi = JABBA_FFI_ABI;
        abi.size = sizeof abi;
        abi.self = this;
        abi.name = name.c_str();
        abi.output_kind = output_kind;
        abi.n_inputs = input_kinds.size();
        abi.input_kinds = input_kinds.data();
        abi.start = &ExportedNode::start;
        abi.cancel = &ExportedNode::cancel;
        abi.end = &ExportedNode::end;
        abi.release = &ExportedNode::release;
        self_ref = shared_from_this();
        return &abi;
    }

    static void start(void* self, jabba_call* call) noexcept {
        auto* n = static_cast<ExportedNode*>(self);
        ExportedCall* c = nullptr;
        try {
            c = new ExportedCall(n->shared_from_this(), call, std::make_shared<PostExecutor>(call->host));
        } catch (...) {
            call->node_data = nullptr;
            auto r = failed_result("out of memory");
            call->complete(call, &r);
            return;
        }
        call->node_data = c;
        try {
            c->shared->request_id.assign(call->host->request_id, call->host->request_id_len);
            c->link->host = call->host;
            c->shared->forward = [link = c->link](const std::string& key, const Json& value) {
                // Held while calling the host: end() may wait for an in-flight set_field.
                std::lock_guard lock(link->mutex);
                if (!link->host || !link->host->set_field) return;
                auto text = value.dump();
                auto v = buffer(JABBA_JSON, text.data(), text.size());
                link->host->set_field(link->host->self, key.data(), key.size(), &v);
            };
            c->ctx = std::make_unique<Ctx>(c->shared, c->stop->get_token());
        } catch (...) {
            c->cut_host();
            auto r = failed_result("out of memory");
            call->complete(call, &r);
            c->unref();
            return;
        }
        n->run(*c, [c](jabba_result r) {
            if (r.status == JABBA_FAILED && c->stop->stop_requested()) {
                r.status = JABBA_CANCELLED;  // it failed because it was asked to stop
                r.error = nullptr;
                r.error_len = 0;
            }
            c->cut_host();                   // no set_field after complete
            c->call->complete(c->call, &r);  // after this, no touching the call
            c->unref();
        });
    }

    static void cancel(void*, jabba_call* call) noexcept {
        auto* c = static_cast<ExportedCall*>(call->node_data);
        if (!c) return;
        // Not through the host: a cancel after complete must not touch it. And stop
        // callbacks run node code, which must not run on the host's thread (a Python
        // event loop, a Rust run's poll).
        Host::instance().post([stop = c->stop] { stop->request_stop(); });
    }

    static void end(void*, jabba_call* call) noexcept {
        auto* c = static_cast<ExportedCall*>(call->node_data);
        if (!c) return;
        c->cut_host();
        c->unref();
    }

    static void release(void* self) {
        auto* n = static_cast<ExportedNode*>(self);
        n->self_ref.reset();  // the node lives on while calls hold it
    }
};

/// Drives an async body: started inline in start(), completes when the task does.
template <class R, class Body>
jabba::detail::detached drive_async(Body body, std::function<void(jabba_result)> done) {
    std::string error;
    try {
        auto t = body();
        auto v = co_await std::move(t);
        done(ok_result(Codec<R>::own(std::move(v))));
        co_return;
    } catch (...) {
        error = describe(std::current_exception());
    }
    done(failed_result(error));
}

template <class R, class Make>
void run_body(ExportedCall& c, Make make_call, std::function<void(jabba_result)> done) {
    if constexpr (is_task_v<R>) {
        using U = typename R::value_type;
        drive_async<U>(std::move(make_call), std::move(done));
    } else {
        // A sync body may compute for long: never inside start(), always on the executor.
        auto task_done = std::make_shared<std::function<void(jabba_result)>>(std::move(done));
        auto posted = [make_call = std::move(make_call), task_done]() mutable {
            auto& finish_with = *task_done;
            std::string error;
            try {
                finish_with(ok_result(Codec<R>::own(make_call())));
                return;
            } catch (...) {
                error = describe(std::current_exception());
            }
            finish_with(failed_result(error));
        };
        try {
            c.exec->post(std::move(posted));
        } catch (...) {
            (*task_done)(failed_result("could not schedule the node: " + describe(std::current_exception())));
        }
    }
}

template <class Body>
const jabba_node* make_exported(std::string name, std::vector<uint32_t> inputs, uint32_t output, Body run) {
    auto n = std::make_shared<ExportedNode>();
    n->name = std::move(name);
    n->input_kinds = std::move(inputs);
    n->output_kind = output;
    n->run = std::move(run);
    return n->expose();
}

}  // namespace detail

/// Export a C++ node with typed inputs: each call of the result is a new instance.
///   extern "C" const jabba_node* score_node() {
///       return jabba::ffi::export_node<Features>("score", [](const Features& f) { ... });
///   }
/// The body may take jabba::Ctx& last and return T or task<T>. Exceptions fail the call.
template <class... Ts, class F>
const jabba_node* export_node(std::string name, F f) {
    constexpr bool with_ctx = std::is_invocable_v<const F&, const Ts&..., Ctx&>;
    static_assert(with_ctx || std::is_invocable_v<const F&, const Ts&...>,
                  "jabba::ffi::export_node<Ts...>: the body must be callable as f(const Ts&...) or "
                  "f(const Ts&..., jabba::Ctx&) on a const object");
    using R = typename std::conditional_t<with_ctx, std::invoke_result<const F&, const Ts&..., Ctx&>,
                                          std::invoke_result<const F&, const Ts&...>>::type;
    using Out = std::conditional_t<is_task_v<R>, typename task_value<R>::type, R>;
    auto fn = std::make_shared<const F>(std::move(f));
    return detail::make_exported(
        std::move(name), {Codec<Ts>::kind...}, Codec<Out>::kind,
        [fn](detail::ExportedCall& c, std::function<void(jabba_result)> done) {
            std::shared_ptr<std::tuple<Ts...>> args;
            try {
                detail::check_inputs(*c.call, c.node->input_kinds);
                args = [&]<std::size_t... I>(std::index_sequence<I...>) {
                    return std::make_shared<std::tuple<Ts...>>(Codec<Ts>::read(c.call->inputs[I])...);
                }(std::index_sequence_for<Ts...>{});
            } catch (...) {
                done(detail::failed_result("bad input: " + detail::describe(std::current_exception())));
                return;
            }
            Ctx* ctx = c.ctx.get();
            auto call = [fn, args, ctx]() -> R {
                return std::apply([&](const Ts&... a) -> R { return jabba::detail::call_maybe_ctx(*fn, *ctx, a...); }, *args);
            };
            detail::run_body<R>(c, std::move(call), std::move(done));
        });
}

/// Export a node whose inputs are all of type V, named for the body (DynInputs<V>).
template <class V, class F>
const jabba_node* export_node_dyn(std::string name, std::vector<std::string> input_names, F f) {
    constexpr bool with_ctx = std::is_invocable_v<const F&, const DynInputs<V>&, Ctx&>;
    static_assert(with_ctx || std::is_invocable_v<const F&, const DynInputs<V>&>,
                  "jabba::ffi::export_node_dyn<V>: the body must be callable as f(const DynInputs<V>&) or "
                  "f(const DynInputs<V>&, jabba::Ctx&) on a const object");
    using R = typename std::conditional_t<with_ctx, std::invoke_result<const F&, const DynInputs<V>&, Ctx&>,
                                          std::invoke_result<const F&, const DynInputs<V>&>>::type;
    using Out = std::conditional_t<is_task_v<R>, typename task_value<R>::type, R>;
    auto fn = std::make_shared<const F>(std::move(f));
    auto names = std::make_shared<const std::vector<std::string>>(std::move(input_names));
    std::vector<uint32_t> kinds(names->size(), Codec<V>::kind);
    return detail::make_exported(
        std::move(name), std::move(kinds), Codec<Out>::kind,
        [fn, names](detail::ExportedCall& c, std::function<void(jabba_result)> done) {
            std::shared_ptr<DynInputs<V>> inputs;
            try {
                detail::check_inputs(*c.call, c.node->input_kinds);
                jabba::detail::Inputs values;
                for (std::size_t i = 0; i < names->size(); ++i) {
                    values.push_back(jabba::detail::make_value(Codec<V>::read(c.call->inputs[i])));
                }
                inputs = std::make_shared<DynInputs<V>>(*names, values);
            } catch (...) {
                done(detail::failed_result("bad input: " + detail::describe(std::current_exception())));
                return;
            }
            Ctx* ctx = c.ctx.get();
            auto call = [fn, inputs, ctx]() -> R { return jabba::detail::call_maybe_ctx(*fn, *ctx, *inputs); };
            detail::run_body<R>(c, std::move(call), std::move(done));
        });
}

}  // namespace jabba::ffi
