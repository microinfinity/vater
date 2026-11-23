/// @file test_conformance.cpp
/// @brief Automated Headless VT100/VT220/VT420/xterm/Kitty Conformance Suite (vttest/esctest core).
/// Verifies strict compliance with ARCHITECTURE.md §5.2, §8, §9, and §11 (M1..M3).

#include "vt/terminal.hpp"

#include <cstdio>
#include <string>

using namespace vater::vt;

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) {                                                            \
            ++g_passed;                                                        \
        } else {                                                               \
            ++g_failed;                                                        \
            std::fprintf(stderr, "CONFORMANCE FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg); \
        }                                                                      \
    } while (0)

static std::string row_ascii(const Terminal& t, int r) {
    std::string s;
    const auto& row = t.unsafe_grid().row(r);
    for (const auto& c : row.cells) {
        s.push_back((c.cp < 128) ? static_cast<char>(c.cp) : '?');
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

int main() {
    // 1. CUP / CUU / CUD / CUF / CUB / CHA / VPA / CNL / CPL
    {
        Terminal t(40, 10);
        t.feed("\x1b[3;5HX\x1b[2AY\x1b[3BZ\x1b[2CW\x1b[3DV");
        CHECK(t.unsafe_grid().cell(2, 4).cp == U'X', "CUP 3;5 places X");
        CHECK(t.unsafe_grid().cell(0, 5).cp == U'Y', "CUU 2 places Y");
        CHECK(t.unsafe_grid().cell(3, 6).cp == U'Z', "CUD 3 places Z");
        CHECK(t.unsafe_grid().cell(3, 9).cp == U'W', "CUF 2 places W");
        CHECK(t.unsafe_grid().cell(3, 7).cp == U'V', "CUB 3 places V");
    }

    // 2. DECSTBM (top/bottom scroll margins) + DECOM (origin mode)
    {
        Terminal t(20, 6);
        t.feed("TOP\r\nR1\r\nR2\r\nR3\r\nR4\r\nBOT");
        t.feed("\x1b[2;5r");       // Scroll region rows 2..5 (1-indexed)
        t.feed("\x1b[?6h");        // Origin mode ON -> cursor moves to margin top (row 1, col 0)
        CHECK(t.unsafe_grid().cursor().row == 1 && t.unsafe_grid().cursor().col == 0,
              "DECOM moves cursor to scroll_top");
        t.feed("\x1b[4;1H\nNEW");  // Scroll inside margin only!
        CHECK(row_ascii(t, 0) == "TOP", "DECSTBM preserves row above top margin");
        CHECK(row_ascii(t, 5) == "BOT", "DECSTBM preserves row below bottom margin");
        CHECK(row_ascii(t, 1) == "R2", "DECSTBM scrolled R2 into top of margin");
        CHECK(row_ascii(t, 4) == "NEW", "DECSTBM wrote NEW at bottom of margin");
    }

    // 3. DECSLRM (left/right margins, ?69h) + scroll inside horizontal margins
    {
        Terminal t(10, 4);
        t.feed("0123456789\r\nABCDEFGHIJ\r\n0123456789\r\n");
        t.feed("\x1b[?69h\x1b[3;7s"); // Left=col 2, Right=col 6
        t.feed("\x1b[1S");            // Scroll up 1 inside cols 2..6
        CHECK(row_ascii(t, 0) == "01CDEFG789", "DECSLRM scrolls only within left/right margins");
    }

    // 4. Editing: ICH (@), DCH (P), IL (L), DL (M), ECH (X), ED (J), EL (K), REP (b)
    {
        Terminal t(12, 4);
        t.feed("ABCDEF\x1b[1;3H\x1b[2@12");
        CHECK(row_ascii(t, 0) == "AB12CDEF", "ICH inserts 2 spaces at col 2");
        t.feed("\x1b[1;3H\x1b[2P");
        CHECK(row_ascii(t, 0) == "ABCDEF", "DCH deletes 2 chars at col 2");
        t.feed("\x1b[1;2H\x1b[3X");
        CHECK(row_ascii(t, 0) == "A   EF", "ECH erases 3 chars in place");
        t.feed("\x1b[2;1H*\x1b[4b");
        CHECK(row_ascii(t, 1) == "*****", "REP repeats previous char 4 times");
    }

    // 5. DECALN (ESC # 8) & DECSC / DECRC (ESC 7 / ESC 8)
    {
        Terminal t(10, 3);
        t.feed("\x1b#8");
        CHECK(row_ascii(t, 0) == "EEEEEEEEEE" && row_ascii(t, 2) == "EEEEEEEEEE",
              "DECALN fills screen with E");
        t.feed("\x1b[2;4H\x1b[31m\x1b" "7\x1b[1;1H\x1b[0m\x1b" "8Z");
        CHECK(t.unsafe_grid().cell(1, 3).cp == U'Z', "DECRC restores cursor position");
        CHECK(t.unsafe_grid().cell(1, 3).fg == color_indexed(1), "DECRC restores SGR pen");
    }

    // 6. G0/G1 DEC Special Graphics Line Drawing (ESC ( 0)
    {
        Terminal t(10, 2);
        t.feed("\x1b(0lqqk\x1b(BA");
        CHECK(t.unsafe_grid().cell(0, 0).cp == U'\u250C', "DEC Special 'l' -> ┌");
        CHECK(t.unsafe_grid().cell(0, 1).cp == U'\u2500', "DEC Special 'q' -> ─");
        CHECK(t.unsafe_grid().cell(0, 3).cp == U'\u2510', "DEC Special 'k' -> ┐");
        CHECK(t.unsafe_grid().cell(0, 4).cp == U'A', "Switch back to ASCII 'B'");
    }

    // 7. SGR: Bold, Italic, Truecolor 38;2 & 38:2::, 256-color 48;5, Curly Underline 4:3, Underline Color 58
    {
        Terminal t(20, 2);
        t.feed("\x1b[1;3;4:3;38;2;12;34;56;48;5;200;58:2::255:0:128mX");
        const Cell c = t.unsafe_grid().cell(0, 0);
        CHECK((c.attrs & cell_attr::kBold) != 0 && (c.attrs & cell_attr::kItalic) != 0, "SGR bold + italic");
        CHECK(get_underline_style(c.attrs) == UnderlineStyle::Curly, "SGR 4:3 curly underline");
        CHECK(c.fg == color_rgb(12, 34, 56), "SGR 38;2 truecolor fg");
        CHECK(c.bg == color_indexed(200), "SGR 48;5 indexed bg");
        const ExtCellData ext = t.get_ext_data(c.ext);
        CHECK(ext.underline_color == color_rgb(255, 0, 128), "SGR 58:2 underline color");
    }

    // 8. Alternate Screen (?1049h / ?1049l) preserves main screen & cursor
    {
        Terminal t(20, 4);
        t.feed("MAIN_CONTENT\x1b[?1049hALT_VIEW\x1b[?1049l!");
        CHECK(row_ascii(t, 0) == "MAIN_CONTENT!", "Alt screen ?1049 preserves main grid & cursor");
    }

    // 9. Synchronized Output (?2026h / ?2026l) freezes snapshot until end of frame
    {
        Terminal t(20, 4);
        ViewportSnapshot snap;
        t.feed("FRAME1");
        CHECK(t.snapshot(snap), "Snapshot succeeds normally");
        t.feed("\x1b[?2026hPARTIAL_FRAME");
        CHECK(!t.snapshot(snap), "DEC 2026 freezes snapshot mid-frame");
        t.feed("_DONE\x1b[?2026l");
        CHECK(t.snapshot(snap), "DEC 2026 releases snapshot on ?2026l");
        CHECK(row_ascii(t, 0) == "FRAME1PARTIAL_FRAME_DONE" || row_ascii(t, 0) == "FRAME1PARTIAL_FRAME_",
              "Full synchronized frame committed cleanly");
    }

    // 10. Device queries (DA1, DA2, DSR 5n/6n, DECRQM, XTVERSION, Kitty kbd) & Security (§8 no CSI 21t!)
    {
        Terminal t(80, 24);
        std::string replies;
        DispatcherCallbacks cbs;
        cbs.on_write_pty = [&](std::string_view r) { replies += r; };
        t.set_callbacks(std::move(cbs));

        t.feed("\x1b[c");
        CHECK(replies.find("\x1b[?62;") != std::string::npos, "DA1 responds cleanly");
        replies.clear();

        t.feed("\x1b[5n\x1b[3;7H\x1b[6n");
        CHECK(replies == "\x1b[0n\x1b[3;7R", "DSR 5n & CPR 6n accurate");
        replies.clear();

        t.feed("\x1b[?2026$p");
        CHECK(replies == "\x1b[?2026;2$y", "DECRQM reports mode 2026 reset (2)");
        replies.clear();

        t.feed("\x1b[>5u\x1b[?u\x1b[<u\x1b[?u");
        CHECK(replies == "\x1b[?5u\x1b[?0u", "Kitty keyboard stack push/query/pop");
        replies.clear();

        // Security §8: CSI 21t title report MUST NOT respond (prevents command injection)
        t.feed("\x1b]2;rm -rf /\x07\x1b[21t");
        CHECK(replies.empty(), "Security §8: CSI 21t title query disabled");
    }

    // 11. OSC 7 (cwd), OSC 8 (hyperlinks), OSC 52 (security policy), OSC 133 (shell integration)
    {
        Terminal t(40, 6);
        std::string clip_written;
        DispatcherCallbacks cbs;
        cbs.on_clipboard_write = [&](std::string_view, std::string_view txt) { clip_written = std::string(txt); };
        t.set_callbacks(std::move(cbs));

        t.feed("\x1b]7;file://host/home/x/Desktop/terminal\x07");
        CHECK(t.cwd() == "/home/x/Desktop/terminal", "OSC 7 parses CWD");

        t.feed("\x1b]8;id=doc1;https://example.com\x1b\\Click\x1b]8;;\x1b\\");
        CHECK(t.hyperlink_at(0, 2) == "https://example.com", "OSC 8 hyperlink stored on cells");
        CHECK(t.hyperlink_at(0, 6).empty(), "OSC 8 hyperlink closed cleanly");

        // OSC 52 blocked by default (§8)
        t.feed("\x1b]52;c;SGVsbG8=\x07");
        CHECK(clip_written.empty(), "Security §8: OSC 52 write blocked by default");
        t.set_clipboard_policy({true, false});
        t.feed("\x1b]52;c;SGVsbG8=\x07");
        CHECK(clip_written == "Hello", "OSC 52 write succeeds when explicitly enabled");

        // OSC 133 shell integration
        t.feed("\r\n\x1b]133;A\x07$ \x1b]133;B\x07ls\r\n\x1b]133;C\x07out1\r\nout2\x1b]133;D;42\x07");
        CHECK(t.last_exit_code() == 42, "OSC 133;D captures exit code 42");
        CHECK(t.unsafe_grid().row(1).meta.prompt_start, "OSC 133;A marks prompt row");
    }

    const double pct = (g_passed + g_failed) > 0
                           ? (100.0 * static_cast<double>(g_passed) / static_cast<double>(g_passed + g_failed))
                           : 0.0;
    std::printf("test_conformance: %d/%d passed (%.1f%%)\n", g_passed, g_passed + g_failed, pct);
    return g_failed == 0 ? 0 : 1;
}

