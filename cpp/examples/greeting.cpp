// SPDX-License-Identifier: MIT
// The example from README.md, built and run by CI.
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

#include <jabba/jabba.hpp>

using namespace std::chrono_literals;

struct Request {
    std::string user;
};

int main() {
    jabba::GraphBuilder<Request> g("greeting");
    auto req = g.request();
    auto name = g.add(jabba::node("name", jabba::deps(req), [](const Request& r) { return r.user; }));
    auto extra = g.add(jabba::node("extra", []() -> jabba::task<std::string> {
                           throw std::runtime_error("source down");
                           co_return "";
                       })
                           .optional()
                           .default_value("")  // what dependents get if it fails
                           .timeout(100ms));
    auto greeting = g.add(jabba::node("greeting", jabba::deps(name, extra),
                                      [](const std::string& who, const std::string& more, jabba::Ctx& ctx) {
                                          ctx.set("had_extra", !more.empty());  // goes into the run's record
                                          return "hello, " + who + more;
                                      }));
    auto graph = g.compile_with_output(greeting);

    auto run = graph.run_blocking(Request{"alice"});
    std::cout << *run.output << "\n";                  // hello, alice
    std::cout << run.stat->to_record().dump() << "\n";  // one JSON record per run
    return run.ok() && *run.output == "hello, alice" ? 0 : 1;
}
