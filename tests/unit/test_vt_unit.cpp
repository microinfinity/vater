/// @file test_vt_unit.cpp
/// @brief Unit Tests for L3 Terminal Core & L1 OS Layer:
/// UTF-8 DFA + SIMD scanner, wcwidth, Cell 16-byte layout, Scrollback RLE/eviction,
/// Grid + Reflow preserving cursor, Sixel & Kitty Graphics, and POSIX PTY spawn.

#include "os/pty.hpp"
#include "os/spawn.hpp"
#include "vt/cell.hpp"
#include "vt/graphics/kitty_graphics.hpp"
#include "vt/graphics/sixel.hpp"
#include "vt/grid.hpp"
#include "vt/parser.hpp"
#include "vt/scrollback.hpp"
#include "vt/terminal.hpp"
#include "vt/utf8_decoder.hpp"
#include "vt/wcwidth.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

using namespace vater::vt;

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (cond) {                                                        \
            ++g_passed;                                                    \
        } else {                                                           \
            ++g_failed;                                                    \
            std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg); \
        }                                                                  \
    } while (0)

static void test_utf8_and_simd() {
    // 1. SIMD ASCII fast-path scanner
    const char* sample = "Hello Vater Sovereign Terminal 1234567890!\x1b[31mRed";
    const size_t n = scan_printable_ascii(reinterpret_cast<const uint8_t*>(sample), std::strlen(sample));
    CHECK(n == 42, "SIMD ASCII scanner stops cleanly at ESC (0x1B)");

    const char* with_utf8 = "ASCII_prefix_0123456789\xD9\x85"; // Arabic Meem
    const size_t n2 = scan_printable_ascii(reinterpret_cast<const uint8_t*>(with_utf8), std::strlen(with_utf8));
    CHECK(n2 == 23, "SIMD ASCII scanner stops at UTF-8 high-bit byte");

    // 2. Branchless UTF-8 DFA decoder: valid multi-byte (Arabic + CJK + Emoji)
    Utf8Decoder dec;
    auto decode_str = [&](std::string_view s) {
        std::vector<char32_t> cps;
        dec.reset();
        for (uint8_t b : s) {
            char32_t cp = 0;
            int r = dec.decode(b, cp);
            if (r == 1) cps.push_back(cp);
            else if (r < 0) cps.push_back(0xFFFDu);
        }
        return cps;
    };

    auto ar = decode_str("\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7"); // مرحبا
    CHECK(ar.size() == 5 && ar[0] == 0x0645 && ar[4] == 0x0627, "UTF-8 Arabic decode");

    auto emoji = decode_str("\xF0\x9F\x9A\x80"); // 🚀 U+1F680
    CHECK(emoji.size() == 1 && emoji[0] == 0x1F680u, "UTF-8 4-byte emoji decode");

    // 3. Reject overlong NUL (C0 80) and UTF-16 surrogate (ED A0 80)
    auto overlong = decode_str(std::string_view("\xC0\x80", 2));
    CHECK(!overlong.empty() && overlong[0] == 0xFFFDu, "UTF-8 rejects overlong sequence");

    auto surrogate = decode_str("\xED\xA0\x80");
    CHECK(!surrogate.empty() && surrogate[0] == 0xFFFDu, "UTF-8 rejects UTF-16 surrogate");
}

static void test_wcwidth() {
    CHECK(wcwidth(U'A') == 1, "ASCII width == 1");
    CHECK(wcwidth(U'\u0645') == 1, "Arabic letter width == 1");
    CHECK(wcwidth(U'\u064E') == 0, "Arabic Fatha combining mark width == 0");
    CHECK(wcwidth(U'\u0301') == 0, "Combining acute accent width == 0");
    CHECK(wcwidth(U'\u4E2D') == 2, "CJK ideograph width == 2");
    CHECK(wcwidth(U'\U0001F680') == 2, "Emoji rocket width == 2");
    CHECK(vater::vt::wcwidth(U'\x1B') == -1, "Control char width == -1");
}

static void test_scrollback_rle() {
    Scrollback sb(500);
    for (int i = 0; i < 600; ++i) {
        std::vector<Cell> row(80, Cell{});
        row[0].cp = static_cast<char32_t>(U'A' + (i % 26));
        row[1].cp = static_cast<char32_t>(U'0' + (i % 10));
        RowMeta meta{};
        meta.cols = 80;
        sb.push(row, meta);
    }
    CHECK(sb.size() == 500, "Scrollback enforces max_lines ring eviction");

    ScrollbackRow out;
    CHECK(sb.get(0, out, 80), "Scrollback get(0) succeeds");
    // Oldest line should be i = 100
    CHECK(out.cells[0].cp == static_cast<char32_t>(U'A' + (100 % 26)), "Scrollback oldest line matches i=100");
    CHECK(sb.get(499, out, 80), "Scrollback get(499) succeeds");
    CHECK(out.cells[0].cp == static_cast<char32_t>(U'A' + (599 % 26)), "Scrollback newest line matches i=599");
}

