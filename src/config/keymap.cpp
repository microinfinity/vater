/// @file keymap.cpp
/// @brief Vater Default Keybindings & Action Lookup.

#include "config/schema.hpp"

#include <cctype>

namespace vater::config {

namespace {
inline constexpr int kShift = 1 << 0;
inline constexpr int kCtrl  = 1 << 1;
inline constexpr int kAlt   = 1 << 2;

int normalize_keysym(int sym) noexcept {
    if (sym >= 'A' && sym <= 'Z') return sym + ('a' - 'A');
    if (sym == '+') return '=';
    if (sym == '_') return '-';
    return sym;
}
} // namespace

std::vector<KeyBinding> default_keybindings() {
    return {
        {'c', kCtrl | kShift, Action::Copy},
        {'v', kCtrl | kShift, Action::Paste},
        {'t', kCtrl | kShift, Action::NewTab},
        {'w', kCtrl | kShift, Action::ClosePane},
        {0xFF09, kCtrl, Action::NextTab},                   // Ctrl+Tab
        {0xFF09, kCtrl | kShift, Action::PrevTab},          // Ctrl+Shift+Tab
        {0xFE20, kCtrl | kShift, Action::PrevTab},          // ISO_Left_Tab
        {'d', kCtrl | kShift, Action::SplitHorizontal},
        {'e', kCtrl | kShift, Action::SplitVertical},
        {0xFF51, kCtrl | kShift, Action::FocusLeft},
        {0xFF53, kCtrl | kShift, Action::FocusRight},
        {0xFF52, kCtrl | kShift, Action::FocusUp},
        {0xFF54, kCtrl | kShift, Action::FocusDown},
        {'z', kCtrl | kShift, Action::ToggleZoomPane},
        {'=', kCtrl, Action::FontIncrease},
        {'=', kCtrl | kShift, Action::FontIncrease},
        {'-', kCtrl, Action::FontDecrease},
        {'0', kCtrl, Action::FontReset},
        {'f', kCtrl | kShift, Action::ToggleSearch},
        {'p', kCtrl | kShift, Action::ToggleCommandPalette},
        {'b', kCtrl | kShift, Action::ToggleBidi},
        {'l', kCtrl | kShift, Action::ToggleLigatures},
        {'g', kCtrl | kShift, Action::ToggleCrtEffect},
        {'k', kCtrl | kShift, Action::ClearScrollback},
        {'r', kCtrl | kShift, Action::ReloadConfig},
        {'m', kCtrl | kShift, Action::TogglePreviewPanel},
        {0xFF55, kShift, Action::ScrollPageUp},             // Shift+PageUp
        {0xFF56, kShift, Action::ScrollPageDown},           // Shift+PageDown
        {0xFF52, kCtrl | kAlt, Action::PrevPrompt},         // Ctrl+Alt+Up (OSC 133)
        {0xFF54, kCtrl | kAlt, Action::NextPrompt},         // Ctrl+Alt+Down (OSC 133)
    };
}

Action lookup_keybinding(const std::vector<KeyBinding>& bindings, int keysym, int mods) noexcept {
    const int norm = normalize_keysym(keysym);
    for (const auto& kb : bindings) {
        if (normalize_keysym(kb.keysym) == norm && kb.mods == mods) {
            return kb.action;
        }
    }
    return Action::None;
}

Action parse_action_name(std::string_view name) noexcept {
    if (name == "copy") return Action::Copy;
    if (name == "paste") return Action::Paste;
    if (name == "new_tab") return Action::NewTab;
    if (name == "close_pane") return Action::ClosePane;
    if (name == "next_tab") return Action::NextTab;
    if (name == "prev_tab") return Action::PrevTab;
    if (name == "split_horizontal") return Action::SplitHorizontal;
    if (name == "split_vertical") return Action::SplitVertical;
    if (name == "focus_left") return Action::FocusLeft;
    if (name == "focus_right") return Action::FocusRight;
    if (name == "focus_up") return Action::FocusUp;
    if (name == "focus_down") return Action::FocusDown;
    if (name == "toggle_zoom") return Action::ToggleZoomPane;
    if (name == "font_increase") return Action::FontIncrease;
    if (name == "font_decrease") return Action::FontDecrease;
    if (name == "font_reset") return Action::FontReset;
    if (name == "search" || name == "toggle_search") return Action::ToggleSearch;
    if (name == "command_palette" || name == "toggle_command_palette") return Action::ToggleCommandPalette;
    if (name == "toggle_bidi") return Action::ToggleBidi;
    if (name == "toggle_ligatures") return Action::ToggleLigatures;
    if (name == "toggle_crt") return Action::ToggleCrtEffect;
    if (name == "scroll_page_up") return Action::ScrollPageUp;
    if (name == "scroll_page_down") return Action::ScrollPageDown;
    if (name == "scroll_to_bottom") return Action::ScrollToBottom;
    if (name == "prev_prompt") return Action::PrevPrompt;
    if (name == "next_prompt") return Action::NextPrompt;
    if (name == "reload_config") return Action::ReloadConfig;
    if (name == "toggle_quake") return Action::ToggleQuake;
    if (name == "toggle_preview") return Action::TogglePreviewPanel;
    if (name == "toggle_recording") return Action::ToggleRecording;
    if (name == "clear_scrollback") return Action::ClearScrollback;
    return Action::None;
}

} // namespace vater::config

