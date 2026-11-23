#pragma once
/// @file input_encoder.hpp
/// @brief L5 UI — Input Encoder: Legacy xterm + modifyOtherKeys + Kitty Keyboard Protocol
/// + Mouse X10/1000/1002/1003/SGR(1006)/SGR-Pixels(1016) + Focus Events + Bracketed Paste.

#include "vt/modes.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace vater::ui {

enum class KeyEventKind : uint8_t {
    Press   = 1,
    Repeat  = 2,
    Release = 3,
};

enum class MouseEventKind : uint8_t {
    Press,
    Release,
    Move,
    WheelUp,
    WheelDown,
};

/// Translates `nisaba::backend_os::Platform` keyboard/mouse/paste events into VT escape sequences.
class InputEncoder {
public:
    /// Encodes a key press/release event given XKB keysym, KeyMod bitmask (1=Shift,2=Ctrl,4=Alt,8=Super),
    /// and current `TerminalModes`. Sets `out_suppress_text = true` if the paired `onTextInput`
    /// signal from backend_os should be ignored to prevent duplicate character transmission.
    [[nodiscard]] static std::string encode_key(int xkb_keysym,
                                                int mods,
                                                KeyEventKind kind,
                                                const vt::TerminalModes& modes,
                                                bool& out_suppress_text);

    /// Encodes a UTF-8 text input string (from `Platform::onTextInput` / IME composition).
    [[nodiscard]] static std::string encode_text(std::string_view utf8_text,
                                                 const vt::TerminalModes& modes);

    /// Encodes a mouse event according to active `mouse_mode` and `mouse_encoding` (X10/SGR/SGR-Pixels).
    /// `cell_col`/`cell_row` are 0-indexed cell coordinates; `px_x`/`px_y` are pane-local pixels.
    [[nodiscard]] static std::string encode_mouse(MouseEventKind kind,
                                                  int button, // 1=left, 2=middle, 3=right, 0=none
                                                  int mods,
                                                  int cell_col, int cell_row,
                                                  int px_x, int px_y,
                                                  const vt::TerminalModes& modes);

    /// Encodes window focus gain/loss (DEC 1004).
    [[nodiscard]] static std::string encode_focus(bool focused, const vt::TerminalModes& modes);

    /// Wraps pasted text in bracketed paste markers (DEC 2004) when active, stripping embedded
    /// `\x1b[201~` end markers to prevent bracketed-paste injection!
    [[nodiscard]] static std::string encode_paste(std::string_view text, const vt::TerminalModes& modes);

    /// Returns true if pasted text contains newlines or dangerous shell control bytes requiring confirmation.
    [[nodiscard]] static bool is_multiline_or_dangerous_paste(std::string_view text) noexcept;

    /// Shell-quotes a dropped file path (from `Platform::onDrop`) for safe insertion into the shell.
    [[nodiscard]] static std::string shell_quote_path(std::string_view path);
};

} // namespace vater::ui

