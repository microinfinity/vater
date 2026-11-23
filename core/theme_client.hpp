#pragma once
/// @file theme_client.hpp
/// @brief Sovereign Header-Only Theme Client SDK for Obsidian Ecosystem Applications.
/// Installed at: /usr/include/obsidian/theme_client.hpp
/// Zero external dependencies (C++20 stdlib + POSIX AF_UNIX socket).
///
/// Design philosophy:
///   - Each ecosystem app owns its own bespoke Dark Mode & Light Mode palettes.
///   - In Live Mode ("live"), the app dynamically derives its palette from `ev.live_seed`
///     streamed live over $XDG_RUNTIME_DIR/obsidian-shell.sock (EVT theme.seed=#RRGGBB).
///   - `ev.apps_opacity` provides the live user-configured application glass opacity (0.0..1.0).

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace obsidian {

struct ThemeEvent {
    std::string mode{"dark"};       ///< "dark", "light", or "live"
    std::string live_seed{"#50E3C2"}; ///< Live wallpaper Monet seed (#RRGGBB), used when mode == "live"
    float apps_opacity{0.85f};      ///< Dedicated Apps Opacity slider value (0.0..1.0)

    [[nodiscard]] bool is_dark()  const noexcept { return mode == "dark"; }
    [[nodiscard]] bool is_light() const noexcept { return mode == "light"; }
    [[nodiscard]] bool is_live()  const noexcept { return mode == "live"; }

    /// Parse "#RRGGBB" (or "#RRGGBBAA") into 8-bit RGB channels
    static bool parse_hex_rgb(const std::string& hex, uint8_t& r, uint8_t& g, uint8_t& b) noexcept {
        std::string s = hex;
        if (!s.empty() && s[0] == '#') s.erase(0, 1);
        if (s.size() < 6) return false;
        try {
            r = static_cast<uint8_t>(std::stoul(s.substr(0, 2), nullptr, 16));
            g = static_cast<uint8_t>(std::stoul(s.substr(2, 2), nullptr, 16));
            b = static_cast<uint8_t>(std::stoul(s.substr(4, 2), nullptr, 16));
            return true;
        } catch (...) {
            return false;
        }
    }
};

class ThemeClient {
public:
    using Callback = std::function<void(const ThemeEvent&)>;

    ThemeClient() = default;
    ~ThemeClient() { stop(); }

    ThemeClient(const ThemeClient&) = delete;
    ThemeClient& operator=(const ThemeClient&) = delete;

    [[nodiscard]] static std::string default_socket_path() {
        const char* rt = std::getenv("XDG_RUNTIME_DIR");
        if (rt && rt[0] != '\0') {
            return std::string(rt) + "/obsidian-shell.sock";
        }
        return "/tmp/obsidian-shell-" + std::to_string(::getuid()) + ".sock";
    }

    /// Fetch initial theme state (socket GET -> disk fallback), invoke callback immediately
    /// with initial state, and start background auto-reconnecting SUB event listener.
    void start(Callback on_changed) {
        if (running_.exchange(true)) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            on_changed_ = std::move(on_changed);
        }

        if (!fetch_initial_via_socket()) {
            load_from_disk_fallback();
        }

        // Deliver initial snapshot immediately
        {
            ThemeEvent init_ev;
            Callback cb;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                init_ev = state_;
                cb = on_changed_;
            }
            if (cb) cb(init_ev);
        }

        if (::pipe(stop_pipe_) != 0) {
            stop_pipe_[0] = stop_pipe_[1] = -1;
        }
        worker_ = std::thread([this]() { worker_loop(); });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (stop_pipe_[1] >= 0) {
            char b = 1;
            ssize_t _ = ::write(stop_pipe_[1], &b, 1);
            (void)_;
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        if (stop_pipe_[0] >= 0) ::close(stop_pipe_[0]);
        if (stop_pipe_[1] >= 0) ::close(stop_pipe_[1]);
        stop_pipe_[0] = stop_pipe_[1] = -1;
    }

    [[nodiscard]] ThemeEvent snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

private:
    static std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    static std::string normalize_mode(const std::string& raw) {
        if (raw == "light" || raw == "retro") return "light";
        if (raw == "live" || raw == "dynamic" || raw == "material") return "live";
        return "dark";
    }

    static int connect_socket(const std::string& path) {
        int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return -1;
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path.size() >= sizeof(addr.sun_path)) {
            ::close(fd);
            return -1;
        }
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd);
            return -1;
        }
        return fd;
    }

    static bool send_all(int fd, const std::string& data) {
        size_t off = 0;
        while (off < data.size()) {
            ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
            if (n > 0) off += static_cast<size_t>(n);
            else if (n < 0 && errno == EINTR) continue;
            else return false;
        }
        return true;
    }

    static std::string xdg_config_base() {
        const char* xdg = std::getenv("XDG_CONFIG_HOME");
        if (xdg && xdg[0] != '\0') return std::string(xdg);
        const char* home = std::getenv("HOME");
        if (home && home[0] != '\0') return std::string(home) + "/.config";
        return "/tmp";
    }

    bool apply_kv(const std::string& key, const std::string& val, bool emit_cb) {
        ThemeEvent ev;
        Callback cb;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (key == "appearance.theme") {
                std::string m = normalize_mode(val);
                if (state_.mode != m) {
                    state_.mode = std::move(m);
                    changed = true;
                }
            } else if (key == "theme.seed") {
                if (!val.empty() && state_.live_seed != val) {
                    state_.live_seed = val;
                    changed = true;
                }
            } else if (key == "appearance.apps_opacity") {
                try {
                    float op = std::clamp(std::stof(val), 0.0f, 1.0f);
                    if (std::abs(state_.apps_opacity - op) > 1e-3f) {
                        state_.apps_opacity = op;
                        changed = true;
                    }
                } catch (...) {}
            }
            if (changed && emit_cb) {
                ev = state_;
                cb = on_changed_;
            }
        }
        if (cb) cb(ev);
        return changed;
    }

    bool fetch_initial_via_socket() {
        int fd = connect_socket(default_socket_path());
        if (fd < 0) return false;
        if (!send_all(fd, "GET\n")) {
            ::close(fd);
            return false;
        }
        std::string buf;
        char tmp[4096];
        bool got_end = false;
        while (!got_end) {
            struct pollfd pfd{fd, POLLIN, 0};
            if (::poll(&pfd, 1, 500) <= 0) break;
            ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
            if (n <= 0) break;
            buf.append(tmp, static_cast<size_t>(n));
            if (buf.find("\nEND\n") != std::string::npos || buf == "END\n") {
                got_end = true;
            }
        }
        ::close(fd);
        if (!got_end) return false;

        std::istringstream iss(buf);
        std::string line;
        bool saw_seed = false;
        while (std::getline(iss, line)) {
            line = trim(line);
            if (line == "END") break;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);
            if (k == "theme.seed" && !v.empty()) saw_seed = true;
            apply_kv(k, v, false);
        }
        if (!saw_seed) {
            load_seed_from_theme_state();
        }
        return true;
    }

    void load_seed_from_theme_state() {
        const std::string base = xdg_config_base() + "/obsidian/";
        std::ifstream in(base + "theme_state.conf");
        std::string line, section;
        while (std::getline(in, line)) {
            std::string s = trim(line);
            if (s.empty() || s[0] == '#' || s[0] == ';') continue;
            if (s.front() == '[' && s.back() == ']') {
                section = trim(s.substr(1, s.size() - 2));
                continue;
            }
            size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            std::string k = trim(s.substr(0, eq));
            std::string v = trim(s.substr(eq + 1));
            if (section == "theme" && k == "seed" && !v.empty()) {
                apply_kv("theme.seed", v, false);
            }
        }
    }

    void load_from_disk_fallback() {
        const std::string base = xdg_config_base() + "/obsidian/";
        {
            std::ifstream in(base + "shell.conf");
            std::string line, section;
            while (std::getline(in, line)) {
                std::string s = trim(line);
                if (s.empty() || s[0] == '#' || s[0] == ';') continue;
                if (s.front() == '[' && s.back() == ']') {
                    section = trim(s.substr(1, s.size() - 2));
                    continue;
                }
                size_t eq = s.find('=');
                if (eq == std::string::npos) continue;
                std::string k = trim(s.substr(0, eq));
                std::string v = trim(s.substr(eq + 1));
                if (section == "appearance") {
                    if (k == "theme") apply_kv("appearance.theme", v, false);
                    else if (k == "apps_opacity") apply_kv("appearance.apps_opacity", v, false);
                }
            }
        }
        {
            std::ifstream in(base + "theme_state.conf");
            std::string line, section;
            while (std::getline(in, line)) {
                std::string s = trim(line);
                if (s.empty() || s[0] == '#' || s[0] == ';') continue;
                if (s.front() == '[' && s.back() == ']') {
                    section = trim(s.substr(1, s.size() - 2));
                    continue;
                }
                size_t eq = s.find('=');
                if (eq == std::string::npos) continue;
                std::string k = trim(s.substr(0, eq));
                std::string v = trim(s.substr(eq + 1));
                if (section == "theme" && k == "seed" && !v.empty()) {
                    apply_kv("theme.seed", v, false);
                }
            }
        }
    }

    void worker_loop() {
        while (running_.load(std::memory_order_acquire)) {
            int fd = connect_socket(default_socket_path());
            if (fd < 0) {
                struct pollfd pfd{stop_pipe_[0], POLLIN, 0};
                if (stop_pipe_[0] >= 0 && ::poll(&pfd, 1, 1000) > 0) break;
                continue;
            }

            // Refresh on (re)connect
            ThemeEvent before = snapshot();
            if (fetch_initial_via_socket()) {
                ThemeEvent after = snapshot();
                if (before.mode != after.mode ||
                    before.live_seed != after.live_seed ||
                    std::abs(before.apps_opacity - after.apps_opacity) > 1e-3f) {
                    Callback cb;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        cb = on_changed_;
                    }
                    if (cb) cb(after);
                }
            }

            if (!send_all(fd, "SUB\n")) {
                ::close(fd);
                continue;
            }

            std::string buf;
            char tmp[1024];
            while (running_.load(std::memory_order_acquire)) {
                struct pollfd pfds[2];
                pfds[0] = {fd, POLLIN, 0};
                pfds[1] = {stop_pipe_[0], POLLIN, 0};
                int nfds = (stop_pipe_[0] >= 0) ? 2 : 1;

                int ret = ::poll(pfds, nfds, 1000);
                if (ret < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (ret == 0) continue;
                if (nfds == 2 && (pfds[1].revents & POLLIN)) {
                    ::close(fd);
                    return;
                }
                if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) break;
                if (pfds[0].revents & POLLIN) {
                    ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
                    if (n <= 0) break;
                    buf.append(tmp, static_cast<size_t>(n));
                    size_t nl = 0;
                    while ((nl = buf.find('\n')) != std::string::npos) {
                        std::string line = trim(buf.substr(0, nl));
                        buf.erase(0, nl + 1);
                        if (line.rfind("EVT ", 0) == 0) {
                            std::string kv = line.substr(4);
                            size_t eq = kv.find('=');
                            if (eq != std::string::npos) {
                                apply_kv(kv.substr(0, eq), kv.substr(eq + 1), true);
                            }
                        }
                    }
                }
            }
            ::close(fd);
        }
    }

    mutable std::mutex mutex_;
    ThemeEvent state_{};
    Callback on_changed_{nullptr};

    std::atomic<bool> running_{false};
    int stop_pipe_[2]{-1, -1};
    std::thread worker_;
};

} // namespace obsidian

