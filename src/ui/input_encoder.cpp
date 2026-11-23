/// @file input_encoder.cpp
/// @brief L5 UI — Input Encoder implementation (xterm + Kitty Keyboard + SGR/Pixel Mouse + Bracketed Paste).

#include "ui/input_encoder.hpp"

#include <algorithm>
#include <cstdio>

namespace vater::ui {

namespace {

inline constexpr int kModShift = 1 << 0;
inline constexpr int kModCtrl  = 1 << 1;
inline constexpr int kModAlt   = 1 << 2;
inline constexpr int kModSuper = 1 << 3;

// XKB keysym constants (zero xkbcommon header dependency!)
inline constexpr int kKeyBackSpace  = 0xFF08;
inline constexpr int kKeyTab        = 0xFF09;
inline constexpr int kKeyReturn     = 0xFF0D;
inline constexpr int kKeyEscape     = 0xFF1B;
inline constexpr int kKeyHome       = 0xFF50;
inline constexpr int kKeyLeft       = 0xFF51;
inline constexpr int kKeyUp         = 0xFF52;
inline constexpr int kKeyRight      = 0xFF53;
inline constexpr int kKeyDown       = 0xFF54;
inline constexpr int kKeyPageUp     = 0xFF55;
inline constexpr int kKeyPageDown   = 0xFF56;
inline constexpr int kKeyEnd        = 0xFF57;
inline constexpr int kKeyInsert     = 0xFF63;
inline constexpr int kKeyDelete     = 0xFFFF;
inline constexpr int kKeyIsoLeftTab = 0xFE20;
inline constexpr int kKeyF1         = 0xFFBE;
inline constexpr int kKeyF12        = 0xFFC9;
inline constexpr int kKeyKpEnter    = 0xFF8D;

bool is_modifier_keysym(int sym) noexcept {
    return sym >= 0xFFE1 && sym <= 0xFFEE;
}

int xterm_mod_param(int mods) noexcept {
    // xterm / Kitty modifier encoding: 1 + (Shift=1, Alt=2, Ctrl=4, Super=8)
    int bits = 0;
    if (mods & kModShift) bits |= 1;
    if (mods & kModAlt)   bits |= 2;
    if (mods & kModCtrl)  bits |= 4;
    if (mods & kModSuper) bits |= 8;
    return 1 + bits;
}

uint32_t xkb_to_kitty_codepoint(int sym) noexcept {
    if (sym >= 0x20 && sym <= 0x7E) {
        if (sym >= 'A' && sym <= 'Z') return static_cast<uint32_t>(sym + ('a' - 'A'));
        return static_cast<uint32_t>(sym);
    }
    switch (sym) {
        case kKeyEscape:    return 27;
        case kKeyReturn:    return 13;
        case kKeyTab:       return 9;
        case kKeyBackSpace: return 127;
        case kKeyKpEnter:   return 57414;
        default:            return 0;
    }
}

std::string format_csi_mod(char final_ch, int mods, int num_prefix = 0) {
    char buf[32];
    const int mp = xterm_mod_param(mods);
    if (num_prefix > 0) {
        if (mp > 1) std::snprintf(buf, sizeof(buf), "\x1b[%d;%d%c", num_prefix, mp, final_ch);
        else        std::snprintf(buf, sizeof(buf), "\x1b[%d%c", num_prefix, final_ch);
    } else {
        if (mp > 1) std::snprintf(buf, sizeof(buf), "\x1b[1;%d%c", mp, final_ch);
        else        std::snprintf(buf, sizeof(buf), "\x1b[%c", final_ch);
    }
    return buf;
}

} // namespace

std::string InputEncoder::encode_key(int sym,
                                     int mods,
                                     KeyEventKind kind,
                                     const vt::TerminalModes& modes,
                                     bool& out_suppress_text) {
    out_suppress_text = false;
    if (is_modifier_keysym(sym)) return {};

    const uint8_t kflags = modes.kitty_flags();
    const bool report_events = (kflags & vt::kitty_kbd::kReportEvents) != 0;

    if (kind == KeyEventKind::Release && !report_events) {
        return {};
    }

    // ── Kitty Keyboard Protocol (CSI u progressive enhancement) ──
    if (kflags != 0) {
        const bool disambiguate = (kflags & vt::kitty_kbd::kDisambiguate) != 0;
        const bool report_all = (kflags & vt::kitty_kbd::kReportAllKeys) != 0;
        const uint32_t cp = xkb_to_kitty_codepoint(sym);
        const bool has_ctrl_or_alt = (mods & (kModCtrl | kModAlt | kModSuper)) != 0;
        const bool is_special_u = (sym == kKeyEscape || sym == kKeyReturn || sym == kKeyTab || sym == kKeyBackSpace);

        if (cp != 0 && (report_all || (disambiguate && (has_ctrl_or_alt || sym == kKeyEscape || (is_special_u && mods != 0))) || kind == KeyEventKind::Release)) {
            const int mp = xterm_mod_param(mods);
            char buf[64];
            if (report_events && kind != KeyEventKind::Press) {
                std::snprintf(buf, sizeof(buf), "\x1b[%u;%d:%du",
                              cp, mp, static_cast<int>(kind));
            } else if (mp > 1) {
                std::snprintf(buf, sizeof(buf), "\x1b[%u;%du", cp, mp);
            } else {
                std::snprintf(buf, sizeof(buf), "\x1b[%uu", cp);
            }
            out_suppress_text = true;
            return buf;
        }
    }

    if (kind == KeyEventKind::Release) return {};

    // ── Functional / Navigation Keys ──
    const int mp = xterm_mod_param(mods);
    auto arrow = [&](char ch) -> std::string {
        out_suppress_text = true;
        if (mp > 1) return format_csi_mod(ch, mods);
        return modes.cursor_keys_app ? (std::string("\x1bO") + ch) : (std::string("\x1b[") + ch);
    };

    switch (sym) {
        case kKeyUp:       return arrow('A');
        case kKeyDown:     return arrow('B');
        case kKeyRight:    return arrow('C');
        case kKeyLeft:     return arrow('D');
        case kKeyHome:     return arrow('H');
        case kKeyEnd:      return arrow('F');
        case kKeyInsert:   out_suppress_text = true; return format_csi_mod('~', mods, 2);
        case kKeyDelete:   out_suppress_text = true; return format_csi_mod('~', mods, 3);
        case kKeyPageUp:   out_suppress_text = true; return format_csi_mod('~', mods, 5);
        case kKeyPageDown: out_suppress_text = true; return format_csi_mod('~', mods, 6);
        case kKeyF1:       out_suppress_text = true; return (mp > 1) ? format_csi_mod('P', mods) : "\x1bOP";
        case kKeyF1 + 1:   out_suppress_text = true; return (mp > 1) ? format_csi_mod('Q', mods) : "\x1bOQ";
        case kKeyF1 + 2:   out_suppress_text = true; return (mp > 1) ? format_csi_mod('R', mods) : "\x1bOR";
        case kKeyF1 + 3:   out_suppress_text = true; return (mp > 1) ? format_csi_mod('S', mods) : "\x1bOS";
        case kKeyF1 + 4:   out_suppress_text = true; return format_csi_mod('~', mods, 15);
        case kKeyF1 + 5:   out_suppress_text = true; return format_csi_mod('~', mods, 17);
        case kKeyF1 + 6:   out_suppress_text = true; return format_csi_mod('~', mods, 18);
        case kKeyF1 + 7:   out_suppress_text = true; return format_csi_mod('~', mods, 19);
        case kKeyF1 + 8:   out_suppress_text = true; return format_csi_mod('~', mods, 20);
        case kKeyF1 + 9:   out_suppress_text = true; return format_csi_mod('~', mods, 21);
        case kKeyF1 + 10:  out_suppress_text = true; return format_csi_mod('~', mods, 23);
        case kKeyF12:      out_suppress_text = true; return format_csi_mod('~', mods, 24);

        case kKeyReturn:
        case kKeyKpEnter:
            out_suppress_text = true;
            if (modes.modify_other_keys && mp > 1) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "\x1b[27;%d;13~", mp);
                return buf;
            }
            if (mods & kModAlt) return "\x1b\r";
            return modes.line_feed_new_line ? "\r\n" : "\r";

