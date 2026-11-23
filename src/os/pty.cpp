/// @file pty.cpp
/// @brief L1 OS Layer — Pure POSIX PTY implementation.

#include "os/pty.hpp"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace vater::os {

Pty::~Pty() {
    close();
}

Pty::Pty(Pty&& other) noexcept
    : master_fd_(other.master_fd_), slave_name_(std::move(other.slave_name_)) {
    other.master_fd_ = -1;
}

Pty& Pty::operator=(Pty&& other) noexcept {
    if (this != &other) {
        close();
        master_fd_ = other.master_fd_;
        slave_name_ = std::move(other.slave_name_);
        other.master_fd_ = -1;
    }
    return *this;
}

bool Pty::open(uint16_t cols, uint16_t rows, uint16_t xpixel, uint16_t ypixel) {
    close();

    int fd = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) return false;

    if (::grantpt(fd) != 0 || ::unlockpt(fd) != 0) {
        ::close(fd);
        return false;
    }

    char name_buf[256];
    if (::ptsname_r(fd, name_buf, sizeof(name_buf)) != 0) {
        ::close(fd);
        return false;
    }

    // Set master fd non-blocking for epoll I/O thread
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    master_fd_ = fd;
    slave_name_ = name_buf;
    resize(cols, rows, xpixel, ypixel);
    return true;
}

int Pty::open_slave() const {
    if (slave_name_.empty()) return -1;
    int sfd = ::open(slave_name_.c_str(), O_RDWR | O_NOCTTY);
    if (sfd < 0) return -1;

    // Enable UTF-8 mode (IUTF8) on slave termios
    struct termios tio{};
    if (::tcgetattr(sfd, &tio) == 0) {
#ifdef IUTF8
        tio.c_iflag |= IUTF8;
#endif
        ::tcsetattr(sfd, TCSANOW, &tio);
    }
    return sfd;
}

bool Pty::resize(uint16_t cols, uint16_t rows, uint16_t xpixel, uint16_t ypixel) {
    if (master_fd_ < 0) return false;
    struct winsize ws{};
    ws.ws_col = cols;
    ws.ws_row = rows;
    ws.ws_xpixel = xpixel;
    ws.ws_ypixel = ypixel;
    return ::ioctl(master_fd_, TIOCSWINSZ, &ws) == 0;
}

ssize_t Pty::read(uint8_t* buf, size_t cap) {
    if (master_fd_ < 0 || !buf || cap == 0) return -1;
    for (;;) {
        ssize_t n = ::read(master_fd_, buf, cap);
        if (n > 0) return n;
        if (n == 0) return -1; // EOF
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1; // EIO when slave closes
    }
}

ssize_t Pty::write(const uint8_t* data, size_t len) {
    if (master_fd_ < 0 || !data || len == 0) return 0;
    size_t written = 0;
    while (written < len) {
        ssize_t n = ::write(master_fd_, data + written, len - written);
        if (n > 0) {
            written += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd{master_fd_, POLLOUT, 0};
            if (::poll(&pfd, 1, 50) <= 0) break;
            continue;
        }
        return -1;
    }
    return static_cast<ssize_t>(written);
}

void Pty::close() noexcept {
    if (master_fd_ >= 0) {
        ::close(master_fd_);
        master_fd_ = -1;
    }
    slave_name_.clear();
}

} // namespace vater::os

