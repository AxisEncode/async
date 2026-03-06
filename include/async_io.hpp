#pragma once

#include <coroutine>
#include <cstdint>
#include <errno.h>
#include <fcntl.h>
#include <optional>
#include <poll.h>
#include <queue>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>
#include <chrono>
#include <unistd.h>

namespace as {

template <typename T>
class task;

template <typename T>
class task {
public:
    struct promise_type {
        std::optional<T> value;
        std::exception_ptr exception;
        std::coroutine_handle<> continuation;

        task get_return_object() {
            return task(std::coroutine_handle<promise_type>::from_promise(*this));
        }

        std::suspend_always initial_suspend() noexcept { return {}; }

        struct final_awaiter {
            bool await_ready() const noexcept { return false; }

            template <typename Promise>
            std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) const noexcept {
                auto cont = h.promise().continuation;
                return cont ? cont : std::noop_coroutine();
            }

            void await_resume() const noexcept {}
        };

        final_awaiter final_suspend() noexcept { return {}; }

        void return_value(T v) noexcept(std::is_nothrow_move_constructible_v<T>) {
            value = std::move(v);
        }

        void unhandled_exception() { exception = std::current_exception(); }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    task() = default;
    explicit task(handle_type h) : handle_(h) {}

    task(const task&) = delete;
    task& operator=(const task&) = delete;