        case kKeyBackSpace:
            out_suppress_text = true;
            if (mods & kModAlt) return "\x1b\x7f";
            if (mods & kModCtrl) return "\x08";
            return "\x7f";

        case kKeyTab:
            out_suppress_text = true;
            if (mods & kModShift) return "\x1b[Z";
            return "\t";

        case kKeyIsoLeftTab:
            out_suppress_text = true;
            return "\x1b[Z";

        case kKeyEscape:
            out_suppress_text = true;
            return (mods & kModAlt) ? "\x1b\x1b" : "\x1b";

        default:
            break;
    }

    // ── Ctrl + key combinations (ASCII C0 control codes) ──
    if (mods & kModCtrl) {
        char ctrl_ch = 0;
        if (sym >= 'a' && sym <= 'z') ctrl_ch = static_cast<char>(sym - 'a' + 1);
        else if (sym >= 'A' && sym <= 'Z') ctrl_ch = static_cast<char>(sym - 'A' + 1);
        else if (sym == ' ' || sym == '@' || sym == '2') ctrl_ch = '\x00';
        else if (sym == '[' || sym == '3') ctrl_ch = '\x1b';
        else if (sym == '\\' || sym == '4') ctrl_ch = '\x1c';
        else if (sym == ']' || sym == '5') ctrl_ch = '\x1d';
        else if (sym == '^' || sym == '6') ctrl_ch = '\x1e';
        else if (sym == '_' || sym == '/' || sym == '7') ctrl_ch = '\x1f';
        else if (sym == '8') ctrl_ch = '\x7f';
        else if (modes.modify_other_keys && sym >= 0x20 && sym <= 0x7E) {
            out_suppress_text = true;
            char buf[32];
            std::snprintf(buf, sizeof(buf), "\x1b[27;%d;%d~", mp, sym);
            return buf;
        } else {
            return {};
        }

        out_suppress_text = true;
        std::string out;
        if (mods & kModAlt) out.push_back('\x1b');
        out.push_back(ctrl_ch);
        return out;
    }

    // ── Alt + printable ASCII (Meta prefix ESC) ──
    if ((mods & kModAlt) && sym >= 0x20 && sym <= 0x7E) {
        out_suppress_text = true;
        std::string out;
        out.push_back('\x1b');
        out.push_back(static_cast<char>(sym));
        return out;
    }

    // Printable text (including Arabic, CJK, accented letters, IME) flows via `onTextInput`!
    return {};
}

