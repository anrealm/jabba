// SPDX-License-Identifier: MIT
// A minimal test runner: no dependencies, one executable per test file.
#pragma once

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

namespace check {

struct Test {
    const char* name;
    void (*fn)();
};

inline std::vector<Test>& registry() {
    static std::vector<Test> tests;
    return tests;
}

inline int run_all() {
    int failed = 0;
    for (auto& t : registry()) {
        std::printf("run  %s\n", t.name);
        std::fflush(stdout);
        try {
            t.fn();
            std::printf("ok   %s\n", t.name);
        } catch (const std::exception& e) {
            ++failed;
            std::printf("FAIL %s\n  %s\n", t.name, e.what());
        }
    }
    std::printf("%zu tests, %d failed\n", registry().size(), failed);
    return failed == 0 ? 0 : 1;
}

}  // namespace check

#define TEST(name)                                                                       \
    static void name();                                                                  \
    static const bool name##_registered = (check::registry().push_back({#name, name}), true); \
    static void name()

#define CHECK(cond)                                                                                 \
    do {                                                                                            \
        if (!(cond)) {                                                                              \
            throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) +       \
                                     ": CHECK failed: " #cond);                                     \
        }                                                                                           \
    } while (0)
