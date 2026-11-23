#pragma once
/// @file session.hpp
/// @brief L2 Session & Mux — Terminal Session (Local PTY + IO Thread OR Remote vaterd Attach)
/// with coalesced UI wakeup and Asciicast v2 recording.

#include "os/event_loop.hpp"
#include "os/ipc.hpp"
#include "os/pty.hpp"
#include "os/spawn.hpp"
#include "vt/terminal.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace vater::mux {

/// Represents a single interactive terminal session backed by either a local PTY
/// or a persistent `vaterd` daemon session over Unix socket IPC.
class Session {
public:
    using WakeCallback = std::function<void()>;

    Session(uint32_t id, int cols, int rows, size_t max_scrollback = vt::Scrollback::kDefaultMaxLines);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /// Starts a local PTY + child process and spawns the epoll I/O thread.
    bool start_local(const os::SpawnConfig& cfg, WakeCallback on_wake = {});

    /// Attaches to an existing or newly created session hosted by `vaterd`.
    bool start_remote(const std::string& socket_path,
                      uint32_t daemon_session_id,
                      const std::string& cwd,
                      const std::string& term,
                      WakeCallback on_wake = {});

    /// Plays back an Asciicast v2 `.cast` recording directly into the terminal.
    bool start_playback(const std::string& cast_path,
                        WakeCallback on_wake = {},
                        double max_idle_sec = 2.0,
                        double speed = 1.0);

    /// Sets OSC 52 clipboard callbacks that persist across start_local / start_remote.
    using ClipboardWriteFn = std::function<void(std::string_view selection, std::string_view text)>;
    using ClipboardReadFn = std::function<std::string(std::string_view selection)>;
    void set_clipboard_handlers(ClipboardWriteFn on_write, ClipboardReadFn on_read);

    /// Detaches cleanly from `vaterd` without killing the remote shell, or stops local PTY.
    void stop(bool detach_only = false);

    /// Writes user input bytes (keyboard/mouse/paste) to the PTY or vaterd socket.
    void write_input(std::string_view bytes);

    /// Resizes terminal grid (with reflow) and notifies PTY (TIOCSWINSZ) / vaterd.
    void resize(int cols, int rows, int xpixel = 0, int ypixel = 0);

    /// Asciicast v2 recording (`.cast` JSON-lines format).
    bool start_recording(const std::string& filepath);
    void stop_recording();
    [[nodiscard]] bool is_recording() const noexcept { return recording_.load(std::memory_order_relaxed); }

    [[nodiscard]] uint32_t id() const noexcept { return id_; }
    [[nodiscard]] uint32_t daemon_session_id() const noexcept { return daemon_session_id_; }
    [[nodiscard]] bool is_remote() const noexcept { return remote_fd_ >= 0; }
    [[nodiscard]] bool is_playback() const noexcept { return playback_mode_.load(std::memory_order_acquire); }
    [[nodiscard]] bool is_playback_finished() const noexcept { return playback_finished_.load(std::memory_order_acquire); }
    [[nodiscard]] bool is_alive() const noexcept { return alive_.load(std::memory_order_acquire); }
    [[nodiscard]] int exit_code() const noexcept { return exit_code_.load(std::memory_order_acquire); }

    [[nodiscard]] vt::Terminal& terminal() noexcept { return terminal_; }
    [[nodiscard]] const vt::Terminal& terminal() const noexcept { return terminal_; }

    /// Pending visual bell indicator timestamp (decays in UI).
    [[nodiscard]] bool consume_bell() noexcept { return bell_flag_.exchange(false, std::memory_order_acq_rel); }

private:
    void install_terminal_callbacks();
    void io_thread_local();
    void io_thread_remote();
    void io_thread_playback(std::string cast_path, double max_idle_sec, double speed);
    void record_output(const uint8_t* data, size_t len);

    uint32_t id_{0};
    uint32_t daemon_session_id_{0};
    vt::Terminal terminal_;

    os::Pty pty_;
    os::ChildProcess child_{};
    int remote_fd_{-1};

    os::EventLoop io_loop_;
    std::thread io_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> alive_{false};
    std::atomic<bool> playback_mode_{false};
    std::atomic<bool> playback_finished_{false};
    std::atomic<int> exit_code_{0};
    std::atomic<bool> bell_flag_{false};
    WakeCallback on_wake_;
    ClipboardWriteFn on_clipboard_write_;
    ClipboardReadFn on_clipboard_read_;

    // Asciicast v2 recorder
    std::atomic<bool> recording_{false};
    std::mutex record_mutex_;
    std::ofstream record_file_;
    std::chrono::steady_clock::time_point record_start_{};
};

} // namespace vater::mux

