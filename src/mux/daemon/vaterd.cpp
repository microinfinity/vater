/// @file vaterd.cpp
/// @brief L2 Persistent Session Daemon (vaterd) — Server & Client implementation.

#include "mux/daemon/vaterd.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <sys/epoll.h>
#include <unistd.h>

namespace vater::daemon {

namespace {

uint16_t read_u16(const uint8_t* p) noexcept {
    uint16_t v = 0;
    std::memcpy(&v, p, 2);
    return v;
}

uint32_t read_u32(const uint8_t* p) noexcept {
    uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

void push_u16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

void push_u32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

} // namespace

VaterDaemon::VaterDaemon(std::string socket_path)
    : socket_path_(socket_path.empty() ? os::default_vaterd_socket_path(true) : std::move(socket_path)) {}

VaterDaemon::~VaterDaemon() {
    for (auto& [fd, _] : clients_) {
        if (fd >= 0) ::close(fd);
    }
    clients_.clear();
    for (auto& [_, sess] : sessions_) {
        sess->child.terminate();
        sess->pty.close();
    }
    sessions_.clear();
    if (sigchld_fd_ >= 0) ::close(sigchld_fd_);
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        ::unlink(socket_path_.c_str());
    }
}

void VaterDaemon::request_stop() noexcept {
    running_.store(false, std::memory_order_release);
    loop_.wake();
}

int VaterDaemon::run() {
    listen_fd_ = os::ipc_listen(socket_path_);
    if (listen_fd_ < 0) {
        std::fprintf(stderr, "vaterd: failed to listen on %s\n", socket_path_.c_str());
        return 1;
    }

    sigchld_fd_ = os::EventLoop::create_sigchld_fd();
    if (sigchld_fd_ >= 0) {
        loop_.add_fd(sigchld_fd_, EPOLLIN, [this](uint32_t) {
            os::EventLoop::drain_signal_fd(sigchld_fd_);
            reap_children();
        });
    }

    loop_.add_fd(listen_fd_, EPOLLIN, [this](uint32_t) {
        handle_accept();
    });

    running_.store(true, std::memory_order_release);
    while (running_.load(std::memory_order_acquire)) {
        loop_.poll_once(250);
        reap_children();
    }

    return 0;
}

void VaterDaemon::handle_accept() {
    for (;;) {
        pid_t peer_pid = 0;
        int cfd = os::ipc_accept_verified(listen_fd_, &peer_pid);
        if (cfd < 0) break;

        auto conn = std::make_unique<ClientConn>();
        conn->fd = cfd;
        clients_[cfd] = std::move(conn);

        loop_.add_fd(cfd, EPOLLIN | EPOLLHUP | EPOLLERR, [this, cfd](uint32_t) {
            handle_client_io(cfd);
        });
    }
}

void VaterDaemon::disconnect_client(int cfd) {
    loop_.remove_fd(cfd);
    auto it = clients_.find(cfd);
    if (it != clients_.end()) {
        ::close(cfd);
        clients_.erase(it);
    }
}

void VaterDaemon::handle_client_io(int cfd) {
    auto it = clients_.find(cfd);
    if (it == clients_.end()) return;
    ClientConn* client = it->second.get();

    int r = client->reader.read_from_fd(cfd);
    if (r < 0) {
        disconnect_client(cfd);
        return;
    }

    os::IpcMsgType type{};
    std::vector<uint8_t> payload;
    while (client->reader.pop_frame(type, payload)) {
        handle_client_frame(*client, type, payload);
        if (clients_.find(cfd) == clients_.end()) return;
    }
}

void VaterDaemon::handle_client_frame(ClientConn& client, os::IpcMsgType type, const std::vector<uint8_t>& payload) {
    switch (type) {
        case os::IpcMsgType::ListSessions: {
            std::vector<uint8_t> out;
            push_u32(out, static_cast<uint32_t>(sessions_.size()));
            for (const auto& [sid, s] : sessions_) {
                push_u32(out, sid);
                push_u16(out, s->cols);
                push_u16(out, s->rows);
                out.push_back(s->child.is_running() ? 1 : 0);
                push_u16(out, static_cast<uint16_t>(s->cwd.size()));
                out.insert(out.end(), s->cwd.begin(), s->cwd.end());
            }
            os::ipc_send_frame(client.fd, os::IpcMsgType::SessionList, out.data(),
                               static_cast<uint32_t>(out.size()));
            break;
        }

        case os::IpcMsgType::CreateSession: {
            uint16_t cols = 80, rows = 24;
            std::string cwd, term = "xterm-256color";
            size_t off = 0;
            if (payload.size() >= 4) {
                cols = std::max<uint16_t>(2, read_u16(payload.data() + 0));
                rows = std::max<uint16_t>(1, read_u16(payload.data() + 2));
                off = 4;
            }
            if (off + 2 <= payload.size()) {
                uint16_t clen = read_u16(payload.data() + off);
                off += 2;
                if (off + clen <= payload.size()) {
                    cwd.assign(reinterpret_cast<const char*>(payload.data() + off), clen);
                    off += clen;
                }
            }
            if (off + 2 <= payload.size()) {
                uint16_t tlen = read_u16(payload.data() + off);
                off += 2;
                if (off + tlen <= payload.size()) {
                    term.assign(reinterpret_cast<const char*>(payload.data() + off), tlen);
                }
            }

            auto hs = std::make_unique<HostedSession>();
            hs->id = next_session_id_++;
            hs->cols = cols;
            hs->rows = rows;
            hs->cwd = cwd;

            os::SpawnConfig scfg;
            scfg.working_dir = cwd;
            scfg.term = term;
            if (!hs->pty.open(cols, rows) || !os::spawn_in_pty(hs->pty, scfg, hs->child)) {
                os::ipc_send_frame(client.fd, os::IpcMsgType::Error, "failed to spawn PTY session");
                return;
            }

            const uint32_t sid = hs->id;
            const int pfd = hs->pty.master_fd();
            sessions_[sid] = std::move(hs);

            loop_.add_fd(pfd, EPOLLIN | EPOLLHUP | EPOLLERR, [this, sid](uint32_t) {
                handle_pty_readable(sid);
            });

            client.attached_session = sid;
            std::vector<uint8_t> ok_payload;
            push_u32(ok_payload, sid);
            os::ipc_send_frame(client.fd, os::IpcMsgType::AttachOk, ok_payload.data(),
                               static_cast<uint32_t>(ok_payload.size()));
            break;
        }

        case os::IpcMsgType::Attach: {
            if (payload.size() < 8) {
                os::ipc_send_frame(client.fd, os::IpcMsgType::Error, "malformed Attach");
                return;
            }
            const uint32_t sid = read_u32(payload.data() + 0);
            const uint16_t cols = std::max<uint16_t>(2, read_u16(payload.data() + 4));
            const uint16_t rows = std::max<uint16_t>(1, read_u16(payload.data() + 6));

            auto it = sessions_.find(sid);
            if (it == sessions_.end()) {
                os::ipc_send_frame(client.fd, os::IpcMsgType::Error, "session not found");
                return;
            }
            auto& hs = *it->second;
            client.attached_session = sid;
            hs.cols = cols;
            hs.rows = rows;
            hs.pty.resize(cols, rows);

            std::vector<uint8_t> ok_payload;
            ok_payload.reserve(4 + hs.replay_ring.size());
            push_u32(ok_payload, sid);
            ok_payload.insert(ok_payload.end(), hs.replay_ring.begin(), hs.replay_ring.end());
            os::ipc_send_frame(client.fd, os::IpcMsgType::AttachOk, ok_payload.data(),
                               static_cast<uint32_t>(ok_payload.size()));
            break;
        }

        case os::IpcMsgType::Detach: {
            client.attached_session = 0;
            break;
        }

        case os::IpcMsgType::Input: {
            auto it = sessions_.find(client.attached_session);
            if (it != sessions_.end() && !payload.empty()) {
                it->second->pty.write(payload.data(), payload.size());
            }
            break;
        }

        case os::IpcMsgType::Resize: {
            if (payload.size() >= 4) {
                auto it = sessions_.find(client.attached_session);
                if (it != sessions_.end()) {
                    const uint16_t cols = std::max<uint16_t>(2, read_u16(payload.data() + 0));
                    const uint16_t rows = std::max<uint16_t>(1, read_u16(payload.data() + 2));
                    it->second->cols = cols;
                    it->second->rows = rows;
                    it->second->pty.resize(cols, rows);
                }
            }
            break;
        }

        case os::IpcMsgType::KillSession: {
            if (payload.size() >= 4) {
                const uint32_t sid = read_u32(payload.data());
                auto it = sessions_.find(sid);
                if (it != sessions_.end()) {
                    loop_.remove_fd(it->second->pty.master_fd());
                    it->second->child.terminate();
                    it->second->pty.close();
                    sessions_.erase(it);
                }
            }
            break;
        }

        default:
            break;
    }
}

void VaterDaemon::handle_pty_readable(uint32_t session_id) {
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return;
    HostedSession& hs = *it->second;

    uint8_t buf[32768];
    for (;;) {
        ssize_t n = hs.pty.read(buf, sizeof(buf));
        if (n > 0) {
            // Append to replay ring buffer
            for (ssize_t i = 0; i < n; ++i) {
                if (hs.replay_ring.size() >= HostedSession::kMaxReplayBytes) {
                    hs.replay_ring.pop_front();
                }
                hs.replay_ring.push_back(buf[i]);
            }
            // Broadcast to all clients attached to this session
            for (auto& [cfd, conn] : clients_) {
                if (conn->attached_session == session_id) {
                    os::ipc_send_frame(cfd, os::IpcMsgType::Output, buf, static_cast<uint32_t>(n));
                }
            }
        } else {
            break;
        }
    }
}

void VaterDaemon::reap_children() {
    std::vector<uint32_t> dead;
    for (auto& [sid, hs] : sessions_) {
        if (hs->child.poll_exit()) {
            handle_pty_readable(sid);
            uint8_t payload[8];
            std::memcpy(payload + 0, &sid, 4);
            int32_t code = hs->child.exit_status;
            std::memcpy(payload + 4, &code, 4);
            for (auto& [cfd, conn] : clients_) {
                if (conn->attached_session == sid) {
                    os::ipc_send_frame(cfd, os::IpcMsgType::SessionExited, payload, sizeof(payload));
                    conn->attached_session = 0;
                }
            }
            dead.push_back(sid);
        }
    }
    for (uint32_t sid : dead) {
        auto it = sessions_.find(sid);
        if (it != sessions_.end()) {
            loop_.remove_fd(it->second->pty.master_fd());
            it->second->pty.close();
            sessions_.erase(it);
        }
    }
}

bool query_daemon_sessions(const std::string& socket_path,
                           std::vector<DaemonSessionInfo>& out_sessions) {
    out_sessions.clear();
    int fd = os::ipc_connect(socket_path);
    if (fd < 0) return false;

    if (!os::ipc_send_frame(fd, os::IpcMsgType::ListSessions, nullptr, 0)) {
        ::close(fd);
        return false;
    }

    os::IpcFrameReader reader;
    struct pollfd pfd{fd, POLLIN, 0};
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (::poll(&pfd, 1, 100) <= 0) continue;
        if (reader.read_from_fd(fd) < 0) break;
        os::IpcMsgType type{};
        std::vector<uint8_t> payload;
        if (reader.pop_frame(type, payload) && type == os::IpcMsgType::SessionList) {
            if (payload.size() >= 4) {
                const uint32_t count = read_u32(payload.data());
                size_t off = 4;
                for (uint32_t i = 0; i < count && off + 11 <= payload.size(); ++i) {
                    DaemonSessionInfo info{};
                    info.id = read_u32(payload.data() + off); off += 4;
                    info.cols = read_u16(payload.data() + off); off += 2;
                    info.rows = read_u16(payload.data() + off); off += 2;
                    info.alive = (payload[off++] != 0);
                    const uint16_t cwd_len = read_u16(payload.data() + off); off += 2;
                    if (off + cwd_len <= payload.size()) {
                        info.cwd.assign(reinterpret_cast<const char*>(payload.data() + off), cwd_len);
                        off += cwd_len;
                    }
                    out_sessions.push_back(std::move(info));
                }
            }
            ::close(fd);
            return true;
        }
    }

    ::close(fd);
    return false;
}

} // namespace vater::daemon

