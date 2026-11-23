/// @file dispatch.cpp
/// @brief L3 Terminal Core — Complete VT100/VT220/VT420/xterm/OSC/DCS/APC Dispatcher implementation.

#include "vt/dispatch.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vater::vt {

namespace {

int parse_decimal_sv(std::string_view sv, int fallback = -1) noexcept {
    if (sv.empty()) return fallback;
    int v = 0;
    bool any = false;
    for (char c : sv) {
        if (c < '0' || c > '9') break;
        any = true;
        if (v < 1000000) v = v * 10 + (c - '0');
    }
    return any ? v : fallback;
}

uint8_t parse_hex_component(std::string_view h) noexcept {
    if (h.empty()) return 0;
    uint32_t v = 0;
    size_t digits = 0;
    for (char c : h) {
        uint32_t d = 0;
        if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(10 + c - 'a');
        else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(10 + c - 'A');
        else break;
        v = (v << 4) | d;
        ++digits;
        if (digits == 4) break;
    }
    if (digits == 1) return static_cast<uint8_t>(v * 17u);
    if (digits == 2) return static_cast<uint8_t>(v);
    if (digits == 3) return static_cast<uint8_t>(v >> 4);
    return static_cast<uint8_t>(v >> 8);
}

std::string percent_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                return -1;
            };
            int hi = hex(s[i + 1]);
            int lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

} // namespace

Dispatcher::Dispatcher(Grid& grid, GraphicsStore& graphics)
    : grid_(grid), graphics_(graphics) {
    grid_.set_on_scroll_up([this](int rows, bool alt) {
        graphics_.scroll_placements_up(rows, alt);
    });
}

void Dispatcher::reply(std::string_view s) {
    if (cb_.on_write_pty && !s.empty()) {
        cb_.on_write_pty(s);
    }
}

void Dispatcher::on_print(char32_t cp) {
    grid_.put_char(cp);
}

void Dispatcher::on_print_ascii(const uint8_t* data, size_t len) {
    grid_.put_ascii_run(data, len);
}

void Dispatcher::on_execute(uint8_t c0) {
    switch (c0) {
        case 0x05u: // ENQ (disabled for security)
            break;
        case 0x07u: // BEL
            if (cb_.on_bell) cb_.on_bell();
            break;
        case 0x08u: // BS
            grid_.backspace();
            break;
        case 0x09u: // HT
            grid_.horizontal_tab(1);
            break;
        case 0x0Au: // LF
        case 0x0Bu: // VT
        case 0x0Cu: // FF
            grid_.line_feed();
            break;
        case 0x0Du: // CR
            grid_.carriage_return();
            break;
        case 0x0Eu: // SO (Locking Shift 1 -> G1)
            grid_.charsets_mut().gl = 1;
            break;
        case 0x0Fu: // SI (Locking Shift 0 -> G0)
            grid_.charsets_mut().gl = 0;
            break;
        default:
            break;
    }
}

void Dispatcher::on_esc(uint8_t final_byte, const uint8_t* intermediates, uint8_t intermediate_count) {
    if (intermediate_count == 0) {
        switch (final_byte) {
            case '7': // DECSC
                grid_.save_cursor();
                break;
            case '8': // DECRC
                grid_.restore_cursor();
                break;
            case 'D': // IND (Index)
                grid_.line_feed();
                break;
            case 'E': // NEL (Next Line)
                grid_.carriage_return();
                grid_.line_feed();
                break;
            case 'H': // HTS (Horizontal Tab Set)
                grid_.set_tab_stop();
                break;
            case 'M': // RI (Reverse Index)
                grid_.reverse_index();
                break;
            case 'N': // SS2
                grid_.charsets_mut().single_shift = 2;
                break;
            case 'O': // SS3
                grid_.charsets_mut().single_shift = 3;
                break;
            case 'c': // RIS (Full Reset)
                grid_.hard_reset();
                graphics_.clear_all();
                dynamic_colors_ = DynamicColorState{};
                if (cb_.on_colors_changed) cb_.on_colors_changed();
                break;
            case 'n': // LS2
                grid_.charsets_mut().gl = 2;
                break;
            case 'o': // LS3
                grid_.charsets_mut().gl = 3;
                break;
            case '=': // DECKPAM
                grid_.modes_mut().app_keypad = true;
                break;
            case '>': // DECKPNM
                grid_.modes_mut().app_keypad = false;
                break;
            case 'Z': // DECID (same as DA1)
                reply("\x1b[?62;4;22c");
                break;
            default:
                break;
        }
        return;
    }

    if (intermediate_count == 1) {
        const uint8_t inter = intermediates[0];
        if (inter == '#' && final_byte == '8') {
            // DECALN: Screen Alignment Test (fill screen with 'E')
            grid_.fill_screen_e();
            return;
        }
        // G0..G3 charset designation: ESC ( / ) / * / + <final>
        int slot = -1;
        if (inter == '(') slot = 0;
        else if (inter == ')') slot = 1;
        else if (inter == '*') slot = 2;
        else if (inter == '+') slot = 3;
        if (slot >= 0) {
            CharsetId cs = CharsetId::Ascii;
            if (final_byte == '0') cs = CharsetId::DecSpecial;
            else if (final_byte == 'A') cs = CharsetId::Uk;
            else if (final_byte == '<') cs = CharsetId::DecSupplemental;
            grid_.charsets_mut().designate(static_cast<uint8_t>(slot), cs);
        }
    }
}

