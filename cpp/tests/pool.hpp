// SPDX-License-Identifier: MIT
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <jabba/graph.hpp>

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
