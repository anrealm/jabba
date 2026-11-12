// SPDX-License-Identifier: MIT
// Runs the language-neutral conformance cases from spec/cases against the C++ core.

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <jabba/jabba.hpp>

#include "json_parse.hpp"

using jabba::Json;
using jabba::JsonObject;
using namespace std::chrono_literals;

namespace {

const char* const language = "cpp";
const std::set<std::string> capabilities = {"ignore_cancel"};

/// spec/ next to cpp/ in a checkout; JABBA_SPEC_DIR overrides it when vendored elsewhere.
std::filesystem::path spec_dir() {
    if (const char* env = std::getenv("JABBA_SPEC_DIR")) return env;
    return JABBA_SPEC_DIR_DEFAULT;
}

// ---- small JSON helpers -------------------------------------------------------

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

std::vector<std::string> strings(const Json* j) {
    std::vector<std::string> out;
    if (auto* a = j ? j->get_if<jabba::JsonArray>() : nullptr) {
        for (auto& v : *a) out.push_back(str(&v));
    }
    return out;
}

std::chrono::nanoseconds millis(double ms) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double, std::milli>(ms));
}

/// JSON equality with objects compared as maps.
bool same(const Json& a, const Json& b) {
    auto* oa = a.get_if<JsonObject>();
    auto* ob = b.get_if<JsonObject>();
    if (oa && ob) {
        if (oa->size() != ob->size()) return false;
        for (auto& [k, v] : *oa) {
            auto* other = b.find(k);
            if (!other || !same(v, *other)) return false;
        }
        return true;
    }
    auto* aa = a.get_if<jabba::JsonArray>();
    auto* ab = b.get_if<jabba::JsonArray>();
    if (aa && ab) {
        if (aa->size() != ab->size()) return false;
        for (std::size_t i = 0; i < aa->size(); ++i) {
            if (!same((*aa)[i], (*ab)[i])) return false;
        }
        return true;
    }
    return a == b;
}

// ---- what scripted nodes did --------------------------------------------------

struct Trace {
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

    std::vector<std::string> snapshot() {
        std::lock_guard lock(mutex);
        return events;
    }
};

struct WaitSignal {
    Trace& trace;
    std::string name;
    bool await_ready() { return false; }
    bool await_suspend(std::coroutine_handle<> h) {
        std::lock_guard lock(trace.mutex);
        auto& s = trace.signals[name];
        if (s.first) return false;
        s.second.push_back(h);
        return true;
    }
    void await_resume() {}
};

