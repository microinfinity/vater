#pragma once
/// @file palette.hpp
/// @brief L4 Renderer — 16-Color Obsidian-Derived ANSI Palette + 256-Color Cube/Grayscale + Truecolor Resolver.

#include "theme.hpp"
#include "vt/cell.hpp"
#include "vt/dispatch.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace vater::render {

/// Complete 256-color terminal palette harmoniously derived from `obsidian::ui::AppTheme`
/// (Dark, Light, or Live Monet) with optional user overrides and dynamic OSC 4/10/11/12 overrides.
struct TerminalPalette {
    nisaba::Color default_fg{nisaba::Color::from_rgba8(232, 232, 238, 255)};
    nisaba::Color default_bg{nisaba::Color::from_rgba8(28, 28, 29, 255)};
    nisaba::Color cursor_color{nisaba::Color::from_rgba8(51, 133, 255, 255)};
    nisaba::Color selection_bg{nisaba::Color::from_rgba8(51, 133, 255, 110)};
    nisaba::Color search_match_bg{nisaba::Color::from_rgba8(254, 160, 66, 135)};
    nisaba::Color search_current_bg{nisaba::Color::from_rgba8(254, 160, 66, 225)};
    std::array<nisaba::Color, 256> colors{};

    /// Derives all 256 colors from `theme`, applying `user_override` (16-color config)
    /// and `dynamic` (OSC 4/10/11/12 runtime overrides).
    static TerminalPalette from_theme(const obsidian::ui::AppTheme& theme,
                                      const std::array<std::optional<uint32_t>, 16>& user_override,
                                      const vt::DynamicColorState& dynamic) noexcept {
        TerminalPalette pal{};
        pal.default_fg = theme.text;
        pal.default_bg = theme.background;
        pal.cursor_color = theme.highlight;
        pal.selection_bg = obsidian::ui::detail::with_alpha(theme.highlight, theme.darkMode ? 0.38f : 0.28f);

        auto rgb = [](uint32_t hex) noexcept {
            return nisaba::Color::from_rgba8(
                static_cast<uint8_t>((hex >> 16) & 0xFFu),
                static_cast<uint8_t>((hex >> 8) & 0xFFu),
                static_cast<uint8_t>(hex & 0xFFu),
                255);
        };

        if (theme.darkMode) {
            // Sovereign Dark / Live Monet-harmonized 16-color ANSI palette
            pal.colors[0]  = obsidian::ui::detail::lerp_color(theme.background, rgb(0x16161A), 0.55f);
            pal.colors[1]  = rgb(0xFF5C6D); // Red (Obsidian accent)
            pal.colors[2]  = rgb(0x35BF56); // Green
            pal.colors[3]  = rgb(0xFEA042); // Yellow/Orange
            pal.colors[4]  = obsidian::ui::detail::lerp_color(theme.highlight, rgb(0x4C9AFF), 0.45f);
            pal.colors[5]  = rgb(0xA68CFF); // Magenta
            pal.colors[6]  = rgb(0x4FD6BE); // Cyan
            pal.colors[7]  = obsidian::ui::detail::lerp_color(theme.text, rgb(0xC8CCD4), 0.35f);

            pal.colors[8]  = rgb(0x636B7A); // Bright Black
            pal.colors[9]  = rgb(0xFF7A88);
            pal.colors[10] = rgb(0x52DF74);
            pal.colors[11] = rgb(0xFFBD6E);
            pal.colors[12] = obsidian::ui::detail::lerp_color(theme.highlight, rgb(0x7AB8FF), 0.50f);
            pal.colors[13] = rgb(0xC4B0FF);
            pal.colors[14] = rgb(0x7CEEE0);
            pal.colors[15] = rgb(0xFFFFFF);
        } else {
            // Sovereign Light Mode high-contrast 16-color ANSI palette
            pal.colors[0]  = rgb(0x28282E);
            pal.colors[1]  = rgb(0xD92D20);
            pal.colors[2]  = rgb(0x1D8F3E);
            pal.colors[3]  = rgb(0xB86200);
            pal.colors[4]  = rgb(0x1E66D0);
            pal.colors[5]  = rgb(0x7B3FE4);
            pal.colors[6]  = rgb(0x0E8587);
            pal.colors[7]  = rgb(0x686872);

            pal.colors[8]  = rgb(0x5A5A64);
            pal.colors[9]  = rgb(0xEE453A);
            pal.colors[10] = rgb(0x24A84A);
            pal.colors[11] = rgb(0xD97706);
            pal.colors[12] = rgb(0x3385FF);
            pal.colors[13] = rgb(0x9359FF);
            pal.colors[14] = rgb(0x13A4A8);
            pal.colors[15] = rgb(0x141418);
        }

        // Apply user config 16-color overrides
        for (size_t i = 0; i < 16; ++i) {
            if (user_override[i]) pal.colors[i] = rgb(*user_override[i]);
        }

        // Standard 6x6x6 xterm color cube (16..231)
        static constexpr uint8_t kLevels[6] = {0, 95, 135, 175, 215, 255};
        for (int i = 0; i < 216; ++i) {
            const uint8_t r = kLevels[i / 36];
            const uint8_t g = kLevels[(i / 6) % 6];
            const uint8_t b = kLevels[i % 6];
            pal.colors[static_cast<size_t>(16 + i)] = nisaba::Color::from_rgba8(r, g, b, 255);
        }

        // Standard 24-step grayscale ramp (232..255)
        for (int i = 0; i < 24; ++i) {
            const uint8_t v = static_cast<uint8_t>(8 + i * 10);
            pal.colors[static_cast<size_t>(232 + i)] = nisaba::Color::from_rgba8(v, v, v, 255);
        }

        // Apply dynamic runtime OSC 4 / 10 / 11 / 12 overrides
        for (size_t i = 0; i < 256; ++i) {
            if (dynamic.palette[i]) pal.colors[i] = rgb(*dynamic.palette[i]);
        }
        if (dynamic.default_fg) pal.default_fg = rgb(*dynamic.default_fg);
        if (dynamic.default_bg) pal.default_bg = rgb(*dynamic.default_bg);
        if (dynamic.cursor_color) pal.cursor_color = rgb(*dynamic.cursor_color);

        return pal;
    }

    [[nodiscard]] nisaba::Color resolve_fg(uint32_t encoded, bool bold, bool dim) const noexcept {
        nisaba::Color c = default_fg;
        if (vt::is_color_indexed(encoded)) {
            uint8_t idx = vt::color_index(encoded);
            if (bold && idx < 8) idx = static_cast<uint8_t>(idx + 8);
            c = colors[idx];
        } else if (vt::is_color_rgb(encoded)) {
            c = nisaba::Color::from_rgba8(vt::color_r(encoded), vt::color_g(encoded), vt::color_b(encoded), 255);
        }
        if (dim) {
            c = obsidian::ui::detail::with_alpha(c, 0.58f);
        }
        return c;
    }

    [[nodiscard]] nisaba::Color resolve_bg(uint32_t encoded) const noexcept {
        if (vt::is_color_indexed(encoded)) {
            return colors[vt::color_index(encoded)];
        }
        if (vt::is_color_rgb(encoded)) {
            return nisaba::Color::from_rgba8(vt::color_r(encoded), vt::color_g(encoded), vt::color_b(encoded), 255);
        }
        return default_bg;
    }
};

} // namespace vater::render