static void test_grid_reflow() {
    Terminal term(10, 5);
    term.feed("ABCDEFGHIJKLMNOP"); // 16 chars on 10-col terminal -> wraps onto 2 rows (10 + 6)
    CHECK(term.unsafe_grid().row(0).meta.wrapped == true, "Row 0 marked soft-wrapped");
    CHECK(term.unsafe_grid().cursor().row == 1 && term.unsafe_grid().cursor().col == 6,
          "Cursor at (1,6) before resize");

    // Widen to 20 cols -> should unwrap back onto a single row, cursor at (0, 16)!
    term.resize(20, 5);
    CHECK(term.unsafe_grid().row(0).meta.wrapped == false, "Row 0 unwrapped after widening to 20 cols");
    CHECK(term.unsafe_grid().cell(0, 15).cp == U'P', "All 16 chars restored on row 0");
    CHECK(term.unsafe_grid().cursor().row == 0 && term.unsafe_grid().cursor().col == 16,
          "Cursor preserved accurately at (0,16) after unwrap reflow");

    // Narrow to 6 cols -> should re-wrap onto 3 rows (6 + 6 + 4), cursor at (2, 4)!
    term.resize(6, 5);
    CHECK(term.unsafe_grid().row(0).meta.wrapped == true, "Row 0 wrapped at 6 cols");
    CHECK(term.unsafe_grid().row(1).meta.wrapped == true, "Row 1 wrapped at 6 cols");
    CHECK(term.unsafe_grid().cell(2, 3).cp == U'P', "Last char 'P' reflowed to (2,3)");
    CHECK(term.unsafe_grid().cursor().row == 2 && term.unsafe_grid().cursor().col == 4,
          "Cursor preserved accurately at (2,4) after narrow reflow");
}