void Dispatcher::on_csi(uint8_t final_byte, const CsiParams& p) {
    const uint8_t priv = p.private_marker;
    const uint8_t inter = (p.intermediate_count > 0) ? p.intermediates[0] : 0;

    // ── Kitty Keyboard Protocol: CSI > flags u | CSI < n u | CSI = flags;mode u | CSI ? u ──
    if (final_byte == 'u' && inter == 0) {
        if (priv == '>') {
            const uint8_t flags = static_cast<uint8_t>(std::clamp(p.get(0, 0), 0, 31));
            grid_.modes_mut().kitty_push(flags);
            return;
        }
        if (priv == '<') {
            const uint32_t count = static_cast<uint32_t>(p.get_or_1(0));
            grid_.modes_mut().kitty_pop(count);
            return;
        }
        if (priv == '=') {
            const uint8_t flags = static_cast<uint8_t>(std::clamp(p.get(0, 0), 0, 31));
            const uint8_t mode = static_cast<uint8_t>(p.get(1, 1));
            grid_.modes_mut().kitty_set(flags, mode);
            return;
        }
        if (priv == '?') {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "\x1b[?%uu", static_cast<unsigned>(grid_.modes().kitty_flags()));
            reply(buf);
            return;
        }
        if (priv == 0) {
            // SCORC (ANSI restore cursor)
            grid_.restore_cursor();
            return;
        }
    }

    // ── DECRQM (Request Mode): CSI ? Ps $ p  /  CSI Ps $ p ──
    if (final_byte == 'p' && inter == '$') {
        const int32_t mode = p.get(0, 0);
        if (priv == '?') {
            const int status = query_dec_mode(mode);
            char buf[48];
            std::snprintf(buf, sizeof(buf), "\x1b[?%d;%d$y", mode, status);
            reply(buf);
        } else if (priv == 0) {
            const int status = query_ansi_mode(mode);
            char buf[48];
            std::snprintf(buf, sizeof(buf), "\x1b[%d;%d$y", mode, status);
            reply(buf);
        }
        return;
    }

    // ── DECSTR (Soft Terminal Reset): CSI ! p ──
    if (final_byte == 'p' && inter == '!' && priv == 0) {
        grid_.soft_reset();
        return;
    }

    // ── DECSCUSR (Set Cursor Style): CSI Ps SP q ──
    if (final_byte == 'q' && inter == ' ' && priv == 0) {
        const int32_t ps = p.get(0, 0);
        auto& m = grid_.modes_mut();
        switch (ps) {
            case 0: case 1: m.cursor_shape = CursorShape::Block;     m.cursor_blink = true;  break;
            case 2:         m.cursor_shape = CursorShape::Block;     m.cursor_blink = false; break;
            case 3:         m.cursor_shape = CursorShape::Underline; m.cursor_blink = true;  break;
            case 4:         m.cursor_shape = CursorShape::Underline; m.cursor_blink = false; break;
            case 5:         m.cursor_shape = CursorShape::Bar;       m.cursor_blink = true;  break;
            case 6:         m.cursor_shape = CursorShape::Bar;       m.cursor_blink = false; break;
            default: break;
        }
        return;
    }

    // ── XTVERSION: CSI > q ──
    if (final_byte == 'q' && priv == '>' && inter == 0) {
        reply("\x1bP>|vater 0.1.0\x1b\\");
        return;
    }

    // ── DECSCA (Character Protection Attribute): CSI Ps " q ──
    if (final_byte == 'q' && inter == '"' && priv == 0) {
        const int32_t ps = p.get(0, 0);
        if (ps == 1) grid_.pen_mut().attrs |= cell_attr::kProtected;
        else grid_.pen_mut().attrs &= static_cast<uint16_t>(~cell_attr::kProtected);
        return;
    }

    // ── DECIC / DECDC (Insert/Delete Columns): CSI Ps ' } / CSI Ps ' ~ ──
    if (inter == '\'' && priv == 0) {
        if (final_byte == '}') {
            grid_.insert_columns(p.get_or_1(0));
            return;
        }
        if (final_byte == '~') {
            grid_.delete_columns(p.get_or_1(0));
            return;
        }
    }

    // ── modifyOtherKeys: CSI > 4 ; m ──
    if (final_byte == 'm' && priv == '>' && inter == 0) {
        if (p.get(0, 0) == 4) {
            const int32_t lvl = p.get(1, 0);
            grid_.modes_mut().modify_other_keys = (lvl > 0);
            grid_.modes_mut().modify_other_keys_level = static_cast<uint8_t>(std::clamp(lvl, 0, 2));
        }
        return;
    }

    // ── Secondary DA: CSI > c ──
    if (final_byte == 'c' && priv == '>' && inter == 0) {
        // VT220 (1), version 10 (0.1.0), 0
        reply("\x1b[>1;10;0c");
        return;
    }

    // ── Private DECSET / DECRST: CSI ? ... h / l ──
    if (priv == '?' && inter == 0) {
        if (final_byte == 'h' || final_byte == 'l') {
            const bool set = (final_byte == 'h');
            for (uint8_t i = 0; i < p.count; ++i) {
                handle_dec_mode(p.get(i, 0), set);
            }
            return;
        }
        if (final_byte == 'J') {
            grid_.erase_in_display(p.get(0, 0), true);
            return;
        }
        if (final_byte == 'K') {
            grid_.erase_in_line(p.get(0, 0), true);
            return;
        }
        if (final_byte == 'n') {
            // DECXCPR / printer status
            if (p.get(0, 0) == 6) {
                char buf[48];
                int r = grid_.cursor().row + 1;
                int c = grid_.cursor().col + 1;
                if (grid_.modes().origin_mode) {
                    r = std::max(1, grid_.cursor().row - grid_.scroll_top() + 1);
                    c = std::max(1, grid_.cursor().col - grid_.scroll_left() + 1);
                }
                std::snprintf(buf, sizeof(buf), "\x1b[?%d;%dR", r, c);
                reply(buf);
            }
            return;
        }
        return;
    }

    // Standard CSI sequences (priv == 0, inter == 0)
    if (priv != 0 || inter != 0) return;

    switch (final_byte) {
        case '@': // ICH - Insert Character
            grid_.insert_chars(p.get_or_1(0));
            break;
        case 'A': // CUU - Cursor Up
            grid_.move_cursor_rel(-p.get_or_1(0), 0);
            break;
        case 'B': // CUD - Cursor Down
        case 'e': // VPR - Vertical Position Relative
            grid_.move_cursor_rel(p.get_or_1(0), 0);
            break;
        case 'C': // CUF - Cursor Forward
        case 'a': // HPR - Horizontal Position Relative
            grid_.move_cursor_rel(0, p.get_or_1(0));
            break;
        case 'D': // CUB - Cursor Backward
            grid_.move_cursor_rel(0, -p.get_or_1(0));
            break;
        case 'E': // CNL - Cursor Next Line
            grid_.move_cursor_rel(p.get_or_1(0), 0);
            grid_.carriage_return();
            break;
        case 'F': // CPL - Cursor Previous Line
            grid_.move_cursor_rel(-p.get_or_1(0), 0);
            grid_.carriage_return();
            break;
        case 'G': // CHA - Cursor Character Absolute
        case '`': // HPA
            grid_.set_cursor_col(p.get_or_1(0) - 1);
            break;
        case 'H': // CUP - Cursor Position
        case 'f': // HVP
            grid_.move_cursor_abs(p.get_or_1(0) - 1, p.get_or_1(1) - 1);
            break;
        case 'I': // CHT - Cursor Forward Tabulation
            grid_.horizontal_tab(p.get_or_1(0));
            break;
        case 'J': // ED - Erase in Display
            grid_.erase_in_display(p.get(0, 0), false);
            if (p.get(0, 0) == 2) {
                graphics_.clear_screen_placements(grid_.modes().alt_screen);
            }
            break;
        case 'K': // EL - Erase in Line
            grid_.erase_in_line(p.get(0, 0), false);
            break;
        case 'L': // IL - Insert Line
            grid_.insert_lines(p.get_or_1(0));
            break;
        case 'M': // DL - Delete Line
            grid_.delete_lines(p.get_or_1(0));
            break;
        case 'P': // DCH - Delete Character
            grid_.delete_chars(p.get_or_1(0));
            break;
        case 'S': // SU - Scroll Up (grid's on_scroll_up hook scrolls inline images automatically)
            grid_.scroll_up(p.get_or_1(0), grid_.scroll_top(), grid_.scroll_bottom());
            break;
        case 'T': // SD - Scroll Down
            grid_.scroll_down(p.get_or_1(0), grid_.scroll_top(), grid_.scroll_bottom());
            break;
        case 'X': // ECH - Erase Character
            grid_.erase_chars(p.get_or_1(0));
            break;
        case 'Z': // CBT - Cursor Backward Tabulation
            grid_.backward_tab(p.get_or_1(0));
            break;
        case 'b': { // REP - Repeat Preceding Graphic Character
            const char32_t last = grid_.last_printed_cp();
            if (last != 0) {
                const int n = std::min(p.get_or_1(0), grid_.cols() * grid_.rows());
                for (int i = 0; i < n; ++i) grid_.put_char(last);
            }
            break;
        }
        case 'c': // DA1 - Primary Device Attributes
            if (p.get(0, 0) == 0) {
                // VT220 (62), Sixel (4), ANSI Color (22), Left/Right Margins (69)
                reply("\x1b[?62;4;22;69c");
            }
            break;
        case 'd': // VPA - Vertical Position Absolute
            grid_.set_cursor_row(p.get_or_1(0) - 1);
            break;
        case 'g': // TBC - Tab Clear
            grid_.clear_tab_stop(p.get(0, 0));
            break;
        case 'h': // SM - Set Mode
            for (uint8_t i = 0; i < p.count; ++i) handle_ansi_mode(p.get(i, 0), true);
            break;
        case 'l': // RM - Reset Mode
            for (uint8_t i = 0; i < p.count; ++i) handle_ansi_mode(p.get(i, 0), false);
            break;
        case 'm': // SGR - Select Graphic Rendition
            handle_sgr(p);
            break;
        case 'n': { // DSR - Device Status Report
            const int32_t code = p.get(0, 0);
            if (code == 5) {
                reply("\x1b[0n"); // Terminal OK
            } else if (code == 6) {
                int r = grid_.cursor().row + 1;
                int c = grid_.cursor().col + 1;
                if (grid_.modes().origin_mode) {
                    r = std::max(1, grid_.cursor().row - grid_.scroll_top() + 1);
                    c = std::max(1, grid_.cursor().col - grid_.scroll_left() + 1);
                }
                char buf[48];
                std::snprintf(buf, sizeof(buf), "\x1b[%d;%dR", r, c);
                reply(buf);
            }
            break;
        }
        case 'r': { // DECSTBM - Set Top and Bottom Margins
            const int top = p.get_or_1(0) - 1;
            const int bottom = (p.count >= 2 && p.get(1, 0) > 0) ? (p.get(1, 0) - 1) : (grid_.rows() - 1);
            grid_.set_scroll_region(top, bottom);
            break;
        }
        case 's': { // DECSLRM (when DECLRMM enabled) OR SCOSC (save cursor)
            if (grid_.modes().allow_left_right_margin) {
                const int left = p.get_or_1(0) - 1;
                const int right = (p.count >= 2 && p.get(1, 0) > 0) ? (p.get(1, 0) - 1) : (grid_.cols() - 1);
                grid_.set_left_right_margins(left, right);
            } else if (p.count == 0) {
                grid_.save_cursor();
            }
            break;
        }
        case 't': { // XTWINOPS (safe subset only; CSI 21t title query DISABLED per §8 security!)
            const int32_t op = p.get(0, 0);
            char buf[64];
            if (op == 14) { // Report text area size in pixels
                std::snprintf(buf, sizeof(buf), "\x1b[4;%d;%dt",
                              grid_.rows() * cell_px_h_, grid_.cols() * cell_px_w_);
                reply(buf);
            } else if (op == 16) { // Report cell size in pixels
                std::snprintf(buf, sizeof(buf), "\x1b[6;%d;%dt", cell_px_h_, cell_px_w_);
                reply(buf);
            } else if (op == 18) { // Report text area size in characters
                std::snprintf(buf, sizeof(buf), "\x1b[8;%d;%dt", grid_.rows(), grid_.cols());
                reply(buf);
            }
            // op == 20 / 21 (report icon/window title) intentionally ignored per §8 security!
            break;
        }
        default:
            break;
    }
}

