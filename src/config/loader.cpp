/// @file loader.cpp
/// @brief Vater Configuration Loader using nisaba::json with line-numbered diagnostics & inotify hot-reload.

#include "config/schema.hpp"
#include "nisaba/json/json.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vater::config {

namespace {

size_t find_key_line(std::string_view src, std::string_view key) {
    std::string needle = "\"" + std::string(key) + "\"";
    const size_t pos = src.find(needle);
    if (pos == std::string_view::npos) return 1;
    size_t line = 1;
    for (size_t i = 0; i < pos; ++i) {
        if (src[i] == '\n') ++line;
    }
    return line;
}

std::optional<uint32_t> parse_hex_color(std::string_view s) {
    if (!s.empty() && s[0] == '#') s.remove_prefix(1);
    if (s.size() != 6) return std::nullopt;
    uint32_t val = 0;
    for (char c : s) {
        uint32_t d = 0;
        if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(10 + c - 'a');
        else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(10 + c - 'A');
        else return std::nullopt;
        val = (val << 4) | d;
    }
    return val;
}

bool parse_shortcut_string(std::string_view combo, int& out_keysym, int& out_mods) {
    out_keysym = 0;
    out_mods = 0;
    size_t pos = 0;
    while (pos < combo.size()) {
        size_t plus = combo.find('+', pos);
        if (plus == std::string_view::npos) plus = combo.size();
        std::string tok;
        for (size_t i = pos; i < plus; ++i) {
            tok.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(combo[i]))));
        }
        pos = plus + 1;

        if (tok == "shift") out_mods |= 1;
        else if (tok == "ctrl" || tok == "control") out_mods |= 2;
        else if (tok == "alt" || tok == "mod1") out_mods |= 4;
        else if (tok == "super" || tok == "meta") out_mods |= 8;
        else if (tok.size() == 1) out_keysym = static_cast<unsigned char>(tok[0]);
        else if (tok == "pageup" || tok == "page_up") out_keysym = 0xFF55;
        else if (tok == "pagedown" || tok == "page_down") out_keysym = 0xFF56;
        else if (tok == "left") out_keysym = 0xFF51;
        else if (tok == "up") out_keysym = 0xFF52;
        else if (tok == "right") out_keysym = 0xFF53;
        else if (tok == "down") out_keysym = 0xFF54;
        else if (tok == "tab") out_keysym = 0xFF09;
        else if (tok == "return" || tok == "enter") out_keysym = 0xFF0D;
        else if (tok == "equal" || tok == "plus") out_keysym = '=';
        else if (tok == "minus") out_keysym = '-';
        else if (tok == "f12") out_keysym = 0xFFC9;
    }
    return out_keysym != 0;
}

} // namespace

std::string default_config_path() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && xdg[0] != '\0') {
        return std::string(xdg) + "/vater/config.json";
    }
    if (const char* home = std::getenv("HOME"); home && home[0] != '\0') {
        return std::string(home) + "/.config/vater/config.json";
    }
    return "config.json";
}