/// Sleeps on a thread; wakes early when stop is requested unless told to ignore it.
/// `co_await` yields true if the sleep was cut short by a stop.
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
    std::unique_ptr<jabba::stop_callback> on_stop;
    bool stopped = false;

    bool await_ready() { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        if (!ignore_cancel) {
            auto s = state;
            on_stop = std::make_unique<jabba::stop_callback>(token, [s] {
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

jabba::task<Json> run_behavior(Json behavior, std::string name, std::shared_ptr<Trace> trace,
                               const jabba::DynInputs<Json>& inputs, jabba::Ctx& ctx) {
    trace->event("start:" + name);
    auto signal = str(at(behavior, "signal"));
    if (!signal.empty()) trace->signal(signal);
    auto wait_for = str(at(behavior, "wait_for"));
    if (!wait_for.empty()) {
        WaitSignal wait{*trace, wait_for};
        co_await wait;
    }
    if (auto ms = num(at(behavior, "sleep_ms"))) {
        Sleep sleep{millis(*ms), ctx.stop_token(), flag(at(behavior, "ignore_cancel")), std::make_shared<Sleep::State>(), nullptr, false};
        bool cut = co_await sleep;
        if (cut) throw std::runtime_error("cancelled");
    }
    if (auto* fields = at(behavior, "set_field") ? at(behavior, "set_field")->get_if<JsonObject>() : nullptr) {
        for (auto& [k, v] : *fields) ctx.set(k, v);
    }
    trace->event("end:" + name);
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

// ---- running a case -----------------------------------------------------------

struct Outcome {
    std::string status;
    std::string compile_error;
    std::string error_node;
    std::optional<Json> output;
    std::optional<Json> record;
    std::vector<std::string> events;
    double duration_ms = 0;
};

void add_nodes(jabba::GraphBuilder<Json>& g, const Json& c, const std::shared_ptr<Trace>& trace) {
    auto* nodes = at(*at(c, "graph"), "nodes")->get_if<jabba::JsonArray>();
    for (auto& n : *nodes) {
        auto name = str(at(n, "name"));
        Json behavior = at(n, "behavior") ? *at(n, "behavior") : Json(JsonObject{});
        auto deps = strings(at(n, "deps"));
        if (flag(at(behavior, "echo_request"))) deps.emplace_back(jabba::request_name);
        auto def = jabba::node(name, jabba::deps_dyn<Json>(deps),
                               [behavior, name, trace](const jabba::DynInputs<Json>& in, jabba::Ctx& ctx) {
                                   return run_behavior(behavior, name, trace, in, ctx);
                               });
        def.after(strings(at(n, "after")));
        if (flag(at(n, "optional"))) def.optional();
        if (auto* d = at(n, "default")) def.default_value(*d);
        if (auto ms = num(at(n, "timeout_ms"))) def.timeout(millis(*ms));
        g.add(std::move(def));
    }
}

template <class O>
void take(Outcome& o, jabba::RunResult<O>& r) {
    switch (r.status) {
        case jabba::RunStatus::ok: o.status = "ok"; break;
        case jabba::RunStatus::failed: o.status = "failed"; break;
        case jabba::RunStatus::timeout: o.status = "timeout"; break;
        case jabba::RunStatus::cancelled: o.status = "cancelled"; break;
    }
    o.error_node = r.failed_node;
    if (r.stat) o.record = r.stat->to_record();
}

Outcome run_case(const Json& c) {
    auto trace = std::make_shared<Trace>();
    jabba::GraphBuilder<Json> g(str(at(c, "name")));
    add_nodes(g, c, trace);

    jabba::RunOptions options;
    if (auto ms = num(at(c, "deadline_ms"))) options.deadline = millis(*ms);
    jabba::stop_source outer;
    if (auto ms = num(at(c, "cancel_after_ms"))) {
        options.stop = outer.get_token();
        std::thread([outer, d = millis(*ms)]() mutable {
            std::this_thread::sleep_for(d);
            outer.request_stop();
        }).detach();
    }
    Json request = at(c, "request") ? *at(c, "request") : Json();

    Outcome o;
    auto started = std::chrono::steady_clock::now();
    try {
        if (auto out = str(at(*at(c, "graph"), "output")); !out.empty()) {
            auto graph = g.compile_with_output_named<Json>(out);
            auto r = graph.run_blocking(request, options);
            take(o, r);
            if (r.output) o.output = *r.output;
        } else {
            auto graph = g.compile();
            auto r = graph.run_blocking(request, options);
            take(o, r);
        }
    } catch (const jabba::CompileError& e) {
        o.status = "compile_error";
        o.compile_error = std::string(jabba::to_string(e.kind()));
        return o;
    }
    o.duration_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    if (auto ms = num(at(c, "settle_ms"))) std::this_thread::sleep_for(millis(*ms));
    o.events = trace->snapshot();
    return o;
}

// ---- checking -----------------------------------------------------------------

std::vector<std::string> check(const Json& c, const Outcome& o) {
    std::vector<std::string> errors;
    auto eq = [&](const std::string& what, const Json& actual, const Json& expected) {
        if (!same(actual, expected)) errors.push_back(what + ": expected " + expected.dump() + ", got " + actual.dump());
    };
    auto& expect = *at(c, "expect");
    auto status = str(at(expect, "status"));
    eq("status", Json(o.status), Json(status));
    if (auto* e = at(expect, "compile_error")) eq("compile_error", Json(o.compile_error), *e);
    if (auto* e = at(expect, "error_node")) eq("error_node", Json(o.error_node), *e);
    if (auto* e = at(expect, "output")) eq("output", o.output.value_or(Json()), *e);

    if (status == "cancelled" || status == "compile_error") {
        if (o.record) errors.push_back("record: expected none");
    } else if (!o.record) {
        errors.push_back("record: expected one, got none");
    } else {
        if (auto* nodes = at(expect, "nodes") ? at(expect, "nodes")->get_if<JsonObject>() : nullptr) {
            for (auto& [name, node_expect] : *nodes) {
                auto* actual = at(*at(*o.record, "nodes"), name);
                if (!actual) {
                    errors.push_back("node '" + name + "': missing from the record");
                    continue;
                }
                for (auto& [key, value] : *node_expect.get_if<JsonObject>()) {
                    eq("node '" + name + "' " + key, *at(*actual, key), value);
                }
            }
        }
        if (auto* f = at(expect, "fields")) eq("fields", *at(*o.record, "fields"), *f);
    }

    auto pos = [&](const std::string& e) -> std::ptrdiff_t {
        auto it = std::find(o.events.begin(), o.events.end(), e);
        return it == o.events.end() ? -1 : it - o.events.begin();
    };
    if (auto* order = at(expect, "order") ? at(expect, "order")->get_if<jabba::JsonArray>() : nullptr) {
        for (auto& pair : *order) {
            auto& p = *pair.get_if<jabba::JsonArray>();
            auto before = str(&p[0]), after = str(&p[1]);
            auto e = pos("end:" + before), s = pos("start:" + after);
            if (e < 0 || s < 0 || e > s) errors.push_back("order: '" + before + "' must finish before '" + after + "' starts");
        }
    }
    for (auto& name : strings(at(expect, "stopped"))) {
        if (pos("end:" + name) >= 0) errors.push_back("stopped: node '" + name + "' ran to the end of its body");
    }
    if (auto max = num(at(expect, "max_duration_ms")); max && o.duration_ms > *max) {
        errors.push_back("duration: " + std::to_string(o.duration_ms) + "ms > max " + std::to_string(*max) + "ms");
    }
    return errors;
}

std::vector<std::pair<std::string, Json>> load_cases() {
    std::vector<std::filesystem::path> paths;
    for (auto& entry : std::filesystem::directory_iterator(spec_dir() / "cases")) {
        if (entry.path().extension() == ".json") paths.push_back(entry.path());
    }
    std::sort(paths.begin(), paths.end());
    std::vector<std::pair<std::string, Json>> cases;
    for (auto& p : paths) {
        std::ifstream in(p);
        std::stringstream buf;
        buf << in.rdbuf();
        auto file = json_parse::parse(buf.str());
        for (auto& c : *at(file, "cases")->get_if<jabba::JsonArray>()) cases.emplace_back(p.filename().string(), c);
    }
    return cases;
}

}  // namespace

int main() {
    auto cases = load_cases();
    std::set<std::string> names;
    std::vector<std::string> failures;
    int ran = 0;
    for (auto& [file, c] : cases) {
        auto id = file + "::" + str(at(c, "name"));
        if (!names.insert(str(at(c, "name"))).second) failures.push_back(id + "\n  duplicate case name");
        std::string missing;
        for (auto& r : strings(at(c, "requires"))) {
            if (!capabilities.count(r)) missing = r;
        }
        if (!missing.empty()) {
            std::printf("skip  %s: driver does not support %s\n", id.c_str(), missing.c_str());
            continue;
        }
        bool pending = at(c, "pending") && at(*at(c, "pending"), language);
        auto errors = check(c, run_case(c));
        ++ran;
        if (!pending && errors.empty()) {
            std::printf("ok    %s\n", id.c_str());
        } else if (pending && !errors.empty()) {
            std::printf("xfail %s\n", id.c_str());
        } else if (pending) {
            failures.push_back(id + "\n  passes but is marked pending for " + language);
        } else {
            std::string text = id;
            for (auto& e : errors) text += "\n  " + e;
            failures.push_back(text);
        }
    }
    if (ran == 0) {
        std::printf("no cases under %s\n", spec_dir().string().c_str());
        return 1;
    }
    for (auto& f : failures) std::printf("FAIL %s\n", f.c_str());
    std::printf("%d cases, %zu failed\n", ran, failures.size());
    return failures.empty() ? 0 : 1;
}