    task(task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    task& operator=(task&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    ~task() {
        if (handle_) {
            handle_.destroy();
        }
    }

    bool await_ready() const noexcept {
        return !handle_ || handle_.done();
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept {
        handle_.promise().continuation = continuation;
        return handle_;
    }

    T await_resume() {
        auto& promise = handle_.promise();
        if (promise.exception) {
            std::rethrow_exception(promise.exception);
        }
        return std::move(*promise.value);
    }

    bool done() const noexcept { return !handle_ || handle_.done(); }

private:
    handle_type handle_{};
};

template <>
class task<void> {
public:
    struct promise_type {
        std::exception_ptr exception;
        std::coroutine_handle<> continuation;

        task get_return_object() {
            return task(std::coroutine_handle<promise_type>::from_promise(*this));
        }

        std::suspend_always initial_suspend() noexcept { return {}; }

        struct final_awaiter {
            bool await_ready() const noexcept { return false; }

            template <typename Promise>
            std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) const noexcept {
                auto cont = h.promise().continuation;
                return cont ? cont : std::noop_coroutine();
            }

            void await_resume() const noexcept {}
        };

        final_awaiter final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() { exception = std::current_exception(); }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    task() = default;
    explicit task(handle_type h) : handle_(h) {}

    task(const task&) = delete;
    task& operator=(const task&) = delete;

    task(task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    task& operator=(task&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    ~task() {
        if (handle_) {
            handle_.destroy();
        }
    }

    bool await_ready() const noexcept {
        return !handle_ || handle_.done();
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept {
        handle_.promise().continuation = continuation;
        return handle_;
    }

    void await_resume() {
        auto& promise = handle_.promise();
        if (promise.exception) {
            std::rethrow_exception(promise.exception);
        }
    }

    bool done() const noexcept { return !handle_ || handle_.done(); }

private:
    handle_type handle_{};
};

class io_context {
public:
    io_context() = default;

    void post(std::coroutine_handle<> h) {
        ready_.push_back(h);
    }

    struct fd_awaitable {
        io_context& ctx;
        int fd;
        short events;

        bool await_ready() const noexcept { return false; }

        void await_suspend(std::coroutine_handle<> h) {
            ctx.register_fd(fd, events, h);
        }

        void await_resume() const noexcept {}
    };

    struct sleep_awaitable {
        io_context& ctx;
        std::chrono::steady_clock::duration duration;

        bool await_ready() const noexcept {
            return duration.count() <= 0;
        }

        void await_suspend(std::coroutine_handle<> h) {
            ctx.add_timer(std::chrono::steady_clock::now() + duration, h);
        }

        void await_resume() const noexcept {}
    };

    struct yield_awaitable {
        io_context& ctx;

        bool await_ready() const noexcept { return false; }

        void await_suspend(std::coroutine_handle<> h) {
            ctx.post(h);
        }

        void await_resume() const noexcept {}
    };

    fd_awaitable readable(int fd) { return fd_awaitable{*this, fd, POLLIN}; }
    fd_awaitable writable(int fd) { return fd_awaitable{*this, fd, POLLOUT}; }
    yield_awaitable yield_now() { return yield_awaitable{*this}; }

    template <typename Rep, typename Period>
    sleep_awaitable sleep_for(std::chrono::duration<Rep, Period> d) {
        return sleep_awaitable{*this, std::chrono::duration_cast<std::chrono::steady_clock::duration>(d)};
    }

    void co_spawn(task<void> t) {
        ++active_tasks_;
        detached_task d = run_task(this, std::move(t));
        post(d.release());
    }

    void run() {
        while (!stop_requested_) {
            drain_ready();
            drain_timers();

            if (ready_.empty() && !has_pending_work()) {
                break;
            }

            if (!ready_.empty()) {
                continue;
            }

            std::vector<pollfd> fds = build_pollfds();
            int timeout_ms = compute_poll_timeout_ms();

            if (fds.empty()) {
                if (timeout_ms < 0) {
                    break;
                }
                ::poll(nullptr, 0, timeout_ms);
                continue;
            }

            int rc = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeout_ms);
            if (rc < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::system_error(errno, std::generic_category(), "poll failed");
            }

            if (rc > 0) {
                for (const auto& pfd : fds) {
                    if (pfd.revents == 0) {
                        continue;
                    }
                    resume_fd_waiters(pfd);
                }
            }
        }

        stop_requested_ = false;
    }

    void stop() {
        stop_requested_ = true;
    }

private:
    class ready_queue {
    public:
        bool empty() const noexcept { return size_ == 0; }

        void push_back(std::coroutine_handle<> h) {
            if (size_ == buffer_.size()) {
                grow();
            }
            buffer_[(head_ + size_) % buffer_.size()] = h;
            ++size_;
        }

        std::coroutine_handle<> front() const noexcept {
            return buffer_[head_];
        }

        void pop_front() noexcept {
            head_ = (head_ + 1) % buffer_.size();
            --size_;
        }

    private:
        void grow() {
            size_t new_capacity = buffer_.empty() ? 64 : buffer_.size() * 2;
            std::vector<std::coroutine_handle<>> new_buffer(new_capacity);
            for (size_t i = 0; i < size_; ++i) {
                new_buffer[i] = buffer_[(head_ + i) % buffer_.size()];
            }
            buffer_ = std::move(new_buffer);
            head_ = 0;
        }

        std::vector<std::coroutine_handle<>> buffer_;
        size_t head_{0};
        size_t size_{0};
    };

    struct fd_waiters {
        std::coroutine_handle<> read{};
        std::coroutine_handle<> write{};
        size_t active_index{0};
        bool active{false};
    };

    struct timer_entry {
        std::chrono::steady_clock::time_point when;
        uint64_t seq;
        std::coroutine_handle<> handle;

        bool operator>(const timer_entry& other) const {
            if (when != other.when) {
                return when > other.when;
            }
            return seq > other.seq;
        }
    };

    struct detached_task {
        struct promise_type {
            detached_task get_return_object() {
                return detached_task(std::coroutine_handle<promise_type>::from_promise(*this));
            }
            std::suspend_always initial_suspend() noexcept { return {}; }
            std::suspend_never final_suspend() noexcept { return {}; }
            void return_void() noexcept {}
            void unhandled_exception() noexcept { std::terminate(); }
        };

        using handle_type = std::coroutine_handle<promise_type>;

        explicit detached_task(handle_type h) : handle(h) {}

        detached_task(const detached_task&) = delete;
        detached_task& operator=(const detached_task&) = delete;

        detached_task(detached_task&& other) noexcept : handle(std::exchange(other.handle, {})) {}
        detached_task& operator=(detached_task&& other) noexcept {
            if (this != &other) {
                if (handle) {
                    handle.destroy();
                }
                handle = std::exchange(other.handle, {});
            }
            return *this;
        }

        ~detached_task() {
            if (handle) {
                handle.destroy();
            }
        }

        std::coroutine_handle<> release() {
            return std::exchange(handle, {});
        }

        handle_type handle{};
    };

    static detached_task run_task(io_context* self, task<void> t) {
        try {
            co_await t;
        } catch (...) {
        }
        --self->active_tasks_;
    }

    void ensure_fd_capacity(int fd) {
        if (fd < 0) {
            throw std::invalid_argument("fd must be non-negative");
        }
        auto idx = static_cast<size_t>(fd);
        if (idx >= fd_waiters_.size()) {
            fd_waiters_.resize(idx + 1);
        }
    }

    void track_fd_if_needed(int fd, fd_waiters& slot) {
        if (slot.active) {
            return;
        }
        slot.active = true;
        slot.active_index = active_fds_.size();
        active_fds_.push_back(fd);
    }

    void untrack_fd_if_unused(int fd, fd_waiters& slot) {
        if (slot.read || slot.write || !slot.active) {
            return;
        }
        size_t idx = slot.active_index;
        int moved_fd = active_fds_.back();
        active_fds_[idx] = moved_fd;
        fd_waiters_[static_cast<size_t>(moved_fd)].active_index = idx;
        active_fds_.pop_back();
        slot.active = false;
    }

    void register_fd(int fd, short events, std::coroutine_handle<> h) {
        ensure_fd_capacity(fd);
        auto& slot = fd_waiters_[static_cast<size_t>(fd)];

        if (events & POLLIN) {
            if (!slot.read) {
                ++waiter_count_;
            }
            slot.read = h;
        }
        if (events & POLLOUT) {
            if (!slot.write) {
                ++waiter_count_;
            }
            slot.write = h;
        }
        track_fd_if_needed(fd, slot);
    }

    void add_timer(std::chrono::steady_clock::time_point tp, std::coroutine_handle<> h) {
        timers_.push(timer_entry{tp, next_timer_seq_++, h});
    }

    void drain_ready() {
        while (!ready_.empty()) {
            auto h = ready_.front();
            ready_.pop_front();
            if (!h.done()) {
                h.resume();
            }
        }
    }

    void drain_timers() {
        auto now = std::chrono::steady_clock::now();
        while (!timers_.empty() && timers_.top().when <= now) {
            auto h = timers_.top().handle;
            timers_.pop();
            post(h);
        }
    }

    bool has_pending_work() const {
        return active_tasks_ > 0 || !timers_.empty() || waiter_count_ > 0;
    }

    int compute_poll_timeout_ms() const {
        if (timers_.empty()) {
            return -1;
        }
        auto now = std::chrono::steady_clock::now();
        auto delta = timers_.top().when - now;
        if (delta <= std::chrono::steady_clock::duration::zero()) {
            return 0;
        }
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(delta).count();
        return static_cast<int>(ms);
    }

    std::vector<pollfd> build_pollfds() const {
        std::vector<pollfd> fds;
        fds.reserve(active_fds_.size());

        for (int fd : active_fds_) {
            const auto& slot = fd_waiters_[static_cast<size_t>(fd)];
            short events = 0;
            if (slot.read) {
                events |= POLLIN;
            }
            if (slot.write) {
                events |= POLLOUT;
            }
            if (events != 0) {
                fds.push_back(pollfd{fd, events, 0});
            }
        }
        return fds;
    }

    void resume_fd_waiters(const pollfd& pfd) {
        if (pfd.fd < 0) {
            return;
        }
        auto idx = static_cast<size_t>(pfd.fd);
        if (idx >= fd_waiters_.size()) {
            return;
        }
        auto& slot = fd_waiters_[idx];

        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            if (slot.read) {
                post(slot.read);
                slot.read = {};
                --waiter_count_;
            }
            if (slot.write) {
                post(slot.write);
                slot.write = {};
                --waiter_count_;
            }
            untrack_fd_if_unused(pfd.fd, slot);
            return;
        }

        if ((pfd.revents & POLLIN) != 0 && slot.read) {
            post(slot.read);
            slot.read = {};
            --waiter_count_;
        }

        if ((pfd.revents & POLLOUT) != 0 && slot.write) {
            post(slot.write);
            slot.write = {};
            --waiter_count_;
        }
        untrack_fd_if_unused(pfd.fd, slot);
    }

    ready_queue ready_;
    std::priority_queue<timer_entry, std::vector<timer_entry>, std::greater<>> timers_;
    std::vector<fd_waiters> fd_waiters_;
    std::vector<int> active_fds_;
    size_t waiter_count_{0};
    uint64_t next_timer_seq_{0};
    size_t active_tasks_{0};
    bool stop_requested_{false};
};

inline void set_non_blocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        throw std::system_error(errno, std::generic_category(), "fcntl(F_GETFL) failed");
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw std::system_error(errno, std::generic_category(), "fcntl(F_SETFL) failed");
    }
}

inline task<ssize_t> async_read(io_context& ctx, int fd, void* buffer, size_t count) {
    while (true) {
        ssize_t n = ::read(fd, buffer, count);
        if (n >= 0) {
            co_return n;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            co_await ctx.readable(fd);
            continue;
        }
        throw std::system_error(errno, std::generic_category(), "read failed");
    }
}

inline task<ssize_t> async_write(io_context& ctx, int fd, const void* buffer, size_t count) {
    const char* ptr = static_cast<const char*>(buffer);
    size_t remaining = count;

    while (remaining > 0) {
        ssize_t n = ::write(fd, ptr, remaining);
        if (n > 0) {
            ptr += n;
            remaining -= static_cast<size_t>(n);
            continue;
        }
        if (n == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            co_await ctx.writable(fd);
            continue;
        }
        throw std::system_error(errno, std::generic_category(), "write failed");
    }

    co_return static_cast<ssize_t>(count - remaining);
}

}  // namespace as
