/// @file session.cpp
/// @brief L2 Session & Mux — Terminal Session implementation (local PTY + remote vaterd + asciicast v2).

#include "mux/session.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <poll.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <vector>

namespace vater::mux {

namespace {

void append_utf8_cp(std::string& out, uint32_t cp) {
    if (cp < 0x80u) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0x10FFFFu) {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

int hex_nibble(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

bool parse_json_string(std::string_view s, size_t& pos, std::string& out) {
    out.clear();
    if (pos >= s.size() || s[pos] != '"') return false;
    ++pos;
    while (pos < s.size()) {
        const char c = s[pos++];
        if (c == '"') return true;
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (pos >= s.size()) return false;
        const char esc = s[pos++];
        switch (esc) {
            case '"':  out.push_back('"');  break;
            case '\\': out.push_back('\\'); break;
            case '/':  out.push_back('/');  break;
            case 'b':  out.push_back('\b'); break;
            case 'f':  out.push_back('\f'); break;
            case 'n':  out.push_back('\n'); break;
            case 'r':  out.push_back('\r'); break;
            case 't':  out.push_back('\t'); break;
            case 'u': {
                if (pos + 4 > s.size()) return false;
                uint32_t u = 0;
                for (int i = 0; i < 4; ++i) {
                    int h = hex_nibble(s[pos + static_cast<size_t>(i)]);
                    if (h < 0) return false;
                    u = (u << 4) | static_cast<uint32_t>(h);
                }
                pos += 4;
                // Handle UTF-16 surrogate pairs
                if (u >= 0xD800u && u <= 0xDBFFu && pos + 6 <= s.size() &&
                    s[pos] == '\\' && s[pos + 1] == 'u') {
                    uint32_t low = 0;
                    bool ok_low = true;
                    for (int i = 0; i < 4; ++i) {
                        int h = hex_nibble(s[pos + 2 + static_cast<size_t>(i)]);
                        if (h < 0) { ok_low = false; break; }
                        low = (low << 4) | static_cast<uint32_t>(h);
                    }
                    if (ok_low && low >= 0xDC00u && low <= 0xDFFFu) {
                        pos += 6;
                        u = 0x10000u + (((u - 0xD800u) << 10) | (low - 0xDC00u));
                    }
                }
                append_utf8_cp(out, u);
                break;
            }
            default:
                out.push_back(esc);
                break;
        }
    }
    return false;
}

bool parse_asciicast_event_line(std::string_view line, double& out_time,
                                std::string& out_kind, std::string& out_data) {
    size_t i = 0;
    auto skip_ws = [&]() {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    };
    skip_ws();
    if (i >= line.size() || line[i] != '[') return false;
    ++i;
    skip_ws();
    const char* num_start = line.data() + i;
    char* num_end = nullptr;
    out_time = std::strtod(num_start, &num_end);
    if (num_end == num_start) return false;
    i = static_cast<size_t>(num_end - line.data());
    skip_ws();
    if (i >= line.size() || line[i] != ',') return false;
    ++i;
    skip_ws();
    if (!parse_json_string(line, i, out_kind)) return false;
    skip_ws();
    if (i >= line.size() || line[i] != ',') return false;
    ++i;
    skip_ws();
    if (!parse_json_string(line, i, out_data)) return false;
    return true;
}

bool parse_asciicast_header_dim(std::string_view header, std::string_view key, int& out_val) {
    const std::string needle = "\"" + std::string(key) + "\"";
    size_t p = header.find(needle);
    if (p == std::string_view::npos) return false;
    p += needle.size();
    while (p < header.size() && (std::isspace(static_cast<unsigned char>(header[p])) || header[p] == ':')) ++p;
    if (p >= header.size() || !std::isdigit(static_cast<unsigned char>(header[p]))) return false;
    out_val = std::atoi(header.data() + p);
    return out_val > 0;
}

std::string json_escape_bytes(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(len + 16);
    for (size_t i = 0; i < len; ++i) {
        const uint8_t c = data[i];
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20u) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

} // namespace

Session::Session(uint32_t id, int cols, int rows, size_t max_scrollback)
    : id_(id), terminal_(cols, rows, max_scrollback) {}

Session::~Session() {
    stop(false);
}

void Session::set_clipboard_handlers(ClipboardWriteFn on_write, ClipboardReadFn on_read) {
    on_clipboard_write_ = std::move(on_write);
    on_clipboard_read_ = std::move(on_read);
    install_terminal_callbacks();
}

void Session::install_terminal_callbacks() {
    vt::DispatcherCallbacks cbs;
    cbs.on_write_pty = [this](std::string_view resp) {
        write_input(resp);
    };
    cbs.on_bell = [this]() {
        bell_flag_.store(true, std::memory_order_release);
        if (on_wake_) on_wake_();
    };
    cbs.on_clipboard_write = on_clipboard_write_;
    cbs.on_clipboard_read = on_clipboard_read_;
    terminal_.set_callbacks(std::move(cbs));
}

bool Session::start_local(const os::SpawnConfig& cfg, WakeCallback on_wake) {
    stop(false);
    on_wake_ = std::move(on_wake);
    playback_mode_.store(false, std::memory_order_release);
    playback_finished_.store(false, std::memory_order_release);

    const uint16_t cols = static_cast<uint16_t>(terminal_.cols());
    const uint16_t rows = static_cast<uint16_t>(terminal_.rows());
    if (!pty_.open(cols, rows)) return false;
    if (!os::spawn_in_pty(pty_, cfg, child_)) {
        pty_.close();
        return false;
    }

    install_terminal_callbacks();

    alive_.store(true, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    io_thread_ = std::thread([this]() { io_thread_local(); });
    return true;
}

bool Session::start_remote(const std::string& socket_path,
                           uint32_t daemon_session_id,
                           const std::string& cwd,
                           const std::string& term,
                           WakeCallback on_wake) {
    stop(false);
    on_wake_ = std::move(on_wake);

    int fd = os::ipc_connect(socket_path);
    if (fd < 0) return false;

    const uint16_t cols = static_cast<uint16_t>(terminal_.cols());
    const uint16_t rows = static_cast<uint16_t>(terminal_.rows());

    if (daemon_session_id == 0) {
        // CreateSession: uint16 cols, uint16 rows, uint16 cwd_len, cwd, uint16 term_len, term
        std::vector<uint8_t> payload;
        auto push16 = [&](uint16_t v) {
            payload.push_back(static_cast<uint8_t>(v & 0xFF));
            payload.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        };
        push16(cols);
        push16(rows);
        push16(static_cast<uint16_t>(cwd.size()));
        payload.insert(payload.end(), cwd.begin(), cwd.end());
        push16(static_cast<uint16_t>(term.size()));
        payload.insert(payload.end(), term.begin(), term.end());

        if (!os::ipc_send_frame(fd, os::IpcMsgType::CreateSession, payload.data(),
                                static_cast<uint32_t>(payload.size()))) {
            ::close(fd);
            return false;
        }
    } else {
        // Attach: uint32 session_id, uint16 cols, uint16 rows
        uint8_t payload[8];
        std::memcpy(payload + 0, &daemon_session_id, 4);
        std::memcpy(payload + 4, &cols, 2);
        std::memcpy(payload + 6, &rows, 2);
        if (!os::ipc_send_frame(fd, os::IpcMsgType::Attach, payload, sizeof(payload))) {
            ::close(fd);
            return false;
        }
    }

    remote_fd_ = fd;
    daemon_session_id_ = daemon_session_id;
    playback_mode_.store(false, std::memory_order_release);
    playback_finished_.store(false, std::memory_order_release);

    install_terminal_callbacks();

    alive_.store(true, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    io_thread_ = std::thread([this]() { io_thread_remote(); });
    return true;
}

bool Session::start_playback(const std::string& cast_path, WakeCallback on_wake,
                             double max_idle_sec, double speed) {
    stop(false);
    std::ifstream probe(cast_path);
    if (!probe.is_open()) return false;
    std::string header;
    if (!std::getline(probe, header) || header.empty()) return false;

    int w = 0, h = 0;
    if (parse_asciicast_header_dim(header, "width", w) &&
        parse_asciicast_header_dim(header, "height", h)) {
        terminal_.resize(std::clamp(w, 10, 500), std::clamp(h, 4, 200));
    }

    on_wake_ = std::move(on_wake);
    playback_mode_.store(true, std::memory_order_release);
    playback_finished_.store(false, std::memory_order_release);
    install_terminal_callbacks();

    alive_.store(true, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    io_thread_ = std::thread([this, cast_path, max_idle_sec, speed]() {
        io_thread_playback(cast_path, max_idle_sec, speed > 0.0 ? speed : 1.0);
    });
    return true;
}

void Session::stop(bool detach_only) {
    stop_recording();
    const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
    io_loop_.wake();

    if (remote_fd_ >= 0 && detach_only) {
        os::ipc_send_frame(remote_fd_, os::IpcMsgType::Detach, nullptr, 0);
    }

    if (io_thread_.joinable()) {
        io_thread_.join();
    }

    if (remote_fd_ >= 0) {
        ::close(remote_fd_);
        remote_fd_ = -1;
    }

    if (was_running && !detach_only) {
        child_.terminate();
    }
    pty_.close();
    alive_.store(false, std::memory_order_release);
}

void Session::write_input(std::string_view bytes) {
    if (bytes.empty() || !alive_.load(std::memory_order_acquire)) return;
    if (remote_fd_ >= 0) {
        os::ipc_send_frame(remote_fd_, os::IpcMsgType::Input, bytes);
    } else if (pty_.is_open()) {
        pty_.write(bytes);
    }
}

void Session::resize(int cols, int rows, int xpixel, int ypixel) {
    if (cols < 2 || rows < 1) return;
    terminal_.resize(cols, rows);
    const uint16_t c16 = static_cast<uint16_t>(cols);
    const uint16_t r16 = static_cast<uint16_t>(rows);
    if (remote_fd_ >= 0) {
        uint8_t payload[4];
        std::memcpy(payload + 0, &c16, 2);
        std::memcpy(payload + 2, &r16, 2);
        os::ipc_send_frame(remote_fd_, os::IpcMsgType::Resize, payload, sizeof(payload));
    } else if (pty_.is_open()) {
        pty_.resize(c16, r16, static_cast<uint16_t>(xpixel), static_cast<uint16_t>(ypixel));
    }
}

bool Session::start_recording(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(record_mutex_);
    if (record_file_.is_open()) record_file_.close();
    record_file_.open(filepath, std::ios::out | std::ios::trunc);
    if (!record_file_.is_open()) return false;

    record_start_ = std::chrono::steady_clock::now();
    const std::time_t now_ts = std::time(nullptr);
    record_file_ << "{\"version\": 2, \"width\": " << terminal_.cols()
                 << ", \"height\": " << terminal_.rows()
                 << ", \"timestamp\": " << static_cast<long long>(now_ts)
                 << ", \"env\": {\"TERM\": \"xterm-256color\", \"TERM_PROGRAM\": \"vater\"}}\n";
    record_file_.flush();
    recording_.store(true, std::memory_order_release);
    return true;
}

void Session::stop_recording() {
    recording_.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(record_mutex_);
    if (record_file_.is_open()) {
        record_file_.flush();
        record_file_.close();
    }
}

void Session::record_output(const uint8_t* data, size_t len) {
    if (!recording_.load(std::memory_order_acquire) || len == 0) return;
    std::lock_guard<std::mutex> lock(record_mutex_);
    if (!record_file_.is_open()) return;
    const double t = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - record_start_).count();
    char tbuf[32];
    std::snprintf(tbuf, sizeof(tbuf), "%.6f", t);
    record_file_ << "[" << tbuf << ", \"o\", \"" << json_escape_bytes(data, len) << "\"]\n";
}

void Session::io_thread_local() {
    const int pty_fd = pty_.master_fd();
    if (pty_fd < 0) {
        alive_.store(false, std::memory_order_release);
        return;
    }

    uint8_t buf[65536];
    bool eof_seen = false;

    io_loop_.add_fd(pty_fd, EPOLLIN | EPOLLHUP | EPOLLERR, [&](uint32_t /*events*/) {
        // Drain up to 256KB per epoll wake for high-throughput `cat` bursts (§6)
        size_t total_read = 0;
        while (total_read < 262144) {
            ssize_t n = pty_.read(buf, sizeof(buf));
            if (n > 0) {
                terminal_.feed(buf, static_cast<size_t>(n));
                record_output(buf, static_cast<size_t>(n));
                total_read += static_cast<size_t>(n);
            } else if (n == 0) {
                break; // EAGAIN
            } else {
                eof_seen = true;
                break;
            }
        }
        if (total_read > 0 && on_wake_) {
            on_wake_();
        }
    });

    while (running_.load(std::memory_order_acquire) && !eof_seen) {
        io_loop_.poll_once(100);
        if (child_.poll_exit()) {
            // Drain any remaining buffered PTY output after child exit
            for (;;) {
                ssize_t n = pty_.read(buf, sizeof(buf));
                if (n > 0) {
                    terminal_.feed(buf, static_cast<size_t>(n));
                    record_output(buf, static_cast<size_t>(n));
                } else {
                    break;
                }
            }
            exit_code_.store(child_.exit_status, std::memory_order_release);
            break;
        }
    }

    io_loop_.remove_fd(pty_fd);
    alive_.store(false, std::memory_order_release);
    if (on_wake_) on_wake_();
}

void Session::io_thread_remote() {
    if (remote_fd_ < 0) {
        alive_.store(false, std::memory_order_release);
        return;
    }

    os::IpcFrameReader reader;
    bool closed = false;

    io_loop_.add_fd(remote_fd_, EPOLLIN | EPOLLHUP | EPOLLERR, [&](uint32_t /*events*/) {
        int r = reader.read_from_fd(remote_fd_);
        if (r < 0) {
            closed = true;
            return;
        }
        os::IpcMsgType type{};
        std::vector<uint8_t> payload;
        bool woke = false;
        while (reader.pop_frame(type, payload)) {
            if (type == os::IpcMsgType::AttachOk) {
                if (payload.size() >= 4) {
                    std::memcpy(&daemon_session_id_, payload.data(), 4);
                    if (payload.size() > 4) {
                        terminal_.feed(payload.data() + 4, payload.size() - 4);
                        woke = true;
                    }
                }
            } else if (type == os::IpcMsgType::Output) {
                if (!payload.empty()) {
                    terminal_.feed(payload.data(), payload.size());
                    record_output(payload.data(), payload.size());
                    woke = true;
                }
            } else if (type == os::IpcMsgType::SessionExited) {
                if (payload.size() >= 8) {
                    int32_t code = 0;
                    std::memcpy(&code, payload.data() + 4, 4);
                    exit_code_.store(code, std::memory_order_release);
                }
                closed = true;
            }
        }
        if (woke && on_wake_) on_wake_();
    });

    while (running_.load(std::memory_order_acquire) && !closed) {
        io_loop_.poll_once(100);
    }

    io_loop_.remove_fd(remote_fd_);
    alive_.store(false, std::memory_order_release);
    if (on_wake_) on_wake_();
}

void Session::io_thread_playback(std::string cast_path, double max_idle_sec, double speed) {
    std::ifstream in(cast_path);
    if (!in.is_open()) {
        playback_finished_.store(true, std::memory_order_release);
        if (on_wake_) on_wake_();
        return;
    }

    // Skip header line
    std::string line;
    std::getline(in, line);

    double prev_t = 0.0;
    std::string kind;
    std::string payload;

    while (running_.load(std::memory_order_acquire) && std::getline(in, line)) {
        double ev_t = 0.0;
        if (!parse_asciicast_event_line(line, ev_t, kind, payload)) continue;

        double dt = std::max(0.0, ev_t - prev_t);
        prev_t = ev_t;
        if (max_idle_sec >= 0.0 && dt > max_idle_sec) dt = max_idle_sec;
        dt /= speed;

        if (dt > 0.0005) {
            auto target = std::chrono::steady_clock::now() +
                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(dt));
            while (running_.load(std::memory_order_acquire)) {
                auto now = std::chrono::steady_clock::now();
                if (now >= target) break;
                auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(target - now);
                std::this_thread::sleep_for(std::min(rem, std::chrono::milliseconds(15)));
            }
        }

        if (!running_.load(std::memory_order_acquire)) break;
        if (kind == "o" && !payload.empty()) {
            terminal_.feed(payload);
            if (on_wake_) on_wake_();
        }
    }

    if (running_.load(std::memory_order_acquire)) {
        terminal_.feed("\r\n\x1b[2m[vater: asciicast playback complete]\x1b[0m\r\n");
    }
    playback_finished_.store(true, std::memory_order_release);
    if (on_wake_) on_wake_();
}

} // namespace vater::mux

