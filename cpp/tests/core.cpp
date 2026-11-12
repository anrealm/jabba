// SPDX-License-Identifier: MIT
// Behavior that conformance cases cannot express: types, exceptions, hooks,
// executors, cooperative cancellation, deep await chains.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <jabba/jabba.hpp>

#include "check.hpp"

using namespace std::chrono_literals;
using jabba::NodeStatus;
using jabba::RunStatus;

namespace {

/// A small thread pool: the kind of executor a service would supply.
class Pool : public jabba::Executor {
public:
    explicit Pool(int threads) {
        for (int i = 0; i < threads; ++i) {
            workers_.emplace_back([this] {
                std::unique_lock lock(mutex_);
                while (true) {
                    changed_.wait(lock, [&] { return stopping_ || !tasks_.empty(); });
                    if (tasks_.empty()) return;
                    auto fn = std::move(tasks_.front());
                    tasks_.pop_front();
                    lock.unlock();
                    fn();
                    lock.lock();
                }
            });
        }
    }
    ~Pool() override {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        changed_.notify_all();
        for (auto& w : workers_) w.join();
    }
    void post(std::function<void()> fn) override {
        std::lock_guard lock(mutex_);
        tasks_.push_back(std::move(fn));
        changed_.notify_one();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::function<void()>> tasks_;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
};

template <class F>
jabba::CompileErrorKind compile_error_of(F build) {
    try {
        build();
    } catch (const jabba::CompileError& e) {
        return e.kind();
    }
    throw std::runtime_error("expected a compile error");
}

}  // namespace

TEST(typed_wiring_and_generic_lambdas) {
    struct Req {
        int x;
    };
    jabba::GraphBuilder<Req> g("g");
    auto req = g.request();
    auto twice = g.add(jabba::node("twice", jabba::deps(req), [](const auto& r) { return r.x * 2; }));
    auto text = g.add(jabba::node("text", jabba::deps(twice), [](const int& v, jabba::Ctx& ctx) -> jabba::task<std::string> {
        ctx.set("v", v);
        co_return std::to_string(v);
    }));
    auto r = g.compile_with_output(text).run_blocking(Req{21});
    CHECK(r.ok());
    CHECK(*r.output == "42");
    CHECK(*r.stat->field("v") == jabba::Json(42));
}

TEST(co_await_run_with_a_non_trivial_request) {
    struct Request {
        std::string user;
    };
    Pool pool(2);
    jabba::GraphBuilder<Request> g("g");
    auto req = g.request();
    auto n = g.add(jabba::node("len", jabba::deps(req), [](const Request& r) { return r.user.size(); }));
    auto graph = g.compile_with_output(n);
    std::promise<std::size_t> got;
    auto caller = [&]() -> jabba::task<void> {
        auto r = co_await graph.run(Request{std::string(64, 'u')}, pool);
        got.set_value(*r.output);
    };
    jabba::spawn(caller());
    CHECK(got.get_future().get() == 64);
}

TEST(bodies_may_own_move_only_state_and_return_references) {
    struct Client {
        std::string greeting = "hi";
        const std::string& hello() const { return greeting; }
    };
    jabba::GraphBuilder<int> g("g");
    auto c = std::make_unique<Client>();
    auto hello = g.add(jabba::node("hello", [client = std::move(c)]() -> const std::string& { return client->hello(); }));
    auto twice = g.add(jabba::node("twice", jabba::deps(hello), [](const std::string& s) { return s + s; }));
    auto r = g.compile_with_output(twice).run_blocking(0);
    CHECK(r.ok());
    CHECK(*r.output == "hihi");
}

TEST(type_errors_name_types_readably) {
    try {
        jabba::GraphBuilder<int> g("g");
        g.add(jabba::node("a", [] { return 1; }));
        (void)g.compile_with_output_named<std::vector<int>>("a");
        throw std::runtime_error("expected a compile error");
    } catch (const jabba::CompileError& e) {
        std::string what = e.what();
        CHECK(what.find("returns int") != std::string::npos);
        CHECK(what.find("vector") != std::string::npos);
        CHECK(what.find("St6") == std::string::npos);  // no mangled names
    }
}

TEST(dyn_dependency_of_the_wrong_type_is_a_compile_error) {
    auto kind = compile_error_of([] {
        jabba::GraphBuilder<int> g("g");
        g.add(jabba::node("a", [] { return 1; }));
        g.add(jabba::node("b", jabba::deps_dyn<std::string>({"a"}), [](const jabba::DynInputs<std::string>&) { return 0; }));
        (void)g.compile();
    });
    CHECK(kind == jabba::CompileErrorKind::type_mismatch);
}

TEST(output_of_the_wrong_type_is_a_compile_error) {
    auto kind = compile_error_of([] {
        jabba::GraphBuilder<int> g("g");
        g.add(jabba::node("a", [] { return 1; }));
        (void)g.compile_with_output_named<std::string>("a");
    });
    CHECK(kind == jabba::CompileErrorKind::type_mismatch);
}

TEST(an_exception_fails_the_node_with_its_message) {
    jabba::GraphBuilder<int> g("g");
    g.add(jabba::node("boom", [](jabba::Ctx&) -> jabba::task<int> {
        throw std::invalid_argument("bad input");
        co_return 0;
    }));
    auto r = g.compile().run_blocking(0);
    CHECK(r.status == RunStatus::failed);
    CHECK(r.failed_node == "boom");
    CHECK(r.error == "bad input");

    jabba::GraphBuilder<int> h("h");
    h.add(jabba::node("odd", [] {
        throw 42;
        return 0;
    }).optional().default_value(0));
    auto odd = h.compile().run_blocking(0);
    CHECK(odd.ok());
    CHECK(*odd.stat->node("odd")->error == "unknown exception");
}

TEST(on_run_done_is_called_once_per_recorded_run_and_never_for_a_cancelled_one) {
    std::atomic<int> calls{0};
    jabba::GraphBuilder<bool> g("g");
    auto req = g.request();
    g.add(jabba::node("maybe_fail", jabba::deps(req), [](const bool& fail) -> jabba::task<int> {
        if (fail) throw std::runtime_error("boom");
        co_return 1;
    }));
    g.add(jabba::node("wait", [](jabba::Ctx& ctx) -> jabba::task<void> { co_await ctx.stopped(); }));
    auto graph = g.compile().on_run_done([&](const jabba::RequestStat&) { ++calls; });

    jabba::stop_source outer;
    std::thread([&] {
        std::this_thread::sleep_for(20ms);
        outer.request_stop();
    }).detach();
    auto cancelled = graph.run_blocking(false, {.stop = outer.get_token()});
    CHECK(cancelled.status == RunStatus::cancelled);
    CHECK(!cancelled.stat.has_value());
    CHECK(calls == 0);

    auto failed = graph.run_blocking(true);
    CHECK(failed.status == RunStatus::failed);
    CHECK(calls == 1);
}

TEST(a_node_awaiting_ctx_stopped_resumes_when_it_times_out) {
    std::atomic<bool> resumed{false};
    jabba::GraphBuilder<int> g("g");
    g.add(jabba::node("patient", [&](jabba::Ctx& ctx) -> jabba::task<int> {
        co_await ctx.stopped();
        resumed = true;
        co_return 1;
    }).optional().default_value(0).timeout(20ms));
    auto started = std::chrono::steady_clock::now();
    auto r = g.compile().run_blocking(0);
    CHECK(std::chrono::steady_clock::now() - started < 500ms);
    CHECK(r.ok());
    CHECK(r.stat->node("patient")->status == NodeStatus::timeout);
    CHECK(resumed);
}

TEST(a_sync_node_that_overran_its_timeout_is_a_timeout) {
    jabba::GraphBuilder<int> g("g");
    auto slow = g.add(jabba::node("slow", [] {
        std::this_thread::sleep_for(60ms);
        return 1;
    }).optional().default_value(0).timeout(10ms));
    auto r = g.compile_with_output(slow).run_blocking(0);
    CHECK(r.ok());
    CHECK(*r.output == 0);
    CHECK(r.stat->node("slow")->status == NodeStatus::timeout);
    CHECK(*r.stat->node("slow")->error_kind == jabba::ErrorKind::timeout);
}

TEST(huge_deadline_and_timeout_mean_no_limit) {
    jabba::GraphBuilder<int> g("g");
    g.add(jabba::node("a", []() -> jabba::task<int> { co_return 1; }).timeout(std::chrono::nanoseconds::max()));
    auto r = g.compile().run_blocking(0, {.deadline = std::chrono::nanoseconds::max()});
    CHECK(r.ok());
}

TEST(runs_on_a_thread_pool_with_co_await) {
    Pool pool(4);
    jabba::GraphBuilder<int> g("g");
    auto req = g.request();
    auto a = g.add(jabba::node("a", jabba::deps(req), [](const int& x) -> jabba::task<int> { co_return x + 1; }));
    auto b = g.add(jabba::node("b", jabba::deps(req), [](const int& x) { return x * 10; }));
    auto sum = g.add(jabba::node("sum", jabba::deps(a, b), [](const int& x, const int& y, jabba::Ctx& ctx) {
        ctx.set("sum", x + y);
        return x + y;
    }));
    auto graph = g.compile_with_output(sum);

    constexpr int runs = 200;
    std::vector<std::promise<int>> promises(runs);
    auto caller = [&](int i) -> jabba::task<void> {
        auto r = co_await graph.run(i, pool);
        promises[static_cast<std::size_t>(i)].set_value(r.ok() ? *r.output : -1);
    };
    for (int i = 0; i < runs; ++i) jabba::spawn(caller(i));
    for (int i = 0; i < runs; ++i) CHECK(promises[static_cast<std::size_t>(i)].get_future().get() == (i + 1) + i * 10);
}

TEST(concurrent_run_blocking_calls_do_not_interfere) {
    jabba::GraphBuilder<int> g("g");
    auto req = g.request();
    auto echo = g.add(jabba::node("echo", jabba::deps(req), [](const int& x, jabba::Ctx& ctx) -> jabba::task<int> {
        ctx.set("x", x);
        co_return x;
    }));
    auto graph = g.compile_with_output(echo);
    std::vector<std::thread> threads;
    std::atomic<int> wrong{0};
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 50; ++i) {
                int x = t * 1000 + i;
                auto r = graph.run_blocking(x);
                if (!r.ok() || *r.output != x || !(*r.stat->field("x") == jabba::Json(x))) ++wrong;
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK(wrong == 0);
}

namespace {
jabba::task<int> one() { co_return 1; }
jabba::task<long> many(int n) {
    long sum = 0;
    for (int i = 0; i < n; ++i) {
        auto t = one();
        sum += co_await std::move(t);
    }
    co_return sum;
}
}  // namespace

TEST(a_million_sequential_awaits_do_not_grow_the_stack) {
    jabba::GraphBuilder<int> g("g");
    auto n = g.add(jabba::node("many", []() { return many(1'000'000); }));
    auto r = g.compile_with_output(n).run_blocking(0);
    CHECK(r.ok());
    CHECK(*r.output == 1'000'000);
}

TEST(record_numbers_keep_their_kind) {
    jabba::JsonObject o{{"one", 1.0}, {"huge", 1e300}, {"big_u64", std::numeric_limits<std::uint64_t>::max()}, {"len", std::size_t{3}}};
    auto text = jabba::Json(o).dump();
    CHECK(text.find("\"one\":1.0") != std::string::npos);
    CHECK(text.find("\"huge\":1e+300") != std::string::npos);
    CHECK(text.find("\"len\":3") != std::string::npos);
    CHECK(text.size() < 200);
    CHECK(jabba::Json('x').dump() == "\"x\"");
}

TEST(a_run_cancelled_before_it_starts_runs_no_node_code_and_reports_once) {
    Pool pool(8);
    std::atomic<int> ran{0};
    jabba::GraphBuilder<int> g("g");
    g.add(jabba::node("a", [&]() -> jabba::task<int> {
        ++ran;
        co_return 1;
    }));
    auto graph = g.compile();
    jabba::GraphBuilder<int> e("empty");
    auto empty = e.compile();

    jabba::stop_source stopped_already;
    stopped_already.request_stop();
    constexpr int runs = 2000;
    std::atomic<int> done{0}, done_empty{0}, not_cancelled{0};
    for (int i = 0; i < runs; ++i) {
        graph.run_async(i, pool, {.stop = stopped_already.get_token()}, [&](jabba::RunResult<std::monostate> r) {
            if (r.status != RunStatus::cancelled) ++not_cancelled;
            ++done;
        });
        empty.run_async(i, pool, {.stop = stopped_already.get_token()}, [&](jabba::RunResult<std::monostate>) { ++done_empty; });
    }
    auto until = std::chrono::steady_clock::now() + 5s;
    while ((done < runs || done_empty < runs) && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(50ms);  // anything late would show up now
    CHECK(done == runs);
    CHECK(done_empty == runs);  // exactly once each: more would mean double delivery
    CHECK(ran == 0);
    CHECK(not_cancelled == 0);
}

TEST(stop_callback_destructor_waits_for_a_callback_running_elsewhere) {
    jabba::stop_source source;
    std::atomic<bool> started{false}, finished{false};
    auto cb = std::make_unique<jabba::stop_callback>(source.get_token(), [&] {
        started = true;
        std::this_thread::sleep_for(50ms);
        finished = true;
    });
    std::thread stopper([&] { source.request_stop(); });
    while (!started) std::this_thread::yield();
    cb.reset();  // must not return while the callback is still running
    CHECK(finished);
    stopper.join();
}

TEST(the_result_does_not_wait_for_cancelled_nodes_cleanup) {
    Pool pool(2);
    jabba::GraphBuilder<int> g("g");
    g.add(jabba::node("slow_cleanup", [](jabba::Ctx& ctx) -> jabba::task<int> {
        co_await ctx.stopped();
        std::this_thread::sleep_for(300ms);  // cleanup that ignores the grace period
        co_return 0;
    }));
    auto graph = g.compile();
    std::promise<std::chrono::steady_clock::duration> took;
    auto started = std::chrono::steady_clock::now();
    graph.run_async(0, pool, {.deadline = 10ms}, [&](jabba::RunResult<std::monostate> r) {
        CHECK(r.status == RunStatus::timeout);
        took.set_value(std::chrono::steady_clock::now() - started);
    });
    CHECK(took.get_future().get() < 200ms);
    std::this_thread::sleep_for(350ms);  // let the cleanup finish before the pool goes away
}

int main() { return check::run_all(); }