void Dispatcher::handle_sgr(const CsiParams& p) {
    auto& pen = grid_.pen_mut();
    if (p.count == 0) {
        pen.reset();
        grid_.refresh_pen_ext();
        return;
    }

    auto parse_extended_color = [&](uint8_t& i, uint32_t& target_color) {
        // Supports both colon subparameters (38:2::R:G:B, 38:5:N) and semicolon (38;2;R;G;B, 38;5;N)
        if (p.has_subparams(i)) {
            const int32_t mode = p.sub(i, 0, -1);
            if (mode == 5) {
                const int32_t idx = std::clamp(p.sub(i, 1, 0), 0, 255);
                target_color = color_indexed(static_cast<uint8_t>(idx));
            } else if (mode == 2) {
                // ITU T.416 format: 38:2:colorspace:R:G:B (5 subparams) OR 38:2:R:G:B (4 subparams)
                const size_t off = (p.items[i].sub_count >= 5) ? 2 : 1;
                const uint8_t r = static_cast<uint8_t>(std::clamp(p.sub(i, off + 0, 0), 0, 255));
                const uint8_t g = static_cast<uint8_t>(std::clamp(p.sub(i, off + 1, 0), 0, 255));
                const uint8_t b = static_cast<uint8_t>(std::clamp(p.sub(i, off + 2, 0), 0, 255));
                target_color = color_rgb(r, g, b);
            }
            return;
        }
        if (i + 1 >= p.count) return;
        const int32_t mode = p.get(i + 1, -1);
        if (mode == 5 && i + 2 < p.count) {
            const int32_t idx = std::clamp(p.get(i + 2, 0), 0, 255);
            target_color = color_indexed(static_cast<uint8_t>(idx));
            i = static_cast<uint8_t>(i + 2);
        } else if (mode == 2 && i + 4 < p.count) {
            const uint8_t r = static_cast<uint8_t>(std::clamp(p.get(i + 2, 0), 0, 255));
            const uint8_t g = static_cast<uint8_t>(std::clamp(p.get(i + 3, 0), 0, 255));
            const uint8_t b = static_cast<uint8_t>(std::clamp(p.get(i + 4, 0), 0, 255));
            target_color = color_rgb(r, g, b);
            i = static_cast<uint8_t>(i + 4);
        }
    };

    bool ext_dirty = false;
    for (uint8_t i = 0; i < p.count; ++i) {
        const int32_t v = p.get(i, 0);
        switch (v) {
            case 0:
                pen.reset();
                ext_dirty = true;
                break;
            case 1: pen.attrs |= cell_attr::kBold; break;
            case 2: pen.attrs |= cell_attr::kDim; break;
            case 3: pen.attrs |= cell_attr::kItalic; break;
            case 4: {
                UnderlineStyle st = UnderlineStyle::Single;
                if (p.has_subparams(i)) {
                    const int32_t sub = p.sub(i, 0, 1);
                    if (sub >= 0 && sub <= 5) st = static_cast<UnderlineStyle>(sub);
                }
                pen.attrs = set_underline_style(pen.attrs, st);
                break;
            }
            case 5: case 6: pen.attrs |= cell_attr::kBlink; break;
            case 7: pen.attrs |= cell_attr::kInverse; break;
            case 8: pen.attrs |= cell_attr::kInvisible; break;
            case 9: pen.attrs |= cell_attr::kStrikethrough; break;
            case 21: pen.attrs = set_underline_style(pen.attrs, UnderlineStyle::Double); break;
            case 22: pen.attrs &= static_cast<uint16_t>(~(cell_attr::kBold | cell_attr::kDim)); break;
            case 23: pen.attrs &= static_cast<uint16_t>(~cell_attr::kItalic); break;
            case 24: pen.attrs = set_underline_style(pen.attrs, UnderlineStyle::None); break;
            case 25: pen.attrs &= static_cast<uint16_t>(~cell_attr::kBlink); break;
            case 27: pen.attrs &= static_cast<uint16_t>(~cell_attr::kInverse); break;
            case 28: pen.attrs &= static_cast<uint16_t>(~cell_attr::kInvisible); break;
            case 29: pen.attrs &= static_cast<uint16_t>(~cell_attr::kStrikethrough); break;
            case 30: case 31: case 32: case 33: case 34: case 35: case 36: case 37:
                pen.fg = color_indexed(static_cast<uint8_t>(v - 30));
                break;
            case 38:
                parse_extended_color(i, pen.fg);
                break;
            case 39:
                pen.fg = color_default();
                break;
            case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47:
                pen.bg = color_indexed(static_cast<uint8_t>(v - 40));
                break;
            case 48:
                parse_extended_color(i, pen.bg);
                break;
            case 49:
                pen.bg = color_default();
                break;
            case 53: pen.attrs |= cell_attr::kOverline; break;
            case 55: pen.attrs &= static_cast<uint16_t>(~cell_attr::kOverline); break;
            case 58:
                parse_extended_color(i, pen.ext.underline_color);
                ext_dirty = true;
                break;
            case 59:
                pen.ext.underline_color = color_default();
                ext_dirty = true;
                break;
            case 90: case 91: case 92: case 93: case 94: case 95: case 96: case 97:
                pen.fg = color_indexed(static_cast<uint8_t>(v - 90 + 8));
                break;
            case 100: case 101: case 102: case 103: case 104: case 105: case 106: case 107:
                pen.bg = color_indexed(static_cast<uint8_t>(v - 100 + 8));
                break;
            default:
                break;
        }
    }
    if (ext_dirty) {
        grid_.refresh_pen_ext();
    }
}