std::string InputEncoder::encode_text(std::string_view utf8_text,
                                      const vt::TerminalModes& /*modes*/) {
    if (utf8_text.empty()) return {};
    // Filter raw C0 control chars (already handled cleanly via onKeyDown)
    if (utf8_text.size() == 1 && static_cast<uint8_t>(utf8_text[0]) < 0x20u) {
        return {};
    }
    if (utf8_text.size() == 1 && static_cast<uint8_t>(utf8_text[0]) == 0x7Fu) {
        return {};
    }
    return std::string(utf8_text);
}

std::string InputEncoder::encode_mouse(MouseEventKind kind,
                                       int button,
                                       int mods,
                                       int cell_col, int cell_row,
                                       int px_x, int px_y,
                                       const vt::TerminalModes& modes) {
    if (modes.mouse_mode == vt::MouseTrackingMode::None) return {};

    // Filter events according to active tracking mode
    if (modes.mouse_mode == vt::MouseTrackingMode::X10) {
        if (kind != MouseEventKind::Press) return {};
    } else if (modes.mouse_mode == vt::MouseTrackingMode::Normal) {
        if (kind == MouseEventKind::Move) return {};
    } else if (modes.mouse_mode == vt::MouseTrackingMode::Button) {
        if (kind == MouseEventKind::Move && button == 0) return {};
    }

    int cb = 0;
    if (kind == MouseEventKind::WheelUp) {
        cb = 64;
    } else if (kind == MouseEventKind::WheelDown) {
        cb = 65;
    } else {
        int bcode = 3; // release / none
        if (button == 1) bcode = 0;
        else if (button == 2) bcode = 1;
        else if (button == 3) bcode = 2;
        if (kind == MouseEventKind::Release && modes.mouse_encoding == vt::MouseEncoding::Default) {
            bcode = 3;
        }
        cb = bcode;
        if (kind == MouseEventKind::Move) cb |= 32;
    }

    if (modes.mouse_mode != vt::MouseTrackingMode::X10) {
        if (mods & kModShift) cb |= 4;
        if (mods & kModAlt)   cb |= 8;
        if (mods & kModCtrl)  cb |= 16;
    }

    const int cx = std::max(1, cell_col + 1);
    const int cy = std::max(1, cell_row + 1);

    // SGR (1006) & SGR-Pixels (1016)
    if (modes.mouse_encoding == vt::MouseEncoding::Sgr ||
        modes.mouse_encoding == vt::MouseEncoding::SgrPixels) {
        const int xval = (modes.mouse_encoding == vt::MouseEncoding::SgrPixels) ? std::max(0, px_x) : cx;
        const int yval = (modes.mouse_encoding == vt::MouseEncoding::SgrPixels) ? std::max(0, px_y) : cy;
        const char term = (kind == MouseEventKind::Release) ? 'm' : 'M';
        char buf[64];
        std::snprintf(buf, sizeof(buf), "\x1b[<%d;%d;%d%c", cb, xval, yval, term);
        return buf;
    }

    // Legacy X10 / Normal encoding (clamped to 223)
    const uint8_t b_byte = static_cast<uint8_t>(32 + cb);
    const uint8_t x_byte = static_cast<uint8_t>(32 + std::clamp(cx, 1, 223));
    const uint8_t y_byte = static_cast<uint8_t>(32 + std::clamp(cy, 1, 223));
    std::string out = "\x1b[M";
    out.push_back(static_cast<char>(b_byte));
    out.push_back(static_cast<char>(x_byte));
    out.push_back(static_cast<char>(y_byte));
    return out;
}

