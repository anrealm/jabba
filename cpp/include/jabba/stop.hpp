// SPDX-License-Identifier: MIT
// Cooperative cancellation: a small stop_source / stop_token / stop_callback.
// Own implementation so it does not depend on standard-library support for <stop_token>.
#pragma once

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace jabba {

namespace detail {

struct StopState {
    std::mutex mutex;
    std::condition_variable callback_done;
    bool stopped = false;
    std::uint64_t next_id = 0;
    std::vector<std::pair<std::uint64_t, std::function<void()>>> callbacks;
    /// The callback request_stop is running right now, and on which thread.
    std::uint64_t running_id = 0;
    std::thread::id running_thread;
};

}  // namespace detail

class stop_token {
public:
    stop_token() = default;

    bool stop_requested() const {
        if (!state_) return false;
        std::lock_guard lock(state_->mutex);
        return state_->stopped;
    }

    bool stop_possible() const { return state_ != nullptr; }

private:
    friend class stop_source;
    friend class stop_callback;
    explicit stop_token(std::shared_ptr<detail::StopState> s) : state_(std::move(s)) {}
    std::shared_ptr<detail::StopState> state_;
};

class stop_source {
public:
    stop_source() : state_(std::make_shared<detail::StopState>()) {}

    stop_token get_token() const { return stop_token(state_); }

    bool stop_requested() const { return get_token().stop_requested(); }

    /// Returns false if stop was already requested. Callbacks run on this thread,
    /// one at a time, without the internal lock held. A callback must not throw:
    /// if it does, std::terminate is called (as with std::stop_callback).
    bool request_stop() {
        std::unique_lock lock(state_->mutex);
        if (state_->stopped) return false;
        state_->stopped = true;
        state_->running_thread = std::this_thread::get_id();
        while (!state_->callbacks.empty()) {
            auto [id, fn] = std::move(state_->callbacks.front());
            state_->callbacks.erase(state_->callbacks.begin());
            state_->running_id = id;
            lock.unlock();
            [&]() noexcept { fn(); }();
            lock.lock();
            state_->running_id = 0;
            state_->callback_done.notify_all();
        }
        return true;
    }

private:
    std::shared_ptr<detail::StopState> state_;
};

/// Runs `fn` once when stop is requested (immediately if it already was).
/// Like std::stop_callback, the destructor unregisters `fn`, or waits for it
/// if it is running on another thread right now.
class stop_callback {
public:
    stop_callback(const stop_token& token, std::function<void()> fn) : state_(token.state_) {
        if (!state_) return;
        {
            std::lock_guard lock(state_->mutex);
            if (!state_->stopped) {
                id_ = ++state_->next_id;
                state_->callbacks.emplace_back(id_, std::move(fn));
                return;
            }
        }
        [&]() noexcept { fn(); }();
    }

    stop_callback(const stop_callback&) = delete;
    stop_callback& operator=(const stop_callback&) = delete;

    ~stop_callback() {
        if (!state_ || id_ == 0) return;
        std::unique_lock lock(state_->mutex);
        auto& cbs = state_->callbacks;
        for (auto it = cbs.begin(); it != cbs.end(); ++it) {
            if (it->first == id_) {
                cbs.erase(it);
                return;
            }
        }
        if (state_->running_id == id_ && state_->running_thread != std::this_thread::get_id()) {
            state_->callback_done.wait(lock, [&] { return state_->running_id != id_; });
        }
    }

private:
    std::shared_ptr<detail::StopState> state_;
    std::uint64_t id_ = 0;
};

/// `co_await stopped(token)` resumes once stop is requested on `token`
/// (on the thread that requested it).
class stopped {
public:
    explicit stopped(stop_token token) : token_(std::move(token)) {}

    bool await_ready() const { return token_.stop_requested(); }

    bool await_suspend(std::coroutine_handle<> h) {
        // The callback may fire inside the stop_callback constructor (stop already
        // requested) or on another thread at any moment: whoever moves `state_` out of
        // `registering` decides who continues the coroutine.
        callback_ = std::make_unique<stop_callback>(token_, [this, h] {
            int expected = registering;
            if (!state_.compare_exchange_strong(expected, fired_inline)) h.resume();
        });
        int expected = registering;
        return state_.compare_exchange_strong(expected, suspended);
    }

    void await_resume() const noexcept {}

private:
    static constexpr int registering = 0, suspended = 1, fired_inline = 2;
    stop_token token_;
    std::atomic<int> state_{registering};
    std::unique_ptr<stop_callback> callback_;
};

}  // namespace jabba