void Dispatcher::handle_ansi_mode(int32_t mode, bool set) {
    auto& m = grid_.modes_mut();
    switch (mode) {
        case 2:  m.keyboard_action = set; break;
        case 4:  m.insert_mode = set; break;
        case 12: m.send_receive = set; break;
        case 20: m.line_feed_new_line = set; break;
        default: break;
    }
}

void Dispatcher::handle_dec_mode(int32_t mode, bool set) {
    auto& m = grid_.modes_mut();
    switch (mode) {
        case 1:    m.cursor_keys_app = set; break;
        case 3:    m.col_132 = set; grid_.reset_margins(); grid_.erase_in_display(2); grid_.move_cursor_abs(0, 0); break;
        case 4:    m.smooth_scroll = set; break;
        case 5:    m.reverse_video = set; grid_.mark_all_dirty(); break;
        case 6:    m.origin_mode = set; grid_.move_cursor_abs(0, 0); break;
        case 7:    m.auto_wrap = set; break;
        case 8:    m.auto_repeat = set; break;
        case 9:    m.mouse_mode = set ? MouseTrackingMode::X10 : MouseTrackingMode::None; break;
        case 12:   m.cursor_blink = set; break;
        case 25:   m.cursor_visible = set; break;
        case 45:   m.reverse_wraparound = set; break;
        case 47:
            if (set) grid_.enter_alt_screen(false, false);
            else     grid_.leave_alt_screen(false);
            break;
        case 69:
            m.allow_left_right_margin = set;
            if (!set) grid_.reset_margins();
            break;
        case 1000: m.mouse_mode = set ? MouseTrackingMode::Normal   : MouseTrackingMode::None; break;
        case 1002: m.mouse_mode = set ? MouseTrackingMode::Button   : MouseTrackingMode::None; break;
        case 1003: m.mouse_mode = set ? MouseTrackingMode::AnyEvent : MouseTrackingMode::None; break;
        case 1004: m.focus_events = set; break;
        case 1005: m.mouse_encoding = set ? MouseEncoding::Utf8      : MouseEncoding::Default; break;
        case 1006: m.mouse_encoding = set ? MouseEncoding::Sgr       : MouseEncoding::Default; break;
        case 1007: m.alt_scroll = set; break;
        case 1016: m.mouse_encoding = set ? MouseEncoding::SgrPixels : MouseEncoding::Default; break;
        case 1047:
            if (set) grid_.enter_alt_screen(false, true);
            else     grid_.leave_alt_screen(false);
            break;
        case 1048:
            if (set) grid_.save_cursor();
            else     grid_.restore_cursor();
            break;
        case 1049:
            if (set) grid_.enter_alt_screen(true, true);
            else     grid_.leave_alt_screen(true);
            break;
        case 2004: m.bracketed_paste = set; break;
        case 2026: m.synchronized_output = set; break;
        case 2027: m.grapheme_clustering = set; break;
        case 2501: // BiDi implicit mode extension
            m.bidi_implicit = set;
            grid_.mark_all_dirty();
            break;
        default: break;
    }
}

