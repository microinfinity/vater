#pragma once
/// @file event_loop.hpp
/// @brief L1 OS Layer — epoll + eventfd + signalfd + timerfd event loop primitives.

#include <cstdint>
#include <functional>
#include <unordered_map>

namespace vater::os {

/// Lightweight Linux epoll wrapper supporting fd callbacks, coalesced eventfd wakeups,
/// signalfd (SIGCHLD), and timerfd (cursor blink / resize debounce).
class EventLoop {
public:
    using FdCallback = std::function<void(uint32_t events)>;

    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    [[nodiscard]] bool is_valid() const noexcept { return epoll_fd_ >= 0; }
    [[nodiscard]] int epoll_fd() const noexcept { return epoll_fd_; }
    [[nodiscard]] int wake_fd() const noexcept { return wake_fd_; }

    /// Registers/updates/removes an fd in epoll.
    bool add_fd(int fd, uint32_t events, FdCallback cb);
    bool mod_fd(int fd, uint32_t events);
    void remove_fd(int fd);

    /// Thread-safe wakeup of `wait()` via coalesced eventfd.
    void wake() noexcept;

    /// Creates a timerfd (monotonic, non-blocking, CLOEXEC). Returns fd or -1.
    static int create_timer_fd();
    static bool arm_timer_ms(int tfd, uint32_t initial_ms, uint32_t interval_ms = 0);
    static uint64_t drain_timer_fd(int tfd) noexcept;

    /// Creates a signalfd for SIGCHLD (blocking SIGCHLD on current thread). Returns fd or -1.
    static int create_sigchld_fd();
    static void drain_signal_fd(int sfd) noexcept;

    /// Waits up to `timeout_ms` (-1 = infinite, 0 = non-blocking) and dispatches ready callbacks.
    /// Returns number of events dispatched, or -1 on fatal error.
    int poll_once(int timeout_ms);

private:
    int epoll_fd_{-1};
    int wake_fd_{-1};
    std::unordered_map<int, FdCallback> callbacks_;
};

} // namespace vater::os

