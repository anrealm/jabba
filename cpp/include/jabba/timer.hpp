// SPDX-License-Identifier: MIT
// One background thread that fires callbacks at their time. Callbacks must be
// tiny: jabba's only post work to an executor, they never run node code.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace jabba::detail {

using Clock = std::chrono::steady_clock;

struct TimerKey {
    Clock::time_point at;
    std::uint64_t id = 0;
};

class Timer {
public:
    /// Deliberately leaked: runs may still use it while static objects are
    /// destroyed at exit, so it is never destroyed (and its thread never joined).
    static Timer& instance() {
        static Timer* timer = new Timer;
        return *timer;
    }

    TimerKey add(Clock::time_point at, std::function<void()> fn) {
        std::unique_lock lock(mutex_);
        TimerKey key{at, ++next_id_};
        bool earliest = entries_.empty() || at < entries_.begin()->first.first;
        entries_.emplace(std::make_pair(at, key.id), std::move(fn));
        lock.unlock();
        if (earliest) changed_.notify_one();
        return key;
    }

    void cancel(const TimerKey& key) {
        std::lock_guard lock(mutex_);
        entries_.erase(std::make_pair(key.at, key.id));
    }

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

private:
    Timer() : thread_([this] { loop(); }) {}

    void loop() {
        std::unique_lock lock(mutex_);
        while (true) {
            auto now = Clock::now();
            std::vector<std::function<void()>> due;
            while (!entries_.empty() && entries_.begin()->first.first <= now) {
                due.push_back(std::move(entries_.begin()->second));
                entries_.erase(entries_.begin());
            }
            if (!due.empty()) {
                lock.unlock();
                for (auto& fn : due) fn();
                lock.lock();
                continue;
            }
            if (entries_.empty()) {
                changed_.wait(lock);
            } else {
                // A copy: the entry may be cancelled (and freed) while we wait.
                auto at = entries_.begin()->first.first;
                changed_.wait_until(lock, at);
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::uint64_t next_id_ = 0;
    std::map<std::pair<Clock::time_point, std::uint64_t>, std::function<void()>> entries_;
    std::thread thread_;
};

}  // namespace jabba::detail