std::string InputEncoder::encode_focus(bool focused, const vt::TerminalModes& modes) {
    if (!modes.focus_events) return {};
    return focused ? "\x1b[I" : "\x1b[O";
}

std::string InputEncoder::encode_paste(std::string_view text, const vt::TerminalModes& modes) {
    if (text.empty()) return {};
    // Normalize CRLF/LF -> CR for terminal input
    std::string clean;
    clean.reserve(text.size() + 16);
    for (size_t i = 0; i < text.size(); ++i) {
        // Strip embedded bracketed-paste end sequence `\x1b[201~` for safety!
        if (text[i] == '\x1b' && i + 5 < text.size() &&
            text.substr(i, 6) == "\x1b[201~") {
            i += 5;
            continue;
        }
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
            clean.push_back('\r');
            ++i;
        } else if (text[i] == '\n') {
            clean.push_back('\r');
        } else {
            clean.push_back(text[i]);
        }
    }
    if (modes.bracketed_paste) {
        return "\x1b[200~" + clean + "\x1b[201~";
    }
    return clean;
}

bool InputEncoder::is_multiline_or_dangerous_paste(std::string_view text) noexcept {
    for (char c : text) {
        if (c == '\n' || c == '\r' || c == '\x1b') return true;
    }
    return false;
}

std::string InputEncoder::shell_quote_path(std::string_view path) {
    std::string out;
    out.reserve(path.size() + 4);
    out.push_back('\'');
    for (char c : path) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

} // namespace vater::ui