ConfigLoadResult parse_config_json(std::string_view json_text) {
    ConfigLoadResult res;
    res.config.keybindings = default_keybindings();

    auto parsed = nisaba::json::JsonValue::parse(json_text);
    if (!parsed || !parsed->is_object()) {
        res.ok = false;
        res.diagnostics.push_back("config.json:1: invalid JSON syntax or root is not an object");
        return res;
    }

    const auto& root = *parsed;
    auto& c = res.config;

    auto diag = [&](std::string_view key, std::string_view msg) {
        const size_t line = find_key_line(json_text, key);
        res.diagnostics.push_back("config.json:" + std::to_string(line) + ": [" +
                                  std::string(key) + "] " + std::string(msg));
    };

    if (const auto* v = root.get("font_family")) {
        if (v->is_string() && !v->as_string().empty()) c.font_family = std::string(v->as_string());
        else diag("font_family", "expected non-empty string");
    }
    if (const auto* v = root.get("fallback_fonts")) {
        if (v->is_array()) {
            c.fallback_fonts.clear();
            for (const auto& item : v->as_array()) {
                if (item.is_string() && !item.as_string().empty()) {
                    c.fallback_fonts.emplace_back(item.as_string());
                }
            }
        } else {
            diag("fallback_fonts", "expected array of font family names");
        }
    }
    if (const auto* v = root.get("font_size")) {
        if (v->is_number()) {
            const float fs = v->to_float(14.0f);
            if (fs >= 6.0f && fs <= 96.0f) c.font_size = fs;
            else diag("font_size", "must be between 6 and 96");
        } else {
            diag("font_size", "expected number");
        }
    }
    if (const auto* v = root.get("line_height")) {
        if (v->is_number()) c.line_height = std::clamp(v->to_float(1.15f), 0.8f, 2.5f);
        else diag("line_height", "expected number");
    }
    if (const auto* v = root.get("ligatures")) c.ligatures = v->to_bool(true);
    if (const auto* v = root.get("bidi_enabled")) c.bidi_enabled = v->to_bool(true);
    if (const auto* v = root.get("shell")) {
        if (v->is_string()) c.shell = std::string(v->as_string());
    }
    if (const auto* v = root.get("term")) {
        if (v->is_string() && !v->as_string().empty()) c.term = std::string(v->as_string());
        else diag("term", "expected non-empty string (e.g. \"xterm-256color\" or \"vater\")");
    }
    if (const auto* v = root.get("scrollback_lines")) {
        if (v->is_number()) c.scrollback_lines = static_cast<size_t>(std::clamp(v->to_int(100000), 0, 2000000));
        else diag("scrollback_lines", "expected integer");
    }
    if (const auto* v = root.get("scrollback_mmap_spill")) c.scrollback_mmap_spill = v->to_bool(false);
    if (const auto* v = root.get("word_delimiters")) {
        if (v->is_string()) c.word_delimiters = std::string(v->as_string());
    }
    if (const auto* v = root.get("cursor_shape")) {
        const auto s = v->to_string("block");
        if (s == "block") c.cursor_shape = vt::CursorShape::Block;
        else if (s == "underline") c.cursor_shape = vt::CursorShape::Underline;
        else if (s == "bar" || s == "beam") c.cursor_shape = vt::CursorShape::Bar;
        else diag("cursor_shape", "expected \"block\", \"underline\", or \"bar\"");
    }
    if (const auto* v = root.get("cursor_blink")) c.cursor_blink = v->to_bool(true);
    if (const auto* v = root.get("cursor_blink_ms")) {
        c.cursor_blink_ms = static_cast<uint32_t>(std::clamp(v->to_int(530), 100, 5000));
    }
    if (const auto* v = root.get("opacity")) {
        if (v->is_number()) c.opacity = std::clamp(v->to_float(0.88f), 0.1f, 1.0f);
        else diag("opacity", "expected number in [0.1, 1.0]");
    }
    if (const auto* v = root.get("follow_system_theme")) c.follow_system_theme = v->to_bool(true);
    if (const auto* v = root.get("blur_behind")) c.blur_behind = v->to_bool(true);
    if (const auto* v = root.get("crt_effect")) c.crt_effect = v->to_bool(false);
    if (const auto* v = root.get("software_renderer")) c.software_renderer = v->to_bool(false);
    if (const auto* v = root.get("osc52_write")) c.osc52_write = v->to_bool(false);
    if (const auto* v = root.get("osc52_read")) c.osc52_read = v->to_bool(false);
    if (const auto* v = root.get("confirm_multiline_paste")) c.confirm_multiline_paste = v->to_bool(true);
    if (const auto* v = root.get("padding_x")) c.padding_x = std::clamp(v->to_float(10.0f), 0.0f, 64.0f);
    if (const auto* v = root.get("padding_y")) c.padding_y = std::clamp(v->to_float(8.0f), 0.0f, 64.0f);

    if (const auto* pal = root.get("palette")) {
        if (pal->is_array()) {
            const auto& arr = pal->as_array();
            for (size_t i = 0; i < std::min<size_t>(16, arr.size()); ++i) {
                if (arr[i].is_string()) {
                    if (auto rgb = parse_hex_color(arr[i].as_string())) {
                        c.palette_override[i] = *rgb;
                    } else {
                        diag("palette", "invalid #RRGGBB color at index " + std::to_string(i));
                    }
                }
            }
        } else {
            diag("palette", "expected array of up to 16 #RRGGBB strings");
        }
    }

    if (const auto* km = root.get("keybindings")) {
        if (km->is_object()) {
            for (const auto& [combo, act_val] : km->as_object()) {
                int sym = 0, mods = 0;
                const Action act = parse_action_name(act_val.to_string());
                if (act == Action::None) {
                    diag("keybindings", "unknown action \"" + std::string(act_val.to_string()) + "\"");
                    continue;
                }
                if (!parse_shortcut_string(combo, sym, mods)) {
                    diag("keybindings", "invalid shortcut \"" + combo + "\"");
                    continue;
                }
                // Override existing or append
                bool replaced = false;
                for (auto& kb : c.keybindings) {
                    if (kb.keysym == sym && kb.mods == mods) {
                        kb.action = act;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) c.keybindings.push_back(KeyBinding{sym, mods, act});
            }
        } else {
            diag("keybindings", "expected object mapping shortcut -> action");
        }
    }

    res.ok = true;
    return res;
}

ConfigLoadResult load_config_file(const std::string& path) {
    ConfigLoadResult res;
    res.config.keybindings = default_keybindings();

    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        // Missing config file is normal — use sovereign defaults
        return res;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return parse_config_json(oss.str());
}

ConfigWatcher::~ConfigWatcher() {
    stop();
}

bool ConfigWatcher::watch(const std::string& config_path) {
    stop();
    const size_t slash = config_path.find_last_of('/');
    const std::string dir = (slash == std::string::npos) ? "." : config_path.substr(0, slash);
    filename_ = (slash == std::string::npos) ? config_path : config_path.substr(slash + 1);

    inotify_fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd_ < 0) return false;

    watch_wd_ = ::inotify_add_watch(inotify_fd_, dir.c_str(),
                                    IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
    if (watch_wd_ < 0) {
        ::close(inotify_fd_);
        inotify_fd_ = -1;
        return false;
    }
    return true;
}

void ConfigWatcher::stop() noexcept {
    if (inotify_fd_ >= 0) {
        if (watch_wd_ >= 0) ::inotify_rm_watch(inotify_fd_, watch_wd_);
        ::close(inotify_fd_);
    }
    inotify_fd_ = -1;
    watch_wd_ = -1;
}

bool ConfigWatcher::poll_changed() {
    if (inotify_fd_ < 0) return false;
    alignas(struct inotify_event) char buf[4096];
    bool changed = false;
    for (;;) {
        ssize_t n = ::read(inotify_fd_, buf, sizeof(buf));
        if (n <= 0) break;
        ssize_t off = 0;
        while (off < n) {
            const auto* ev = reinterpret_cast<const struct inotify_event*>(buf + off);
            if (ev->len > 0 && filename_ == ev->name) {
                changed = true;
            }
            off += static_cast<ssize_t>(sizeof(struct inotify_event) + ev->len);
        }
    }
    return changed;
}

} // namespace vater::config

