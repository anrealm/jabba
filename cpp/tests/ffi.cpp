// SPDX-License-Identifier: MIT
// The foreign-node ABI from both sides: the C++ importer against adversarial C nodes,
// and C++ exported nodes against a C host (ffi/testing).

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <jabba/ffi.hpp>

#include "check.hpp"
#include "jabba_ffi_testing.h"
#include "pool.hpp"

using namespace std::chrono_literals;
using jabba::Json;
using jabba::NodeStatus;
using jabba::RunStatus;
namespace ffi = jabba::ffi;

namespace {

/// Wait until every started call has ended (calls may finish after their run), then
/// check that the nodes saw no protocol violation and nothing leaked.
void settle_and_check_counters(bool all_instances_released = true) {
    jt_counters c{};
    for (int i = 0; i < 400; ++i) {
        jt_counters_get(&c);
        bool instances = !all_instances_released || c.instances_made == c.instances_released;
        if (c.starts == c.ends && c.values_made == c.values_released && instances) break;
        std::this_thread::sleep_for(5ms);
    }
    jt_counters_get(&c);
    if (c.violations != 0) throw std::runtime_error(std::string("protocol violation: ") + jt_first_violation());
    CHECK(c.starts == c.ends);
    CHECK(c.values_made == c.values_released);
    if (all_instances_released) CHECK(c.instances_made == c.instances_released);
}

template <class T>
jabba::RunResult<T> run_one(jabba_node_factory factory, std::optional<std::chrono::nanoseconds> timeout = {},
                            std::optional<std::chrono::nanoseconds> deadline = {}, bool optional = false) {
    jabba::GraphBuilder<int> g("g");
    auto def = ffi::foreign<T>("n", jabba::Deps<>{}, ffi::Foreign::from_factory(factory));
    if (timeout) def.timeout(*timeout);
    if (optional) def.optional().default_value(T{});
    auto n = g.add(std::move(def));
    jabba::RunOptions options;
    options.deadline = deadline;
    return g.compile_with_output(n).run_blocking(0, options);
}

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

// ---- importer: C++ host, adversarial C nodes ----------------------------------------

TEST(import_a_node_that_completes_inside_start) {
    jt_counters_reset();
    auto r = run_one<Json>(&jt_node_inline);
    CHECK(r.ok());
    CHECK(*r.output == Json("inline"));
    settle_and_check_counters();
}

TEST(import_a_node_completing_from_its_thread) {
    jt_counters_reset();
    auto r = run_one<Json>(&jt_node_delay);
    CHECK(r.ok());
    CHECK(*r.output == Json("delayed"));
    settle_and_check_counters();
}

TEST(a_node_timeout_cancels_the_foreign_call) {
    jt_counters_reset();
    auto started = std::chrono::steady_clock::now();
    // completes only when cancelled: the timeout must cancel it (a node completing on its
    // own could beat a late timer, and then no cancel is due)
    auto r = run_one<Json>(&jt_node_cancel_inline, 3ms, {}, true);
    CHECK(std::chrono::steady_clock::now() - started < 15ms + 50ms);
    CHECK(r.ok());
    CHECK(r.stat->node("n")->status == NodeStatus::timeout);
    settle_and_check_counters();
    jt_counters c{};
    jt_counters_get(&c);
    CHECK(c.cancels == 1);
}

TEST(a_node_that_completes_inside_cancel) {
    jt_counters_reset();
    auto r = run_one<Json>(&jt_node_cancel_inline, {}, 10ms);
    CHECK(r.status == RunStatus::timeout);
    CHECK(r.stat->node("n")->error_kind == jabba::ErrorKind::deadline);
    settle_and_check_counters();
}

TEST(a_node_holding_a_lock_across_complete_does_not_deadlock_the_host) {
    jt_counters_reset();
    auto done = std::async(std::launch::async, [] {
        for (int i = 0; i < 200; ++i) {
            // a timeout around the ~2 ms completion makes cancel race with complete
            auto r = run_one<Json>(&jt_node_locked, std::chrono::microseconds(1500 + (i % 7) * 150), {}, true);
            if (!r.ok()) return false;
        }
        return true;
    });
    CHECK(done.wait_for(30s) == std::future_status::ready);
    CHECK(done.get());
    settle_and_check_counters();
}

TEST(cancel_racing_complete_on_a_pool) {
    jt_counters_reset();
    Pool pool(4);
    jabba::GraphBuilder<int> g("g");
    auto n = g.add(ffi::foreign<Json>("race", jabba::Deps<>{}, ffi::Foreign::from_factory(&jt_node_race))
                       .optional()
                       .default_value(Json("timed out"))
                       .timeout(50us));
    auto graph = g.compile_with_output(n);
    constexpr int runs = 2000;
    std::atomic<int> finished{0}, bad{0};
    for (int i = 0; i < runs; ++i) {
        graph.run_async(i, pool, {}, [&](jabba::RunResult<Json> r) {
            if (!r.ok()) ++bad;
            ++finished;
        });
    }
    for (int i = 0; i < 2000 && finished < runs; ++i) std::this_thread::sleep_for(5ms);
    CHECK(finished == runs);
    CHECK(bad == 0);
    settle_and_check_counters(false);  // the graph still holds its instance
}

TEST(a_result_of_the_wrong_kind_fails_the_node) {
    jt_counters_reset();
    auto r = run_one<std::vector<double>>(&jt_node_wrong_kind);
    CHECK(r.status == RunStatus::failed);
    CHECK(r.error.find("returned f32s") != std::string::npos);
    settle_and_check_counters();
}

TEST(a_failing_node_reports_its_error) {
    jt_counters_reset();
    auto r = run_one<Json>(&jt_node_failing);
    CHECK(r.status == RunStatus::failed);
    CHECK(r.error == "boom");
    settle_and_check_counters();
}

TEST(abi_and_kind_mismatches_are_compile_errors) {
    jt_counters_reset();
    const jabba_node* bad = jt_node_bad_abi();
    CHECK(compile_error_of([bad] { (void)ffi::foreign<Json>("n", jabba::Deps<>{}, ffi::Foreign::adopt(bad)); }) ==
          jabba::CompileErrorKind::abi_mismatch);
    CHECK(compile_error_of([] { (void)ffi::foreign<Json>("n", jabba::Deps<>{}, ffi::Foreign::from_factory(&jt_node_f64_scale)); }) ==
          jabba::CompileErrorKind::type_mismatch);  // it takes an input the graph does not wire
    CHECK(compile_error_of([] {
              jabba::GraphBuilder<std::vector<double>> g("g");
              auto req = g.request();
              (void)ffi::foreign<Json>("n", jabba::deps(req), ffi::Foreign::from_factory(&jt_node_f64_scale));
          }) == jabba::CompileErrorKind::type_mismatch);  // it returns f64s, not json
    settle_and_check_counters(false);
    jt_counters c{};
    jt_counters_get(&c);
    CHECK(c.instances_made == 3);
    CHECK(c.instances_released == 2);  // the one with another ABI is never called into, not even released
    bad->release(bad->self);           // a host cannot know where it is; the test knows the kit's layout
}

TEST(arrays_cross_without_copies_and_are_released_once) {
    jt_counters_reset();
    {
        jabba::GraphBuilder<std::vector<double>> g("g");
        auto req = g.request();
        auto n = g.add(ffi::foreign<ffi::Array<double>>("scale", jabba::deps(req), ffi::Foreign::from_factory(&jt_node_f64_scale)));
        auto r = g.compile_with_output(n).run_blocking({1.0, 2.5, 4.0});
        CHECK(r.ok());
        CHECK(r.output->to_vector() == (std::vector<double>{2.0, 5.0, 8.0}));
        jt_counters c{};
        jt_counters_get(&c);
        CHECK(c.values_released == 0);  // still owned by the zero-copy Array in the result
    }
    settle_and_check_counters();
}

// ---- exporter: C host, C++ exported nodes ---------------------------------------------

namespace {

extern "C" const jabba_node* exported_twice() {
    return ffi::export_node<std::int64_t>("twice", [](const std::int64_t& x) { return x * 2; });
}

extern "C" const jabba_node* exported_async_fields() {
    return ffi::export_node<Json>("fields", [](const Json& in, jabba::Ctx& ctx) -> jabba::task<Json> {
        ctx.set("seen", in);
        co_return Json(jabba::JsonObject{{"echo", in}});
    });
}

extern "C" const jabba_node* exported_throws() {
    return ffi::export_node<double>("throws", [](const double&) -> double { throw std::runtime_error("bad luck"); });
}

extern "C" const jabba_node* exported_waits_for_cancel() {
    return ffi::export_node<>("waits", [](jabba::Ctx& ctx) -> jabba::task<Json> {
        co_await ctx.stopped();
        throw std::runtime_error("stopped");
    });
}

extern "C" const jabba_node* exported_racy() {
    return ffi::export_node<>("racy", [](jabba::Ctx& ctx) -> jabba::task<Json> {
        // completes on a thread of its own after a random moment, unless stopped first
        struct Later {
            jabba::stop_token token;
            bool await_ready() { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                std::thread([h, token = token] {
                    for (int i = 0; i < 20 && !token.stop_requested(); ++i) std::this_thread::sleep_for(10us);
                    h.resume();
                }).detach();
            }
            void await_resume() {}
        };
        Later later{ctx.stop_token()};
        co_await later;
        co_return Json("done");
    });
}

extern "C" const jabba_node* exported_scale() {
    return ffi::export_node<std::vector<double>>("scale", [](const std::vector<double>& xs) {
        std::vector<double> out;
        for (double x : xs) out.push_back(x * 3);
        return ffi::Array<double>(std::move(out));
    });
}

extern "C" const jabba_node* exported_dyn() {
    return ffi::export_node_dyn<Json>("dyn", {"a"}, [](const jabba::DynInputs<Json>& in) { return *in.get("a"); });
}

jt_drive_result drive(jabba_node_factory factory, jt_mode mode = JT_PLAIN) {
    jt_drive_result r{};
    jt_drive(factory(), mode, &r);
    if (!r.ok) throw std::runtime_error(std::string("protocol: ") + r.error);
    return r;
}

}  // namespace

TEST(export_a_sync_node) {
    auto r = drive(&exported_twice);
    CHECK(r.status == JABBA_OK);
    CHECK(std::string(r.value_json) == "42");
}

TEST(export_an_async_node_that_writes_fields) {
    auto r = drive(&exported_async_fields);
    CHECK(r.status == JABBA_OK);
    CHECK(std::string(r.value_json) == R"({"echo":{"a":1}})");
    CHECK(r.set_fields == 1);
}

TEST(export_a_node_that_throws) {
    auto r = drive(&exported_throws);
    CHECK(r.status == JABBA_FAILED);
    CHECK(std::string(r.value_json) == "bad luck");
}

TEST(an_exported_node_stops_when_cancelled) {
    auto r = drive(&exported_waits_for_cancel, JT_CANCEL_SOON);
    CHECK(r.status == JABBA_CANCELLED);
}

TEST(an_exported_node_survives_cancel_racing_its_completion) {
    for (int i = 0; i < 300; ++i) {
        auto r = drive(&exported_racy, JT_CANCEL_RACE);
        CHECK(r.status == JABBA_OK);
    }
}

TEST(export_arrays_and_dyn_inputs) {
    auto a = drive(&exported_scale);
    CHECK(a.status == JABBA_OK);
    CHECK(std::string(a.value_json) == "[3,6,9]");
    auto d = drive(&exported_dyn);
    CHECK(std::string(d.value_json) == R"({"a":1})");
}

TEST(cpp_to_cpp_through_the_abi) {
    jabba::GraphBuilder<std::int64_t> g("g");
    auto req = g.request();
    auto n = g.add(ffi::foreign<std::int64_t>("twice", jabba::deps(req), ffi::Foreign::from_factory(&exported_twice)));
    auto r = g.compile_with_output(n).run_blocking(21);
    CHECK(r.ok());
    CHECK(*r.output == 42);
}

TEST(a_borrowed_result_buffer_is_copied_during_complete) {
    jt_counters_reset();
    auto r = run_one<std::string>(&jt_node_borrowed_str);
    CHECK(r.ok());
    CHECK(*r.output == "borrowed");
    settle_and_check_counters();
}

TEST(a_failed_result_carrying_a_value_still_releases_it) {
    jt_counters_reset();
    auto r = run_one<Json>(&jt_node_failed_with_value);
    CHECK(r.status == RunStatus::failed);
    CHECK(r.error == "bad");
    settle_and_check_counters();
}

TEST(the_kit_reports_a_node_that_breaks_the_protocol) {
    jt_drive_result r{};
    jt_drive(jt_node_failed_with_value(), JT_PLAIN, &r);
    CHECK(!r.ok);
    CHECK(std::string(r.error).find("non-OK result carries a value") != std::string::npos);
}

namespace {

extern "C" const jabba_node* exported_stray_ctx() {
    // The body hands a copy of its Ctx to work that outlives the call.
    return ffi::export_node<>("stray", [](jabba::Ctx& ctx) -> Json {
        std::thread([ctx] {
            std::this_thread::sleep_for(30ms);
            auto copy = ctx;
            copy.set("late", true);  // after complete and end: must be dropped, not crash
        }).detach();
        return Json("now");
    });
}

extern "C" const jabba_node* exported_inline_async() {
    return ffi::export_node<>("inline_async", []() -> jabba::task<Json> { co_return Json("at once"); });
}

}  // namespace

TEST(late_ctx_writes_of_an_exported_node_are_dropped) {
    auto r = drive(&exported_stray_ctx);
    CHECK(r.status == JABBA_OK);
    std::this_thread::sleep_for(60ms);  // let the stray write happen (under ASan: no use-after-free)
}

TEST(cancel_after_an_exported_node_completed_does_not_touch_the_host) {
    auto r = drive(&exported_inline_async, JT_CANCEL_SOON);
    CHECK(r.status == JABBA_OK);
}

int main() { return check::run_all(); }
