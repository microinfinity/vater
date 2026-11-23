/// @file command_palette.cpp
/// @brief L5 UI — Obsidian Glass Command Palette implementation.

#include "ui/command_palette.hpp"

#include "nisaba/canvas/paint.hpp"

#include <algorithm>
#include <cctype>

namespace vater::ui {

namespace {

int fuzzy_score(std::string_view needle, std::string_view haystack) noexcept {
    if (needle.empty()) return 1;
    size_t ni = 0;
    int score = 0;
    int streak = 0;
    for (size_t hi = 0; hi < haystack.size() && ni < needle.size(); ++hi) {
        const char nc = static_cast<char>(std::tolower(static_cast<unsigned char>(needle[ni])));
        const char hc = static_cast<char>(std::tolower(static_cast<unsigned char>(haystack[hi])));
        if (nc == hc) {
            ++ni;
            ++streak;
            score += 10 + streak * 5 + (hi == 0 ? 15 : 0);
        } else {
            streak = 0;
        }
    }
    return (ni == needle.size()) ? score : 0;
}

} // namespace

CommandPalette::CommandPalette() {
    register_commands({
        {"New Tab", "Ctrl+Shift+T", config::Action::NewTab, {}},
        {"Close Pane / Tab", "Ctrl+Shift+W", config::Action::ClosePane, {}},
        {"Split Horizontal (Left / Right)", "Ctrl+Shift+D", config::Action::SplitHorizontal, {}},
        {"Split Vertical (Top / Bottom)", "Ctrl+Shift+E", config::Action::SplitVertical, {}},
        {"Toggle Pane Zoom", "Ctrl+Shift+Z", config::Action::ToggleZoomPane, {}},
        {"Search Scrollback (Regex)", "Ctrl+Shift+F", config::Action::ToggleSearch, {}},
        {"Toggle Arabic BiDi Layout", "Ctrl+Shift+B", config::Action::ToggleBidi, {}},
        {"Toggle OpenType Ligatures", "Ctrl+Shift+L", config::Action::ToggleLigatures, {}},
        {"Toggle Retro CRT Shader", "Ctrl+Shift+G", config::Action::ToggleCrtEffect, {}},
        {"Toggle Markdown / PDF Preview Panel", "Ctrl+Shift+M", config::Action::TogglePreviewPanel, {}},
        {"Start / Stop Asciicast Recording", "", config::Action::ToggleRecording, {}},
        {"Jump to Previous Prompt (OSC 133)", "Ctrl+Alt+Up", config::Action::PrevPrompt, {}},
        {"Jump to Next Prompt (OSC 133)", "Ctrl+Alt+Down", config::Action::NextPrompt, {}},
        {"Increase Font Size", "Ctrl++", config::Action::FontIncrease, {}},
        {"Decrease Font Size", "Ctrl+-", config::Action::FontDecrease, {}},
        {"Reset Font Size", "Ctrl+0", config::Action::FontReset, {}},
        {"Clear Scrollback", "Ctrl+Shift+K", config::Action::ClearScrollback, {}},
        {"Reload Config", "Ctrl+Shift+R", config::Action::ReloadConfig, {}},
    });
}

void CommandPalette::register_commands(std::vector<PaletteCommand> cmds) {
    commands_ = std::move(cmds);
    refilter();
}

void CommandPalette::open() {
    open_ = true;
    query_.clear();
    selected_ = 0;
    refilter();
}

void CommandPalette::close() {
    open_ = false;
    query_.clear();
}

void CommandPalette::refilter() {
    std::vector<std::pair<int, size_t>> scored;
    scored.reserve(commands_.size());
    for (size_t i = 0; i < commands_.size(); ++i) {
        int s = fuzzy_score(query_, commands_[i].title);
        if (s > 0) scored.emplace_back(s, i);
    }
    std::stable_sort(scored.begin(), scored.end(), [](auto a, auto b) { return a.first > b.first; });
    filtered_.clear();
    for (auto [_, idx] : scored) filtered_.push_back(idx);
    if (selected_ >= static_cast<int>(filtered_.size())) {
        selected_ = std::max(0, static_cast<int>(filtered_.size()) - 1);
    }
}

bool CommandPalette::handle_key(int keysym, int /*mods*/, config::Action& out_action) {
    out_action = config::Action::None;
    if (!open_) return false;

    if (keysym == 0xFF1B) { // Escape
        close();
        return true;
    }
    if (keysym == 0xFF52) { // Up
        if (!filtered_.empty()) {
            selected_ = (selected_ - 1 + static_cast<int>(filtered_.size())) % static_cast<int>(filtered_.size());
        }
        return true;
    }
    if (keysym == 0xFF54) { // Down
        if (!filtered_.empty()) {
            selected_ = (selected_ + 1) % static_cast<int>(filtered_.size());
        }
        return true;
    }
    if (keysym == 0xFF0D) { // Enter
        if (selected_ >= 0 && selected_ < static_cast<int>(filtered_.size())) {
            const auto& cmd = commands_[filtered_[static_cast<size_t>(selected_)]];
            out_action = cmd.action;
            auto fn = cmd.custom_fn;
            close();
            if (fn) fn();
        } else {
            close();
        }
        return true;
    }
    if (keysym == 0xFF08) { // BackSpace
        if (!query_.empty()) {
            query_.pop_back();
            refilter();
        }
        return true;
    }
    return true;
}

bool CommandPalette::handle_text(std::string_view utf8) {
    if (!open_) return false;
    if (utf8.empty() || (utf8.size() == 1 && static_cast<uint8_t>(utf8[0]) < 0x20u)) return true;
    query_.append(utf8);
    refilter();
    return true;
}

void CommandPalette::render(nisaba::gpu::GpuCanvas& canvas, float width, float height,
                            const obsidian::ui::AppTheme& theme, const obsidian::ui::UnitScale& scale) {
    if (!open_) return;

    const float u = scale.u;
    const float box_w = std::min(width - 40.0f * u, 500.0f * u);
    const int max_vis = std::min<int>(8, static_cast<int>(filtered_.size()));
    const float item_h = 34.0f * u;
    const float header_h = 44.0f * u;
    const float box_h = header_h + static_cast<float>(std::max(1, max_vis)) * item_h + 12.0f * u;
    const float bx = (width - box_w) * 0.5f;
    const float by = std::max(56.0f * u, height * 0.14f);

    // Subtle dim scrim
    if (auto scrim = nisaba::Rect::from_xywh(0.0f, 0.0f, width, height)) {
        canvas.fill_rect(*scrim, nisaba::Paint(nisaba::Color::from_rgba8(0, 0, 0, 80)));
    }

    auto box = nisaba::Rect::from_xywh(bx, by, box_w, box_h);
    if (!box) return;
    const nisaba::Color panel_bg = obsidian::ui::detail::with_alpha(theme.secondBackground, 0.96f);
    canvas.fill_round_rect(*box, 12.0f * u, 12.0f * u, nisaba::Paint(panel_bg));
    nisaba::Stroke st;
    st.width = 1.0f;
    canvas.stroke_round_rect(*box, 12.0f * u, 12.0f * u, nisaba::Paint(theme.separator), st);

    // Search input line
    const std::string prompt = query_.empty() ? "> Type a command..." : ("> " + query_);
    canvas.draw_text(bx + 16.0f * u, by + 28.0f * u, prompt,
                     nisaba::Paint(query_.empty() ? theme.textFieldPlaceholder : theme.text),
                     14.0f * u, "sans");

    if (auto sep = nisaba::Rect::from_xywh(bx + 10.0f * u, by + header_h, box_w - 20.0f * u, 1.0f)) {
        canvas.fill_rect(*sep, nisaba::Paint(theme.headerSeparator));
    }

    for (int i = 0; i < max_vis; ++i) {
        const auto& cmd = commands_[filtered_[static_cast<size_t>(i)]];
        const float iy = by + header_h + 6.0f * u + static_cast<float>(i) * item_h;
        if (i == selected_) {
            if (auto sel_r = nisaba::Rect::from_xywh(bx + 8.0f * u, iy, box_w - 16.0f * u, item_h - 2.0f * u)) {
                canvas.fill_round_rect(*sel_r, 8.0f * u, 8.0f * u,
                                       nisaba::Paint(obsidian::ui::detail::with_alpha(theme.highlight, 0.25f)));
            }
        }
        canvas.draw_text(bx + 18.0f * u, iy + 22.0f * u, cmd.title,
                         nisaba::Paint(theme.text), 13.0f * u, "sans");
        if (!cmd.shortcut.empty()) {
            canvas.draw_text(bx + box_w - 118.0f * u, iy + 22.0f * u, cmd.shortcut,
                             nisaba::Paint(theme.disabledText), 11.5f * u, "sans");
        }
    }
}

} // namespace vater::ui