int Dispatcher::query_ansi_mode(int32_t mode) const noexcept {
    const auto& m = grid_.modes();
    switch (mode) {
        case 2:  return m.keyboard_action ? 1 : 2;
        case 4:  return m.insert_mode ? 1 : 2;
        case 12: return m.send_receive ? 1 : 2;
        case 20: return m.line_feed_new_line ? 1 : 2;
        default: return 0; // not recognized
    }
}

int Dispatcher::query_dec_mode(int32_t mode) const noexcept {
    const auto& m = grid_.modes();
    switch (mode) {
        case 1:    return m.cursor_keys_app ? 1 : 2;
        case 5:    return m.reverse_video ? 1 : 2;
        case 6:    return m.origin_mode ? 1 : 2;
        case 7:    return m.auto_wrap ? 1 : 2;
        case 12:   return m.cursor_blink ? 1 : 2;
        case 25:   return m.cursor_visible ? 1 : 2;
        case 45:   return m.reverse_wraparound ? 1 : 2;
        case 69:   return m.allow_left_right_margin ? 1 : 2;
        case 1000: return m.mouse_mode == MouseTrackingMode::Normal ? 1 : 2;
        case 1002: return m.mouse_mode == MouseTrackingMode::Button ? 1 : 2;
        case 1003: return m.mouse_mode == MouseTrackingMode::AnyEvent ? 1 : 2;
        case 1004: return m.focus_events ? 1 : 2;
        case 1006: return m.mouse_encoding == MouseEncoding::Sgr ? 1 : 2;
        case 1016: return m.mouse_encoding == MouseEncoding::SgrPixels ? 1 : 2;
        case 1049: return m.alt_screen ? 1 : 2;
        case 2004: return m.bracketed_paste ? 1 : 2;
        case 2026: return m.synchronized_output ? 1 : 2;
        case 2027: return m.grapheme_clustering ? 1 : 2;
        case 2501: return m.bidi_implicit ? 1 : 2;
        default:   return 0;
    }
}

