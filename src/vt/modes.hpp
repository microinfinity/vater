#pragma once
/// @file modes.hpp
/// @brief L3 Terminal Core — Complete DECSET/DECRST & ANSI SM/RM modes, cursor style, mouse & Kitty keyboard flags.
/// Strictly Nisaba-agnostic (term_core).

#include <array>
#include <cstddef>
#include <cstdint>

namespace vater::vt {

enum class CursorShape : uint8_t {
    Block     = 0,
    Underline = 1,
    Bar       = 2,
};

enum class MouseTrackingMode : uint8_t {
    None       = 0,
    X10        = 1,  // ?9  press only
    Normal     = 2,  // ?1000 press + release
    Button     = 3,  // ?1002 drag + press + release
    AnyEvent   = 4,  // ?1003 all motion
};

enum class MouseEncoding : uint8_t {
    Default   = 0,  // Legacy X10/UTF-8
    Utf8      = 1,  // ?1005
    Sgr       = 2,  // ?1006
    SgrPixels = 3,  // ?1016
};

/// Kitty keyboard protocol progressive enhancement flags (CSI > flags u).
namespace kitty_kbd {
inline constexpr uint8_t kDisambiguate    = 1u << 0; // 1
inline constexpr uint8_t kReportEvents    = 1u << 1; // 2
inline constexpr uint8_t kReportAlternate = 1u << 2; // 4
inline constexpr uint8_t kReportAllKeys   = 1u << 3; // 8
inline constexpr uint8_t kReportText      = 1u << 4; // 16
} // namespace kitty_kbd

/// All terminal behavioral modes (ANSI + DEC private).
struct TerminalModes {
    // ANSI Standard Modes (SM / RM)
    bool keyboard_action{false};  // 2  (KAM)
    bool insert_mode{false};      // 4  (IRM)
    bool send_receive{true};      // 12 (SRM: true = local echo off)
    bool line_feed_new_line{false}; // 20 (LNM: LF implies CR)

    // DEC Private Modes (DECSET ? / DECRST ?)
    bool cursor_keys_app{false};     // ?1  DECCKM
    bool ansi_mode{true};            // ?2  DECANM
    bool col_132{false};             // ?3  DECCOLM
    bool smooth_scroll{false};       // ?4  DECSCLM
    bool reverse_video{false};       // ?5  DECSCNM
    bool origin_mode{false};         // ?6  DECOM
    bool auto_wrap{true};            // ?7  DECAWM
    bool auto_repeat{true};          // ?8  DECARM
    bool cursor_blink{true};         // ?12 att610 / DECSCUSR
    bool cursor_visible{true};       // ?25 DECTCEM
    bool reverse_wraparound{false};  // ?45 xterm
    bool allow_left_right_margin{false}; // ?69 DECLRMM
    bool app_keypad{false};          // DECKPAM / DECKPNM (ESC = / ESC >)
    bool alt_screen{false};          // ?47 / ?1047 / ?1049
    bool focus_events{false};        // ?1004
    bool alt_scroll{false};          // ?1007
    bool bracketed_paste{false};     // ?2004
    bool synchronized_output{false}; // ?2026 (DEC 2026)
    bool grapheme_clustering{true};  // ?2027
    bool bidi_implicit{true};        // Implicit per-line visual BiDi (default enabled per spec)
    bool modify_other_keys{false};   // CSI > 4 ; m (level 1/2)
    uint8_t modify_other_keys_level{0};

    // Mouse tracking
    MouseTrackingMode mouse_mode{MouseTrackingMode::None};
    MouseEncoding mouse_encoding{MouseEncoding::Default};

    // Cursor shape (DECSCUSR)
    CursorShape cursor_shape{CursorShape::Block};

    // Kitty keyboard protocol stacks (separate for main & alt screen, depth 16)
    std::array<uint8_t, 16> kitty_stack_main{};
    uint8_t kitty_depth_main{0};
    std::array<uint8_t, 16> kitty_stack_alt{};
    uint8_t kitty_depth_alt{0};

    [[nodiscard]] uint8_t kitty_flags() const noexcept {
        if (alt_screen) {
            return kitty_depth_alt > 0 ? kitty_stack_alt[kitty_depth_alt - 1] : 0;
        }
        return kitty_depth_main > 0 ? kitty_stack_main[kitty_depth_main - 1] : 0;
    }

    void kitty_push(uint8_t flags) noexcept {
        auto& stack = alt_screen ? kitty_stack_alt : kitty_stack_main;
        auto& depth = alt_screen ? kitty_depth_alt : kitty_depth_main;
        if (depth < stack.size()) {
            stack[depth++] = flags & 0x1Fu;
        } else {
            // Drop oldest entry when stack overflows
            for (size_t i = 1; i < stack.size(); ++i) stack[i - 1] = stack[i];
            stack.back() = flags & 0x1Fu;
        }
    }

    void kitty_pop(uint32_t count) noexcept {
        auto& depth = alt_screen ? kitty_depth_alt : kitty_depth_main;
        if (count == 0) count = 1;
        if (count >= depth) {
            depth = 0;
        } else {
            depth = static_cast<uint8_t>(depth - count);
        }
    }

    void kitty_set(uint8_t flags, uint8_t mode) noexcept {
        auto& stack = alt_screen ? kitty_stack_alt : kitty_stack_main;
        auto& depth = alt_screen ? kitty_depth_alt : kitty_depth_main;
        if (depth == 0) depth = 1;
        uint8_t& cur = stack[depth - 1];
        const uint8_t masked = flags & 0x1Fu;
        switch (mode) {
            case 1: cur = masked; break;           // set
            case 2: cur |= masked; break;          // OR
            case 3: cur &= static_cast<uint8_t>(~masked); break; // AND NOT
            default: cur = masked; break;
        }
    }

    void reset() noexcept {
        *this = TerminalModes{};
    }
};

} // namespace vater::vt
