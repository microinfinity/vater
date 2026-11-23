#pragma once
/// @file vaterd.hpp
/// @brief L2 Persistent Session Daemon (vaterd) — Server & Client utilities.
/// Owns PTYs and replay ring buffers so terminal sessions survive UI close/crash.

#include "os/event_loop.hpp"
#include "os/ipc.hpp"
#include "os/pty.hpp"
#include "os/spawn.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace vater::daemon {

struct DaemonSessionInfo {
    uint32_t id{0};
    uint16_t cols{80};
    uint16_t rows{24};
    bool alive{true};
    std::string cwd;
};

/// Persistent session daemon server (`vaterd`).
class VaterDaemon {
public:
    explicit VaterDaemon(std::string socket_path = {});
    ~VaterDaemon();

    /// Runs the blocking epoll daemon loop until `request_stop()` or SIGTERM/SIGINT.
    int run();
    void request_stop() noexcept;

    [[nodiscard]] const std::string& socket_path() const noexcept { return socket_path_; }

private:
    struct HostedSession {
        uint32_t id{0};
        uint16_t cols{80};
        uint16_t rows{24};
        std::string cwd;
        os::Pty pty;
        os::ChildProcess child;
        std::deque<uint8_t> replay_ring; // Last 256 KB of raw PTY output for seamless re-attach
        static constexpr size_t kMaxReplayBytes = 256 * 1024;
    };

    struct ClientConn {
        int fd{-1};
        uint32_t attached_session{0};
        os::IpcFrameReader reader;
    };

    void handle_accept();
    void handle_client_io(int cfd);
    void handle_client_frame(ClientConn& client, os::IpcMsgType type, const std::vector<uint8_t>& payload);
    void handle_pty_readable(uint32_t session_id);
    void disconnect_client(int cfd);
    void reap_children();

    std::string socket_path_;
    int listen_fd_{-1};
    int sigchld_fd_{-1};
    os::EventLoop loop_;
    std::atomic<bool> running_{false};

    uint32_t next_session_id_{1};
    std::unordered_map<uint32_t, std::unique_ptr<HostedSession>> sessions_;
    std::unordered_map<int, std::unique_ptr<ClientConn>> clients_;
};

/// Queries a running `vaterd` instance for all active sessions (used by `vater --list-sessions`).
[[nodiscard]] bool query_daemon_sessions(const std::string& socket_path,
                                         std::vector<DaemonSessionInfo>& out_sessions);

} // namespace vater::daemon