bool Dispatcher::parse_rgb_spec(std::string_view spec, uint32_t& out_rgb) {
    if (spec.starts_with("rgb:")) {
        spec.remove_prefix(4);
        const size_t s1 = spec.find('/');
        if (s1 == std::string_view::npos) return false;
        const size_t s2 = spec.find('/', s1 + 1);
        if (s2 == std::string_view::npos) return false;
        const uint8_t r = parse_hex_component(spec.substr(0, s1));
        const uint8_t g = parse_hex_component(spec.substr(s1 + 1, s2 - s1 - 1));
        const uint8_t b = parse_hex_component(spec.substr(s2 + 1));
        out_rgb = (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
        return true;
    }
    if (spec.starts_with("#") && spec.size() == 7) {
        const uint8_t r = parse_hex_component(spec.substr(1, 2));
        const uint8_t g = parse_hex_component(spec.substr(3, 2));
        const uint8_t b = parse_hex_component(spec.substr(5, 2));
        out_rgb = (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
        return true;
    }
    return false;
}

std::string Dispatcher::format_rgb_reply(uint32_t rgb) {
    const unsigned r = (rgb >> 16) & 0xFFu;
    const unsigned g = (rgb >> 8) & 0xFFu;
    const unsigned b = rgb & 0xFFu;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "rgb:%02x%02x/%02x%02x/%02x%02x", r, r, g, g, b, b);
    return buf;
}

void Dispatcher::handle_osc_color(int osc_num, std::string_view spec) {
    if (osc_num == 4) {
        // OSC 4 ; index ; spec [; index ; spec ...]
        size_t pos = 0;
        bool changed = false;
        while (pos < spec.size()) {
            size_t s1 = spec.find(';', pos);
            if (s1 == std::string_view::npos) break;
            const int idx = parse_decimal_sv(spec.substr(pos, s1 - pos), -1);
            size_t s2 = spec.find(';', s1 + 1);
            std::string_view val = (s2 == std::string_view::npos)
                                       ? spec.substr(s1 + 1)
                                       : spec.substr(s1 + 1, s2 - s1 - 1);
            pos = (s2 == std::string_view::npos) ? spec.size() : (s2 + 1);
            if (idx < 0 || idx >= 256) continue;
            if (val == "?") {
                const uint32_t rgb = dynamic_colors_.palette[static_cast<size_t>(idx)].value_or(0x808080u);
                reply("\x1b]4;" + std::to_string(idx) + ";" + format_rgb_reply(rgb) + "\x1b\\");
            } else {
                uint32_t rgb = 0;
                if (parse_rgb_spec(val, rgb)) {
                    dynamic_colors_.palette[static_cast<size_t>(idx)] = rgb;
                    changed = true;
                }
            }
        }
        if (changed) {
            grid_.mark_all_dirty();
            if (cb_.on_colors_changed) cb_.on_colors_changed();
        }
        return;
    }

    auto apply_one = [&](std::optional<uint32_t>& slot, uint32_t fallback_rgb, int code) {
        if (spec == "?") {
            reply("\x1b]" + std::to_string(code) + ";" + format_rgb_reply(slot.value_or(fallback_rgb)) + "\x1b\\");
        } else {
            uint32_t rgb = 0;
            if (parse_rgb_spec(spec, rgb)) {
                slot = rgb;
                grid_.mark_all_dirty();
                if (cb_.on_colors_changed) cb_.on_colors_changed();
            }
        }
    };

    if (osc_num == 10) apply_one(dynamic_colors_.default_fg, 0xE6E6EAu, 10);
    else if (osc_num == 11) apply_one(dynamic_colors_.default_bg, 0x1C1C1Du, 11);
    else if (osc_num == 12) apply_one(dynamic_colors_.cursor_color, 0x3385FFu, 12);
}

void Dispatcher::on_osc(std::string_view payload) {
    const size_t semi = payload.find(';');
    const std::string_view head = (semi == std::string_view::npos) ? payload : payload.substr(0, semi);
    const std::string_view body = (semi == std::string_view::npos) ? std::string_view{} : payload.substr(semi + 1);
    const int osc_num = parse_decimal_sv(head, -1);

    switch (osc_num) {
        case 0:
        case 2: // Window title
            title_ = std::string(body);
            if (cb_.on_title_changed) cb_.on_title_changed(title_);
            break;

        case 4:
        case 10:
        case 11:
        case 12:
            handle_osc_color(osc_num, body);
            break;

        case 104: // Reset indexed colors
            if (body.empty()) {
                dynamic_colors_.palette.fill(std::nullopt);
            } else {
                const int idx = parse_decimal_sv(body, -1);
                if (idx >= 0 && idx < 256) dynamic_colors_.palette[static_cast<size_t>(idx)].reset();
            }
            grid_.mark_all_dirty();
            if (cb_.on_colors_changed) cb_.on_colors_changed();
            break;
        case 110: dynamic_colors_.default_fg.reset(); grid_.mark_all_dirty(); if (cb_.on_colors_changed) cb_.on_colors_changed(); break;
        case 111: dynamic_colors_.default_bg.reset(); grid_.mark_all_dirty(); if (cb_.on_colors_changed) cb_.on_colors_changed(); break;
        case 112: dynamic_colors_.cursor_color.reset(); grid_.mark_all_dirty(); if (cb_.on_colors_changed) cb_.on_colors_changed(); break;

        case 7: { // Current Working Directory: file://host/path
            std::string_view p = body;
            if (p.starts_with("file://")) {
                p.remove_prefix(7);
                const size_t slash = p.find('/');
                if (slash != std::string_view::npos) {
                    cwd_ = percent_decode(p.substr(slash));
                    if (cb_.on_cwd_changed) cb_.on_cwd_changed(cwd_);
                }
            }
            break;
        }

        case 8: { // Hyperlinks: OSC 8 ; params ; uri ST
            const size_t sep = body.find(';');
            if (sep == std::string_view::npos) break;
            const std::string_view params = body.substr(0, sep);
            const std::string_view uri = body.substr(sep + 1);
            std::string_view link_id;
            size_t pos = 0;
            while (pos < params.size()) {
                size_t colon = params.find(':', pos);
                if (colon == std::string_view::npos) colon = params.size();
                std::string_view kv = params.substr(pos, colon - pos);
                if (kv.starts_with("id=")) {
                    link_id = kv.substr(3);
                }
                pos = colon + 1;
            }
            if (uri.empty()) {
                grid_.pen_mut().ext.hyperlink_id = 0;
            } else {
                grid_.pen_mut().ext.hyperlink_id = grid_.hyperlink_table().intern(link_id, uri);
            }
            grid_.refresh_pen_ext();
            break;
        }

        case 52: { // Clipboard (subject to §8 security policy!)
            const size_t sep = body.find(';');
            if (sep == std::string_view::npos) break;
            const std::string_view sel = body.substr(0, sep);
            const std::string_view data = body.substr(sep + 1);
            if (data == "?") {
                if (clipboard_policy_.allow_read && cb_.on_clipboard_read) {
                    const std::string clip = cb_.on_clipboard_read(sel);
                    const std::string enc = GraphicsStore::encode_base64(
                        reinterpret_cast<const uint8_t*>(clip.data()), clip.size());
                    reply("\x1b]52;" + std::string(sel) + ";" + enc + "\x1b\\");
                }
            } else if (clipboard_policy_.allow_write && cb_.on_clipboard_write) {
                std::vector<uint8_t> decoded;
                if (GraphicsStore::decode_base64(data, decoded)) {
                    cb_.on_clipboard_write(sel, std::string_view(
                        reinterpret_cast<const char*>(decoded.data()), decoded.size()));
                }
            }
            break;
        }

        case 133: { // Shell Integration (FinalTerm / OSC 133): A=prompt, B=command, C=output, D;code=done
            if (body.empty()) break;
            const char marker = body[0];
            int exit_code = -1;
            auto& meta = grid_.row(grid_.cursor().row).meta;
            if (marker == 'A') meta.prompt_start = true;
            else if (marker == 'B') meta.command_start = true;
            else if (marker == 'C') meta.output_start = true;
            else if (marker == 'D') {
                meta.command_end = true;
                const size_t s = body.find(';');
                if (s != std::string_view::npos) {
                    exit_code = parse_decimal_sv(body.substr(s + 1), 0);
                    meta.exit_code = static_cast<int16_t>(std::clamp(exit_code, -1, 32767));
                    last_exit_code_ = exit_code;
                }
            }
            if (cb_.on_shell_integration) cb_.on_shell_integration(marker, exit_code);
            break;
        }

        case 1337: { // Partial iTerm2 compat: CurrentDir=...
            if (body.starts_with("CurrentDir=")) {
                cwd_ = std::string(body.substr(11));
                if (cb_.on_cwd_changed) cb_.on_cwd_changed(cwd_);
            }
            break;
        }

        default:
            break;
    }
}

void Dispatcher::on_dcs(uint8_t final_byte, const CsiParams& params, std::string_view payload) {
    // Sixel Graphics: DCS P1 ; P2 ; P3 q <sixel_data> ST
    if (final_byte == 'q' && params.private_marker == 0 && params.intermediate_count == 0) {
        const bool transparent_bg = (params.get(1, 0) == 1);
        DecodedImage img{};
        if (SixelDecoder::decode(payload, transparent_bg, 0xFF1D1C1Cu, img)) {
            const uint32_t img_h = img.height;
            graphics_.add_sixel_image(std::move(img), grid_.cursor().row, grid_.cursor().col, grid_.modes().alt_screen);
            const int advance_rows = std::max(1, static_cast<int>((img_h + cell_px_h_ - 1) / cell_px_h_));
            for (int i = 0; i < advance_rows; ++i) grid_.line_feed();
            grid_.carriage_return();
            grid_.mark_all_dirty();
        }
        return;
    }

    // XTGETTCAP: DCS + q <hex_cap_names> ST
    if (final_byte == 'q' && params.intermediate_count == 1 && params.intermediates[0] == '+') {
        // Respond negatively (0+r) for unknown caps safely
        reply("\x1bP0+r\x1b\\");
    }
}

void Dispatcher::on_apc(std::string_view payload) {
    if (!payload.empty() && payload[0] == 'G') {
        int adv_rows = 0;
        int adv_cols = 0;
        std::string resp = graphics_.handle_kitty_apc(
            payload,
            grid_.cursor().row,
            grid_.cursor().col,
            cell_px_w_,
            cell_px_h_,
            grid_.modes().alt_screen,
            adv_rows,
            adv_cols);
        if (!resp.empty()) reply(resp);
        for (int i = 0; i < adv_rows; ++i) grid_.line_feed();
        if (adv_cols > 0) grid_.move_cursor_rel(0, adv_cols);
        grid_.mark_all_dirty();
    }
}

} // namespace vater::vt

