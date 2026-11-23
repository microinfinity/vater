/// @file ipc.cpp
/// @brief L1 OS Layer — Framed Binary Unix Socket IPC with SO_PEERCRED verification.

#include "os/ipc.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace vater::os {

std::string default_vaterd_socket_path(bool create_dir) {
    const char* rt = std::getenv("XDG_RUNTIME_DIR");
    std::string dir;
    if (rt && rt[0] != '\0') {
        dir = std::string(rt) + "/vater";
    } else {
        dir = "/tmp/vater-" + std::to_string(::getuid());
    }
    if (create_dir) {
        ::mkdir(dir.c_str(), 0700);
        ::chmod(dir.c_str(), 0700);
    }
    return dir + "/vaterd.sock";
}

int ipc_listen(const std::string& socket_path) {
    if (socket_path.size() >= sizeof(sockaddr_un::sun_path)) return -1;

    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    ::unlink(socket_path.c_str());

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    ::chmod(socket_path.c_str(), 0600);

    if (::listen(fd, 16) != 0) {
        ::close(fd);
        ::unlink(socket_path.c_str());
        return -1;
    }
    return fd;
}

int ipc_accept_verified(int listen_fd, pid_t* out_peer_pid) {
    if (listen_fd < 0) return -1;
    int cfd = ::accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (cfd < 0) return -1;

    // Verify peer credentials (SO_PEERCRED): must belong to the same user UID (§8)
    struct ucred cred{};
    socklen_t len = sizeof(cred);
    if (::getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 || cred.uid != ::geteuid()) {
        ::close(cfd);
        return -1;
    }
    if (out_peer_pid) *out_peer_pid = cred.pid;
    return cfd;
}

int ipc_connect(const std::string& socket_path) {
    if (socket_path.size() >= sizeof(sockaddr_un::sun_path)) return -1;

    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }

    // Verify server peer UID as well
    struct ucred cred{};
    socklen_t len = sizeof(cred);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 || cred.uid != ::geteuid()) {
        ::close(fd);
        return -1;
    }

    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

namespace {

bool write_all_fd(int fd, const uint8_t* ptr, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, ptr + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd{fd, POLLOUT, 0};
            if (::poll(&pfd, 1, 200) <= 0) return false;
            continue;
        }
        return false;
    }
    return true;
}

} // namespace

bool ipc_send_frame(int fd, IpcMsgType type, const void* payload, uint32_t len) {
    if (fd < 0 || len > IpcFrameReader::kMaxFrameBytes) return false;
    IpcFrameHeader hdr{};
    hdr.magic = 0x5654;
    hdr.type = static_cast<uint16_t>(type);
    hdr.length = len;
    if (!write_all_fd(fd, reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr))) return false;
    if (len > 0 && payload) {
        if (!write_all_fd(fd, static_cast<const uint8_t*>(payload), len)) return false;
    }
    return true;
}

int IpcFrameReader::read_from_fd(int fd) {
    if (fd < 0) return -1;
    uint8_t tmp[16384];
    bool any = false;
    for (;;) {
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n > 0) {
            if (buf_.size() + static_cast<size_t>(n) > kMaxFrameBytes + sizeof(IpcFrameHeader)) {
                return -1; // Flood protection
            }
            buf_.insert(buf_.end(), tmp, tmp + n);
            any = true;
            continue;
        }
        if (n == 0) return any ? 1 : -1;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return any ? 1 : 0;
        return -1;
    }
}

bool IpcFrameReader::pop_frame(IpcMsgType& out_type, std::vector<uint8_t>& out_payload) {
    if (buf_.size() < sizeof(IpcFrameHeader)) return false;
    IpcFrameHeader hdr{};
    std::memcpy(&hdr, buf_.data(), sizeof(hdr));
    if (hdr.magic != 0x5654 || hdr.length > kMaxFrameBytes) {
        buf_.clear();
        return false;
    }
    const size_t total = sizeof(IpcFrameHeader) + hdr.length;
    if (buf_.size() < total) return false;

    out_type = static_cast<IpcMsgType>(hdr.type);
    out_payload.assign(buf_.begin() + static_cast<ptrdiff_t>(sizeof(IpcFrameHeader)),
                       buf_.begin() + static_cast<ptrdiff_t>(total));
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<ptrdiff_t>(total));
    return true;
}

} // namespace vater::os

