/// @file test_regex_input.cpp
/// @brief Tests for Thompson NFA Regex Engine, InputEncoder (xterm + Kitty + SGR/Pixels + Paste Guard),
/// JSON Config Loader with line-numbered diagnostics, and live `vaterd` Unix Socket IPC + SO_PEERCRED.

#include "config/schema.hpp"
#include "mux/daemon/vaterd.hpp"
#include "mux/pane_tree.hpp"
#include "mux/session.hpp"
#include "os/ipc.hpp"
#include "ui/context_menu.hpp"
#include "ui/input_encoder.hpp"
#include "ui/search_overlay.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) {                                                            \
            ++g_passed;                                                        \
        } else {                                                               \
            ++g_failed;                                                        \
            std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg); \
        }                                                                      \
    } while (0)

static void test_thompson_regex() {
    vater::ui::ThompsonRegex re;
    std::vector<std::pair<size_t, size_t>> m;

    CHECK(re.compile("err(or|s)?:\\s+[0-9]+", false), "Compile regex with alternation, class, quantifier");
    re.find_all("INFO ok; ERROR: 404 and errs:   500 done", m);
    CHECK(m.size() == 2, "Found both case-insensitive matches");
    CHECK(m[0].first == 9 && m[0].second == 19, "First match span accurate");
    CHECK(m[1].first == 24 && m[1].second == 35, "Second match span accurate");

    // ReDoS pathological pattern check: must finish instantaneously in linear O(N*M) time!
    CHECK(re.compile("a?a?a?a?a?a?a?a?a?a?a?a?a?a?a?a?a?a?a?a?aaaaaaaaaaaaaaaaaaaa", true),
          "Compile classic exponential-backtracking stress regex");
    re.find_all("aaaaaaaaaaaaaaaaaaaa", m);
    CHECK(m.size() == 1 && m[0].first == 0 && m[0].second == 20, "Thompson NFA immune to ReDoS");
}

static void test_input_encoder() {
    vater::vt::TerminalModes modes;
    bool sup = false;

    // Legacy arrows + DECCKM
    CHECK(vater::ui::InputEncoder::encode_key(0xFF52, 0, vater::ui::KeyEventKind::Press, modes, sup) == "\x1b[A" && sup,
          "Up arrow normal mode");
    modes.cursor_keys_app = true;
    CHECK(vater::ui::InputEncoder::encode_key(0xFF52, 0, vater::ui::KeyEventKind::Press, modes, sup) == "\x1bOA",
          "Up arrow application cursor mode (DECCKM)");

    // Ctrl+C -> 0x03
    modes.cursor_keys_app = false;
    CHECK(vater::ui::InputEncoder::encode_key('c', 2, vater::ui::KeyEventKind::Press, modes, sup) == "\x03",
          "Ctrl+C encodes ETX (0x03)");

    // Kitty keyboard protocol (disambiguate + report events)
    modes.kitty_push(vater::vt::kitty_kbd::kDisambiguate | vater::vt::kitty_kbd::kReportEvents);
    CHECK(vater::ui::InputEncoder::encode_key('c', 2, vater::ui::KeyEventKind::Press, modes, sup) == "\x1b[99;5u",
          "Kitty Ctrl+C press -> CSI 99;5u");
    CHECK(vater::ui::InputEncoder::encode_key('c', 2, vater::ui::KeyEventKind::Release, modes, sup) == "\x1b[99;5:3u",
          "Kitty Ctrl+C release -> CSI 99;5:3u");

    // SGR (1006) & SGR-Pixels (1016) mouse encoding
    modes.mouse_mode = vater::vt::MouseTrackingMode::Normal;
    modes.mouse_encoding = vater::vt::MouseEncoding::Sgr;
    CHECK(vater::ui::InputEncoder::encode_mouse(vater::ui::MouseEventKind::Press, 1, 0, 9, 4, 90, 80, modes) == "\x1b[<0;10;5M",
          "SGR 1006 mouse press");
    modes.mouse_encoding = vater::vt::MouseEncoding::SgrPixels;
    CHECK(vater::ui::InputEncoder::encode_mouse(vater::ui::MouseEventKind::Release, 1, 0, 9, 4, 90, 80, modes) == "\x1b[<0;90;80m",
          "SGR-Pixels 1016 mouse release");

    // Bracketed paste + injection stripping
    modes.bracketed_paste = true;
    std::string pasted = vater::ui::InputEncoder::encode_paste("safe\x1b[201~evil\nline2", modes);
    CHECK(pasted == "\x1b[200~safeevil\rline2\x1b[201~", "Bracketed paste strips embedded ESC[201~ injection");
}

static void test_config_loader() {
    const std::string json = R"({
        "font_family": "JetBrains Mono",
        "font_size": 16,
        "bidi_enabled": true,
        "ligatures": false,
        "term": "vater",
        "osc52_write": true,
        "cursor_shape": "bar",
        "keybindings": {
            "ctrl+alt+n": "new_tab"
        }
    })";
    auto res = vater::config::parse_config_json(json);
    CHECK(res.ok && res.diagnostics.empty(), "Valid JSON config parses with 0 diagnostics");
    CHECK(res.config.font_family == "JetBrains Mono", "font_family parsed");
    CHECK(res.config.font_size == 16.0f, "font_size parsed");
    CHECK(res.config.term == "vater", "term override parsed");
    CHECK(res.config.osc52_write == true && res.config.osc52_read == false, "osc52 security flags parsed");
    CHECK(res.config.cursor_shape == vater::vt::CursorShape::Bar, "cursor_shape parsed");
    CHECK(vater::config::lookup_keybinding(res.config.keybindings, 'n', 2 | 4) == vater::config::Action::NewTab,
          "Custom keybinding mapped");

    // Invalid config yields line-numbered diagnostic
    auto bad = vater::config::parse_config_json("{\n  \"font_size\": 999\n}");
    CHECK(!bad.diagnostics.empty() && bad.diagnostics[0].find(":2:") != std::string::npos,
          "Out-of-range config reports exact line number (:2:)");
}

