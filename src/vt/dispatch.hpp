#pragma once
/// @file dispatch.hpp
/// @brief L3 Terminal Core — Complete VT100/VT220/VT420/xterm/OSC/DCS/APC Dispatcher.
/// Applies commands directly to Grid (zero intermediate command queue allocations).
/// Strictly Nisaba-agnostic (term_core).

#include "vt/graphics/kitty_graphics.hpp"
#include "vt/grid.hpp"
#include "vt/parser.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace vater::vt {

/// Security policy for OSC 52 clipboard access (§8).
struct ClipboardPolicy {
    bool allow_write{false}; // Explicit opt-in per §8 (can be enabled in config)
    bool allow_read{false};  // Disabled by default per §8
};

/// Dynamic palette overrides set via OSC 4 / 10 / 11 / 12.
struct DynamicColorState {
    std::array<std::optional<uint32_t>, 256> palette{}; // 0x00RRGGBB
    std::optional<uint32_t> default_fg;
    std::optional<uint32_t> default_bg;
    std::optional<uint32_t> cursor_color;
};

/// Callbacks emitted upward by the VT dispatcher (title, cwd, bell, clipboard, PTY reply, shell markers).
struct DispatcherCallbacks {
    std::function<void(std::string_view)> on_write_pty;          // Replies to DA/DSR/Kitty/etc.
    std::function<void(std::string_view)> on_title_changed;      // OSC 0 / 2
    std::function<void(std::string_view)> on_cwd_changed;        // OSC 7 / 1337
    std::function<void()> on_bell;                               // BEL (0x07)
    std::function<void(std::string_view selection, std::string_view text)> on_clipboard_write; // OSC 52
    std::function<std::string(std::string_view selection)> on_clipboard_read;                  // OSC 52
    std::function<void(char marker, int exit_code)> on_shell_integration;                      // OSC 133
    std::function<void()> on_colors_changed;                     // OSC 4/10/11/12/104/110/111/112
};

/// Connects Parser events directly to Grid + GraphicsStore + Callbacks.
class Dispatcher final : public VtHandler {
public:
    Dispatcher(Grid& grid, GraphicsStore& graphics);

    void set_callbacks(DispatcherCallbacks cb) { cb_ = std::move(cb); }
    void set_clipboard_policy(ClipboardPolicy pol) noexcept { clipboard_policy_ = pol; }
    [[nodiscard]] const ClipboardPolicy& clipboard_policy() const noexcept { return clipboard_policy_; }

    [[nodiscard]] const std::string& title() const noexcept { return title_; }
    [[nodiscard]] const std::string& cwd() const noexcept { return cwd_; }
    [[nodiscard]] const DynamicColorState& dynamic_colors() const noexcept { return dynamic_colors_; }
    [[nodiscard]] int last_exit_code() const noexcept { return last_exit_code_; }

    /// Optional cell pixel geometry used for CSI 14t / 16t / 18t replies.
    void set_cell_pixel_size(int cell_w, int cell_h) noexcept {
        cell_px_w_ = cell_w > 0 ? cell_w : 8;
        cell_px_h_ = cell_h > 0 ? cell_h : 16;
    }

    // VtHandler overrides
    void on_print(char32_t cp) override;
    void on_print_ascii(const uint8_t* data, size_t len) override;
    void on_execute(uint8_t c0) override;
    void on_esc(uint8_t final_byte, const uint8_t* intermediates, uint8_t intermediate_count) override;
    void on_csi(uint8_t final_byte, const CsiParams& params) override;
    void on_osc(std::string_view payload) override;
    void on_dcs(uint8_t final_byte, const CsiParams& params, std::string_view payload) override;
    void on_apc(std::string_view payload) override;

private:
    void handle_sgr(const CsiParams& params);
    void handle_dec_mode(int32_t mode, bool set);
    void handle_ansi_mode(int32_t mode, bool set);
    int query_dec_mode(int32_t mode) const noexcept;
    int query_ansi_mode(int32_t mode) const noexcept;
    void handle_osc_color(int osc_num, std::string_view spec);
    void reply(std::string_view s);

    static bool parse_rgb_spec(std::string_view spec, uint32_t& out_rgb);
    static std::string format_rgb_reply(uint32_t rgb);

    Grid& grid_;
    GraphicsStore& graphics_;
    DispatcherCallbacks cb_{};
    ClipboardPolicy clipboard_policy_{};
    DynamicColorState dynamic_colors_{};

    std::string title_{"Vater"};
    std::string cwd_{};
    int last_exit_code_{-1};
    int cell_px_w_{9};
    int cell_px_h_{18};
};

} // namespace vater::vt

