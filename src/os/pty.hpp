#pragma once
/// @file pty.hpp
/// @brief L1 OS Layer — Pure POSIX PTY (posix_openpt / grantpt / unlockpt / ptsname_r / TIOCSWINSZ).
/// Zero libutil/forkpty dependency.

#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/types.h>

namespace vater::os {

class Pty {
public:
    Pty() = default;
    ~Pty();

    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;
    Pty(Pty&& other) noexcept;
    Pty& operator=(Pty&& other) noexcept;

    /// Opens a new PTY master/slave pair using pure POSIX APIs.
    bool open(uint16_t cols, uint16_t rows, uint16_t xpixel = 0, uint16_t ypixel = 0);

    /// Opens the slave fd (called in child process after setsid).
    [[nodiscard]] int open_slave() const;

    /// Resizes PTY via ioctl(TIOCSWINSZ), triggering SIGWINCH in the child process group.
    bool resize(uint16_t cols, uint16_t rows, uint16_t xpixel = 0, uint16_t ypixel = 0);

    /// Non-blocking read from master fd. Returns >0 bytes read, 0 on EAGAIN, -1 on EOF/error.
    ssize_t read(uint8_t* buf, size_t cap);

    /// Writes bytes to master fd (handles partial writes / EINTR).
    ssize_t write(const uint8_t* data, size_t len);
    ssize_t write(std::string_view s) {
        return write(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }

    void close() noexcept;

    [[nodiscard]] int master_fd() const noexcept { return master_fd_; }
    [[nodiscard]] bool is_open() const noexcept { return master_fd_ >= 0; }
    [[nodiscard]] const std::string& slave_name() const noexcept { return slave_name_; }

private:
    int master_fd_{-1};
    std::string slave_name_;
};

} // namespace vater::os

