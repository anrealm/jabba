// SPDX-License-Identifier: MIT
// task<T>: a lazy coroutine that is awaited by exactly one caller.
#pragma once

#include <atomic>
#include <coroutine>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

namespace jabba {

template <class T = void>
class task;

namespace detail {

// Handing control back and forth uses a "ready" flag rather than symmetric
// transfer: compilers are not required to turn symmetric transfer into a tail
// call (GCC does not at -O0/-O1), and a loop of synchronously completing awaits
// would then overflow the stack. Whoever flips the flag second continues the
// awaiting coroutine: the awaiter (inline, without suspending) if the task
// finished synchronously, or the task's final suspend otherwise.
struct promise_base {
    std::coroutine_handle<> continuation;
    std::atomic<bool> ready{false};
    std::exception_ptr error;

    std::suspend_always initial_suspend() noexcept { return {}; }

    struct final_awaiter {
        bool await_ready() noexcept { return false; }
        template <class P>
        void await_suspend(std::coroutine_handle<P> h) noexcept {
            auto& p = h.promise();
            if (p.ready.exchange(true, std::memory_order_acq_rel)) p.continuation.resume();
        }
        void await_resume() noexcept {}
    };
    final_awaiter final_suspend() noexcept { return {}; }

    void unhandled_exception() noexcept { error = std::current_exception(); }
};

template <class T>
struct promise : promise_base {
    std::optional<T> value;
    task<T> get_return_object() noexcept;
    template <class U>
    void return_value(U&& v) {
        value.emplace(std::forward<U>(v));
    }
    T take() {
        if (error) std::rethrow_exception(error);
        return std::move(*value);
    }
};

template <>
struct promise<void> : promise_base {
    task<void> get_return_object() noexcept;
    void return_void() noexcept {}
    void take() {
        if (error) std::rethrow_exception(error);
    }
};

}  // namespace detail

template <class T>
class [[nodiscard]] task {
public:
    using promise_type = detail::promise<T>;
    using value_type = T;

    task(task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    task& operator=(task&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }
    task(const task&) = delete;
    task& operator=(const task&) = delete;
    ~task() {
        if (handle_) handle_.destroy();
    }

    auto operator co_await() && noexcept {
        struct awaiter {
            std::coroutine_handle<promise_type> handle;
            bool await_ready() noexcept { return false; }
            bool await_suspend(std::coroutine_handle<> awaiting) noexcept {
                handle.promise().continuation = awaiting;
                handle.resume();
                return !handle.promise().ready.exchange(true, std::memory_order_acq_rel);
            }
            T await_resume() { return handle.promise().take(); }
        };
        return awaiter{handle_};
    }

private:
    friend promise_type;
    explicit task(std::coroutine_handle<promise_type> h) noexcept : handle_(h) {}
    std::coroutine_handle<promise_type> handle_;
};

template <class T>
task<T> detail::promise<T>::get_return_object() noexcept {
    return task<T>(std::coroutine_handle<promise<T>>::from_promise(*this));
}

inline task<void> detail::promise<void>::get_return_object() noexcept {
    return task<void>(std::coroutine_handle<promise<void>>::from_promise(*this));
}

template <class T>
struct is_task : std::false_type {};
template <class T>
struct is_task<task<T>> : std::true_type {};
template <class T>
inline constexpr bool is_task_v = is_task<T>::value;

/// The value type of a task; `void` for anything that is not a task.
template <class T>
struct task_value {
    using type = void;
};
template <class T>
struct task_value<task<T>> {
    using type = T;
};

namespace detail {

/// An eagerly started coroutine that destroys itself when it finishes.
/// Nobody else ever destroys it: whatever resumes it later finds it alive.
struct detached {
    struct promise_type {
        detached get_return_object() noexcept { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };
};

}  // namespace detail

}  // namespace jabba
