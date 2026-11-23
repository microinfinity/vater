/// @file event_loop.cpp
/// @brief L1 OS Layer — epoll + eventfd + signalfd + timerfd implementation.

#include "os/event_loop.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <vector>

namespace vater::os {

EventLoop::EventLoop() {
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (epoll_fd_ >= 0 && wake_fd_ >= 0) {
        struct epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = wake_fd_;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &ev);
    }
}

EventLoop::~EventLoop() {
    if (wake_fd_ >= 0) ::close(wake_fd_);
    if (epoll_fd_ >= 0) ::close(epoll_fd_);
}

bool EventLoop::add_fd(int fd, uint32_t events, FdCallback cb) {
    if (epoll_fd_ < 0 || fd < 0) return false;
    struct epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) != 0) {
        if (errno == EEXIST) {
            if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) != 0) return false;
        } else {
            return false;
        }
    }
    callbacks_[fd] = std::move(cb);
    return true;
}

bool EventLoop::mod_fd(int fd, uint32_t events) {
    if (epoll_fd_ < 0 || fd < 0) return false;
    struct epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    return ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) == 0;
}

void EventLoop::remove_fd(int fd) {
    if (epoll_fd_ >= 0 && fd >= 0) {
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    }
    callbacks_.erase(fd);
}

void EventLoop::wake() noexcept {
    if (wake_fd_ < 0) return;
    uint64_t one = 1;
    ssize_t r = ::write(wake_fd_, &one, sizeof(one));
    (void)r;
}

int EventLoop::create_timer_fd() {
    return ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
}

bool EventLoop::arm_timer_ms(int tfd, uint32_t initial_ms, uint32_t interval_ms) {
    if (tfd < 0) return false;
    struct itimerspec its{};
    its.it_value.tv_sec = static_cast<time_t>(initial_ms / 1000u);
    its.it_value.tv_nsec = static_cast<long>((initial_ms % 1000u) * 1000000u);
    its.it_interval.tv_sec = static_cast<time_t>(interval_ms / 1000u);
    its.it_interval.tv_nsec = static_cast<long>((interval_ms % 1000u) * 1000000u);
    return ::timerfd_settime(tfd, 0, &its, nullptr) == 0;
}

uint64_t EventLoop::drain_timer_fd(int tfd) noexcept {
    uint64_t exp = 0;
    if (tfd >= 0 && ::read(tfd, &exp, sizeof(exp)) == static_cast<ssize_t>(sizeof(exp))) {
        return exp;
    }
    return 0;
}

int EventLoop::create_sigchld_fd() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    ::pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    return ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
}

void EventLoop::drain_signal_fd(int sfd) noexcept {
    if (sfd < 0) return;
    struct signalfd_siginfo si{};
    while (::read(sfd, &si, sizeof(si)) == static_cast<ssize_t>(sizeof(si))) {}
}

int EventLoop::poll_once(int timeout_ms) {
    if (epoll_fd_ < 0) return -1;
    struct epoll_event events[32];
    int n = ::epoll_wait(epoll_fd_, events, 32, timeout_ms);
    if (n < 0) {
        return (errno == EINTR) ? 0 : -1;
    }
    for (int i = 0; i < n; ++i) {
        const int fd = events[i].data.fd;
        if (fd == wake_fd_) {
            uint64_t val = 0;
            while (::read(wake_fd_, &val, sizeof(val)) > 0) {}
            continue;
        }
        auto it = callbacks_.find(fd);
        if (it != callbacks_.end() && it->second) {
            auto cb = it->second; // copy in case callback modifies callbacks_
            cb(events[i].events);
        }
    }
    return n;
}

} // namespace vater::os

