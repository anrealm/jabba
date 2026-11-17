// SPDX-License-Identifier: MIT
// The C++ testkit (ffi/testing/jabba_testkit.h): scripted conformance nodes, exported
// over the ABI with jabba::ffi.

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <jabba/ffi.hpp>

#include "jabba_testkit.h"

using jabba::Json;
using jabba::JsonArray;
using jabba::JsonObject;

namespace {

struct Session {
    std::mutex mutex;
    std::vector<std::string> events;
    std::map<std::string, std::pair<bool, std::vector<std::coroutine_handle<>>>> signals;

    void event(std::string e) {
        std::lock_guard lock(mutex);
        events.push_back(std::move(e));
    }

    void signal(const std::string& name) {
        std::vector<std::coroutine_handle<>> waiting;
        {
            std::lock_guard lock(mutex);
            auto& s = signals[name];
            s.first = true;
            waiting.swap(s.second);
        }
        for (auto h : waiting) h.resume();
    }
};

struct WaitSignal {
    Session& session;
    std::string name;
    bool await_ready() { return false; }
    bool await_suspend(std::coroutine_handle<> h) {
        std::lock_guard lock(session.mutex);
        auto& s = session.signals[name];
        if (s.first) return false;
        s.second.push_back(h);
        return true;
    }
    void await_resume() {}
};

/// Sleeps on a thread; wakes early on stop unless told to ignore it. Yields true if cut short.
struct Sleep {
    struct State {
        std::mutex mutex;
        std::condition_variable cv;
        bool stop = false;
    };
    std::chrono::nanoseconds duration;
    jabba::stop_token token;
    bool ignore_cancel;
    std::shared_ptr<State> state = std::make_shared<State>();
    std::unique_ptr<jabba::stop_callback> on_stop = nullptr;
    bool stopped = false;

    bool await_ready() { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        if (!ignore_cancel) {
            on_stop = std::make_unique<jabba::stop_callback>(token, [s = state] {
                {
                    std::lock_guard lock(s->mutex);
                    s->stop = true;
                }
                s->cv.notify_all();
            });
        }
        std::thread([s = state, d = duration, h, this] {
            bool cut;
            {
                std::unique_lock lock(s->mutex);
                cut = s->cv.wait_for(lock, d, [&] { return s->stop; });
            }
            stopped = cut;
            h.resume();
        }).detach();
    }
    bool await_resume() { return stopped; }
};

const Json* at(const Json& j, std::string_view key) { return j.find(key); }

std::string str(const Json* j) {
    auto* s = j ? j->get_if<std::string>() : nullptr;
    return s ? *s : std::string();
}

bool flag(const Json* j) {
    auto* b = j ? j->get_if<bool>() : nullptr;
    return b && *b;
}

std::optional<double> num(const Json* j) {
    if (!j) return std::nullopt;
    if (auto* i = j->get_if<std::int64_t>()) return static_cast<double>(*i);
    if (auto* d = j->get_if<double>()) return *d;
    return std::nullopt;
}

jabba::task<Json> run_behavior(std::shared_ptr<Session> session, Json behavior, std::string name,
                               const jabba::DynInputs<Json>& inputs, jabba::Ctx& ctx) {
    session->event("start:" + name);
    auto signal = str(at(behavior, "signal"));
    if (!signal.empty()) session->signal(signal);
    auto wait_for = str(at(behavior, "wait_for"));
    if (!wait_for.empty()) {
        WaitSignal wait{*session, wait_for};
        co_await wait;
    }
    if (auto ms = num(at(behavior, "sleep_ms"))) {
        auto d = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double, std::milli>(*ms));
        Sleep sleep{d, ctx.stop_token(), flag(at(behavior, "ignore_cancel"))};
        bool cut = co_await sleep;
        if (cut) throw std::runtime_error("cancelled");
    }
    if (auto* fields = at(behavior, "set_field") ? at(behavior, "set_field")->get_if<JsonObject>() : nullptr) {
        for (auto& [k, v] : *fields) ctx.set(k, v);
    }
    session->event("end:" + name);
    if (auto* f = at(behavior, "fail")) throw std::runtime_error(str(f));
    if (flag(at(behavior, "echo_inputs"))) {
        JsonObject out;
        for (auto& [n, v] : inputs.items()) {
            if (n != jabba::request_name) out.emplace_back(n, *v);
        }
        co_return Json(std::move(out));
    }
    if (flag(at(behavior, "echo_request"))) co_return *inputs.get(jabba::request_name);
    if (auto* v = at(behavior, "value")) co_return *v;
    co_return Json();
}

}  // namespace

struct jabba_testkit_session {
    std::shared_ptr<Session> session = std::make_shared<Session>();
};

extern "C" {

jabba_testkit_session* jabba_testkit_session_new(void) { return new jabba_testkit_session; }

void jabba_testkit_session_free(jabba_testkit_session* s) { delete s; }

const jabba_node* jabba_testkit_node(jabba_testkit_session* s, const char* spec, size_t spec_len) {
    try {
        auto j = jabba::parse_json(std::string_view(spec, spec_len));
        auto name = str(at(j, "name"));
        std::vector<std::string> deps;
        if (auto* a = at(j, "deps") ? at(j, "deps")->get_if<JsonArray>() : nullptr) {
            for (auto& d : *a) deps.push_back(str(&d));
        }
        Json behavior = at(j, "behavior") ? *at(j, "behavior") : Json(JsonObject{});
        auto session = s->session;
        return jabba::ffi::export_node_dyn<Json>(
            name, deps, [session, behavior, name](const jabba::DynInputs<Json>& in, jabba::Ctx& ctx) {
                return run_behavior(session, behavior, name, in, ctx);
            });
    } catch (...) {
        return nullptr;
    }
}

size_t jabba_testkit_events(jabba_testkit_session* s, char* out, size_t cap) {
    JsonArray events;
    {
        std::lock_guard lock(s->session->mutex);
        for (auto& e : s->session->events) events.emplace_back(e);
    }
    auto text = Json(std::move(events)).dump();
    if (cap) std::memcpy(out, text.data(), std::min(cap, text.size()));
    return text.size();
}

}  // extern "C"
