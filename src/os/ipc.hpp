#pragma once
/// @file ipc.hpp
/// @brief L1 OS Layer — Framed Binary Unix Socket IPC with 0700 runtime dir & SO_PEERCRED auth (§8).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vater::os {

/// Framed binary message header: 4-byte magic/type + 4-byte payload length (max 16 MB).
struct IpcFrameHeader {
    uint16_t magic{0x5654}; // 'VT'
    uint16_t type{0};
    uint32_t length{0};
};

static_assert(sizeof(IpcFrameHeader) == 8);

enum class IpcMsgType : uint16_t {
    Hello         = 1,
    CreateSession = 2, // payload: uint16 cols, uint16 rows, string cwd, string term
    ListSessions  = 3,
    SessionList   = 4, // response: uint32 count, [uint32 id, uint16 cols, uint16 rows, uint8 alive, string title, string cwd]
    Attach        = 5, // payload: uint32 session_id, uint16 cols, uint16 rows
    AttachOk      = 6, // payload: uint32 session_id, followed by initial replay bytes
    Detach        = 7,
    Input         = 8, // payload: raw bytes to PTY
    Output        = 9, // payload: raw bytes from PTY
    Resize        = 10,// payload: uint16 cols, uint16 rows
    KillSession   = 11,// payload: uint32 session_id
    SessionExited = 12,// payload: uint32 session_id, int32 exit_code
    Error         = 15,// payload: utf-8 error message
};

/// Returns `$XDG_RUNTIME_DIR/vater/vaterd.sock` (ensuring parent dir exists with 0700 perms).
[[nodiscard]] std::string default_vaterd_socket_path(bool create_dir = false);

/// Creates a listening AF_UNIX SOCK_STREAM server socket at `socket_path` (mode 0600).
[[nodiscard]] int ipc_listen(const std::string& socket_path);

/// Accepts a client and verifies `SO_PEERCRED` matches current UID (§8 security!).
/// Returns accepted non-blocking fd or -1 if rejected/EAGAIN.
[[nodiscard]] int ipc_accept_verified(int listen_fd, pid_t* out_peer_pid = nullptr);

/// Connects as a client to `socket_path`. Returns fd or -1.
[[nodiscard]] int ipc_connect(const std::string& socket_path);

/// Sends a complete framed message (blocking/retry on EINTR/EAGAIN with short timeout).
bool ipc_send_frame(int fd, IpcMsgType type, const void* payload, uint32_t len);
inline bool ipc_send_frame(int fd, IpcMsgType type, std::string_view s) {
    return ipc_send_frame(fd, type, s.data(), static_cast<uint32_t>(s.size()));
}

/// Incremental non-blocking frame reader for epoll-driven IPC endpoints.
class IpcFrameReader {
public:
    static constexpr uint32_t kMaxFrameBytes = 16u * 1024u * 1024u;

    /// Reads available bytes from `fd`. Returns:
    ///   1 = got data, 0 = EAGAIN (no data yet), -1 = EOF/error/protocol violation.
    int read_from_fd(int fd);

    /// Pops next complete frame if buffered. Returns false if incomplete.
    bool pop_frame(IpcMsgType& out_type, std::vector<uint8_t>& out_payload);

private:
    std::vector<uint8_t> buf_;
};

} // namespace vater::os