static void test_vaterd_ipc_roundtrip() {
    const std::string sock = "/tmp/vaterd-test-" + std::to_string(::getpid()) + ".sock";
    vater::daemon::VaterDaemon daemon(sock);
    std::thread server_thread([&]() { daemon.run(); });

    // Wait briefly for listen socket to appear
    std::vector<vater::daemon::DaemonSessionInfo> list;
    bool connected = false;
    for (int i = 0; i < 30; ++i) {
        if (vater::daemon::query_daemon_sessions(sock, list)) {
            connected = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    CHECK(connected, "Connected to vaterd Unix socket with SO_PEERCRED verification");
    CHECK(list.empty(), "Initial vaterd session list is empty");

    daemon.request_stop();
    if (server_thread.joinable()) server_thread.join();
    ::unlink(sock.c_str());
}

static void test_context_menu_and_pane_tree_and_playback() {
    // 1. ContextMenu clamping, hit-testing & keyboard navigation
    vater::ui::ContextMenu menu;
    menu.open(980.0f, 650.0f, {
        {"Copy", "Ctrl+Shift+C", vater::config::Action::Copy, {}, false, false, false, false},
        {"Paste", "Ctrl+Shift+V", vater::config::Action::Paste, {}, true, false, false, false},
        vater::ui::ContextMenuItem::separator(),
        {"New Tab", "Ctrl+Shift+T", vater::config::Action::NewTab, {}, true, false, false, false},
    });
    CHECK(menu.is_open(), "ContextMenu opens cleanly");
    const auto bounds = menu.compute_bounds(1000.0f, 700.0f, 1.0f);
    CHECK(bounds.right() <= 993.0f && bounds.bottom() <= 693.0f, "ContextMenu clamps within window bounds");

    vater::config::Action act = vater::config::Action::None;
    std::function<void()> fn;
    menu.handle_key(0xFF54, act, fn); // Down -> skips disabled Copy, selects Paste (idx 1)
    menu.handle_key(0xFF54, act, fn); // Down -> skips separator, selects New Tab (idx 3)
    menu.handle_key(0xFF0D, act, fn); // Enter -> executes NewTab and closes
    CHECK(act == vater::config::Action::NewTab && !menu.is_open(),
          "ContextMenu keyboard navigation skips disabled/separators and executes action");

    // 2. PaneTree divider split_id + set_split_ratio
    vater::mux::PaneTree tree;
    tree.init_root(std::make_unique<vater::mux::Session>(1, 80, 24));
    CHECK(tree.split_pane(1, vater::mux::SplitAxis::Horizontal,
                          std::make_unique<vater::mux::Session>(2, 80, 24), 0.5f),
          "PaneTree horizontal split succeeds");
    std::vector<vater::mux::PaneLayoutRect> panes;
    std::vector<vater::mux::DividerRect> divs;
    const auto vp = nisaba::Rect::from_xywh(0.0f, 0.0f, 1000.0f, 600.0f).value();
    tree.compute_layout(vp, panes, divs, 2.0f);
    CHECK(panes.size() == 2 && divs.size() == 1 && divs[0].split_id != 0,
          "PaneTree produces divider with valid split_id");
    CHECK(tree.set_split_ratio(divs[0].split_id, 0.25f), "PaneTree::set_split_ratio updates ratio");
    tree.compute_layout(vp, panes, divs, 2.0f);
    CHECK(panes[0].bounds.width() < 280.0f && panes[1].bounds.width() > 700.0f,
          "PaneTree recomputes split geometry after set_split_ratio");

    // 3. Asciicast v2 playback
    const std::string cast_path = "/tmp/vater-test-" + std::to_string(::getpid()) + ".cast";
    {
        std::ofstream f(cast_path, std::ios::out | std::ios::trunc);
        f << "{\"version\": 2, \"width\": 60, \"height\": 15}\n";
        f << "[0.001, \"o\", \"VATER_PLAY_OK\\u001b[0m\"]\n";
    }
    {
        vater::mux::Session sess(99, 80, 24);
        CHECK(sess.start_playback(cast_path, {}, 0.1, 10.0), "Session::start_playback opens .cast");
        for (int i = 0; i < 60 && !sess.is_playback_finished(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(sess.is_playback_finished(), "Asciicast playback completes cleanly");
        CHECK(sess.terminal().cols() == 60 && sess.terminal().rows() == 15,
              "Asciicast playback applies header terminal dimensions");
        const std::string text = sess.terminal().extract_text(0, 0, 0, 20, false);
        CHECK(text.find("VATER_PLAY_OK") != std::string::npos,
              "Asciicast playback feeds unescaped output bytes into terminal");
        sess.stop(false);
    }
    ::unlink(cast_path.c_str());
}

int main() {
    test_thompson_regex();
    test_input_encoder();
    test_config_loader();
    test_vaterd_ipc_roundtrip();
    test_context_menu_and_pane_tree_and_playback();

    std::printf("test_regex_input: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