static void test_sixel_and_kitty_graphics() {
    // 1. Sixel: 4x6 solid red block (`#1;2;100;0;0#1!4~`)
    DecodedImage simg{};
    CHECK(SixelDecoder::decode("#1;2;100;0;0#1!4~", false, 0xFF000000u, simg), "Sixel decode succeeds");
    CHECK(simg.width == 4 && simg.height == 6, "Sixel dimensions 4x6");
    CHECK(simg.rgba[0] == 255 && simg.rgba[1] == 0 && simg.rgba[2] == 0 && simg.rgba[3] == 255,
          "Sixel pixel (0,0) is red RGBA");

    // 2. Kitty Graphics APC G: transmit & display 2x2 raw RGBA (16 bytes base64)
    std::vector<uint8_t> rgba(16, 0);
    for (int i = 0; i < 4; ++i) {
        rgba[static_cast<size_t>(i * 4 + 0)] = 0;
        rgba[static_cast<size_t>(i * 4 + 1)] = 255;
        rgba[static_cast<size_t>(i * 4 + 2)] = 128;
        rgba[static_cast<size_t>(i * 4 + 3)] = 255;
    }
    std::string b64 = GraphicsStore::encode_base64(rgba.data(), rgba.size());

    Terminal term(80, 24);
    std::string pty_reply;
    DispatcherCallbacks cbs;
    cbs.on_write_pty = [&](std::string_view r) { pty_reply += r; };
    term.set_callbacks(std::move(cbs));

    std::string apc = "\x1b_Ga=T,f=32,s=2,v=2,i=42;" + b64 + "\x1b\\";
    term.feed(apc);
    CHECK(pty_reply == "\x1b_Gi=42;OK\x1b\\", "Kitty graphics sends OK reply with image id");
    CHECK(term.unsafe_graphics().images().count(42) == 1, "Kitty image 42 stored");
    CHECK(term.unsafe_graphics().placements().size() == 1, "Kitty image 42 placed");

    // When i=0 (omitted, e.g. fastfetch kitty-direct), no OK reply must be written to PTY
    pty_reply.clear();
    term.feed("\x1b_Ga=T,f=32,s=2,v=2;" + b64 + "\x1b\\");
    CHECK(pty_reply.empty(), "Kitty graphics suppresses reply when image id is omitted (i=0)");
    term.feed("\x1b_Ga=q,i=31;\x1b\\");
    CHECK(pty_reply == "\x1b_Gi=31;OK\x1b\\", "Kitty graphics still replies to a=q queries");
    term.feed("\x1b_Ga=d,d=A\x1b\\");

    // Delete image via a=d,d=I,i=42
    term.feed("\x1b_Ga=d,d=I,i=42\x1b\\");
    CHECK(term.unsafe_graphics().images().empty(), "Kitty graphics delete frees image");

    // 3. Kitty Graphics o=z zlib callback + aspect-ratio cursor advance + auto scroll-up
    term.set_cell_pixel_size(8, 16);
    term.set_zlib_decompressor([](const uint8_t* /*data*/, size_t /*len*/, size_t /*expected_size*/,
                                  std::vector<uint8_t>& out) -> bool {
        // Return a 2x2 RGBA image (16 bytes)
        out.assign(16, 0xCCu);
        return true;
    });
    term.feed("\x1b[H\x1b_Ga=T,f=32,o=z,s=2,v=2,i=77,c=4;QUJDRA==\x1b\\");
    CHECK(term.unsafe_graphics().images().count(77) == 1, "Kitty graphics o=z decompressor decoded image");
    CHECK(term.unsafe_graphics().placements().size() == 1, "Kitty graphics o=z created placement");
    const int initial_row = term.unsafe_graphics().placements()[0].row;
    // Scroll screen down by 1 line (moving cursor to bottom row 24 and emitting newline)
    term.feed("\x1b[24;1H\n");
    CHECK(!term.unsafe_graphics().placements().empty() &&
              term.unsafe_graphics().placements()[0].row == initial_row - 1,
          "Kitty graphics placement scrolls up with terminal content");

    // 4. Kitty Graphics t=s POSIX shared memory transfer
    const std::string shm_name = "/vater_test_shm_" + std::to_string(::getpid());
    int sfd = ::shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0600);
    CHECK(sfd >= 0, "shm_open created POSIX shared memory object");
    if (sfd >= 0) {
        uint8_t px[4] = {0x10, 0x20, 0x30, 0xFF};
        CHECK(::ftruncate(sfd, 4) == 0 && ::write(sfd, px, 4) == 4, "Wrote 1x1 RGBA pixel to shm");
        ::close(sfd);
        // Base64-encode shm_name
        static constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string b64;
        for (size_t i = 0; i < shm_name.size(); i += 3) {
            uint32_t v = static_cast<uint8_t>(shm_name[i]) << 16;
            if (i + 1 < shm_name.size()) v |= static_cast<uint8_t>(shm_name[i + 1]) << 8;
            if (i + 2 < shm_name.size()) v |= static_cast<uint8_t>(shm_name[i + 2]);
            b64.push_back(kB64[(v >> 18) & 63]);
            b64.push_back(kB64[(v >> 12) & 63]);
            b64.push_back(i + 1 < shm_name.size() ? kB64[(v >> 6) & 63] : '=');
            b64.push_back(i + 2 < shm_name.size() ? kB64[v & 63] : '=');
        }
        term.feed("\x1b_Ga=t,f=32,t=s,s=1,v=1,i=88;" + b64 + "\x1b\\");
        auto it88 = term.unsafe_graphics().images().find(88);
        CHECK(it88 != term.unsafe_graphics().images().end() &&
                  it88->second.rgba.size() == 4 &&
                  it88->second.rgba[0] == 0x10 && it88->second.rgba[2] == 0x30,
              "Kitty graphics t=s read pixels from POSIX shm");
        // Verify shm object was unlinked by receiver
        int probe = ::shm_open(shm_name.c_str(), O_RDONLY, 0);
        CHECK(probe < 0, "Kitty graphics t=s unlinked POSIX shm object after read");
        if (probe >= 0) {
            ::close(probe);
            ::shm_unlink(shm_name.c_str());
        }
    }
}

static void test_pty_spawn_roundtrip() {
    vater::os::Pty pty;
    CHECK(pty.open(80, 24), "POSIX PTY opens cleanly");

    vater::os::SpawnConfig cfg;
    cfg.program = "/bin/sh";
    cfg.args = {"-c", "printf 'VATER_PTY_OK\\n'"};
    cfg.login_shell = false;
    cfg.term = "xterm-256color";

    vater::os::ChildProcess child;
    CHECK(vater::os::spawn_in_pty(pty, cfg, child), "spawn_in_pty forks & execs /bin/sh");

    std::string captured;
    uint8_t buf[1024];
    for (int iter = 0; iter < 50; ++iter) {
        struct pollfd pfd{pty.master_fd(), POLLIN, 0};
        if (::poll(&pfd, 1, 50) > 0) {
            ssize_t n = pty.read(buf, sizeof(buf));
            if (n > 0) captured.append(reinterpret_cast<const char*>(buf), static_cast<size_t>(n));
            else if (n < 0) break;
        }
        if (child.poll_exit() && captured.find("VATER_PTY_OK") != std::string::npos) break;
    }
    CHECK(captured.find("VATER_PTY_OK") != std::string::npos, "PTY child output received cleanly");
    child.terminate();
}

int main() {
    test_utf8_and_simd();
    test_wcwidth();
    test_scrollback_rle();
    test_grid_reflow();
    test_sixel_and_kitty_graphics();
    test_pty_spawn_roundtrip();

    std::printf("test_vt_unit: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

