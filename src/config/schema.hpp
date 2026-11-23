#pragma once
/// @file schema.hpp
/// @brief Vater Configuration Schema (typed JSON config + keymap + hot-reload via inotify).

#include "vt/modes.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vater::config {

enum class Action : uint16_t {
    None = 0,
    Copy,
    Paste,
    NewTab,
    ClosePane,
    NextTab,
    PrevTab,
    SplitHorizontal,
    SplitVertical,
    FocusLeft,
    FocusRight,
    FocusUp,
    FocusDown,
    ToggleZoomPane,
    FontIncrease,
    FontDecrease,
    FontReset,
    ToggleSearch,
    ToggleCommandPalette,
    ToggleBidi,
    ToggleLigatures,
    ToggleCrtEffect,
    ScrollPageUp,
    ScrollPageDown,
    ScrollToBottom,
    PrevPrompt,
    NextPrompt,
    ReloadConfig,
    ToggleQuake,
    TogglePreviewPanel,
    ToggleRecording,
    ClearScrollback,
};

struct KeyBinding {
    int keysym{0};       // XKB keysym (lowercase for letters, e.g. 'c', 'v', 't', 0xFF55 Page_Up)
    int mods{0};         // nisaba::backend_os::KeyMod bitmask (1=Shift, 2=Ctrl, 4=Alt, 8=Super)
    Action action{Action::None};
};

struct VaterConfig {
    // Font & Typography
    std::string font_family{"DejaVu Sans Mono"};
    std::vector<std::string> fallback_fonts{"Noto Sans Mono", "Cascadia Mono", "Liberation Mono", "Symbols Nerd Font Mono", "Noto Sans Arabic", "Noto Color Emoji"};
    float font_size{14.0f};
    float line_height{1.15f};
    bool ligatures{true};
    bool bidi_enabled{true}; // Default ON per user decision #3

    // Shell & Terminal
    std::string shell{};                  // Empty => $SHELL
    std::string term{"xterm-256color"};   // Default per user decision #1 ("vater" optional)
    size_t scrollback_lines{100000};
    bool scrollback_mmap_spill{false};
    std::string word_delimiters{" \t\n\"'`()[]{}<>|;:,!?"};

    // Cursor & Visuals
    vt::CursorShape cursor_shape{vt::CursorShape::Block};
    bool cursor_blink{true};
    uint32_t cursor_blink_ms{530};
    float opacity{0.88f};                 // Overridden by live ThemeClient apps_opacity when follow_system_theme=true
    bool follow_system_theme{true};
    bool force_dark{false};
    bool force_light{false};
    bool blur_behind{true};
    bool crt_effect{false};               // Optional RuntimeEffect CRT/scanline shader
    bool software_renderer{false};        // Force CPU fallback renderer
    float padding_x{10.0f};
    float padding_y{8.0f};

    // Security (§8)
    bool osc52_write{false};              // Explicit opt-in per §8
    bool osc52_read{false};               // Disabled by default per §8
    bool confirm_multiline_paste{true};

    // Optional 16-color palette override (0x00RRGGBB)
    std::array<std::optional<uint32_t>, 16> palette_override{};

    // Keybindings
    std::vector<KeyBinding> keybindings;
};

struct ConfigLoadResult {
    bool ok{true};
    VaterConfig config{};
    std::vector<std::string> diagnostics; // Line-numbered error / warning strings
};

/// Default path: `$XDG_CONFIG_HOME/vater/config.json` or `~/.config/vater/config.json`.
[[nodiscard]] std::string default_config_path();

/// Loads & validates config from `path` using `nisaba::json::JsonValue` with line-numbered diagnostics.
[[nodiscard]] ConfigLoadResult load_config_file(const std::string& path);

/// Parses JSON config text directly (used by loader & unit tests).
[[nodiscard]] ConfigLoadResult parse_config_json(std::string_view json_text);

/// Populates default keybindings (Ctrl+Shift+C/V/T/W/F/P, Ctrl+Shift+H/E splits, +/- zoom, etc.).
[[nodiscard]] std::vector<KeyBinding> default_keybindings();

/// Resolves a key event to a configured Action (or Action::None).
[[nodiscard]] Action lookup_keybinding(const std::vector<KeyBinding>& bindings, int keysym, int mods) noexcept;

/// Parses action name string (e.g. "copy", "split_horizontal") to Action enum.
[[nodiscard]] Action parse_action_name(std::string_view name) noexcept;

/// Linux `inotify` watcher for live config hot-reload (non-blocking).
class ConfigWatcher {
public:
    ConfigWatcher() = default;
    ~ConfigWatcher();

    ConfigWatcher(const ConfigWatcher&) = delete;
    ConfigWatcher& operator=(const ConfigWatcher&) = delete;

    bool watch(const std::string& config_path);
    void stop() noexcept;

    /// Non-blocking check: returns true if config file was modified/replaced since last poll.
    [[nodiscard]] bool poll_changed();
    [[nodiscard]] int fd() const noexcept { return inotify_fd_; }

private:
    int inotify_fd_{-1};
    int watch_wd_{-1};
    std::string filename_;
};

} // namespace vater::config

