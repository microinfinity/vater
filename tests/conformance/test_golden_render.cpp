/// @file test_golden_render.cpp
/// @brief Headless Software (CPU) Golden Pixel Render Verification (§9).
/// Renders ASCII, truecolor backgrounds, procedural Box-Drawing, and Arabic BiDi contextual text
/// onto a `nisaba::Pixmap` via `CpuTerminalRenderer` and verifies pixel outputs deterministically.

#include "nisaba/canvas/canvas.hpp"
#include "nisaba/canvas/pixmap.hpp"
#include "nisaba/text/font_system.hpp"
#include "render/cpu_renderer.hpp"
#include "render/font_stack.hpp"
#include "render/palette.hpp"
#include "theme.hpp"
#include "vt/terminal.hpp"

#include <cstdio>

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) {                                                            \
            ++g_passed;                                                        \
        } else {                                                               \
            ++g_failed;                                                        \
            std::fprintf(stderr, "GOLDEN FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg); \
        }                                                                      \
    } while (0)

static size_t count_non_bg_pixels(const nisaba::Pixmap& pm, uint8_t bg_r, uint8_t bg_g, uint8_t bg_b) {
    size_t count = 0;
    const auto ref = pm.as_ref();
    for (uint32_t y = 0; y < ref.height(); ++y) {
        const uint8_t* row = ref.row_raw(y);
        for (uint32_t x = 0; x < ref.width(); ++x) {
            const uint8_t r = row[x * 4 + 0];
            const uint8_t g = row[x * 4 + 1];
            const uint8_t b = row[x * 4 + 2];
            if (std::abs(int(r) - int(bg_r)) > 4 ||
                std::abs(int(g) - int(bg_g)) > 4 ||
                std::abs(int(b) - int(bg_b)) > 4) {
                ++count;
            }
        }
    }
    return count;
}

int main() {
    // Regression test: even when FontSystem already has 1 non-monospace UI font loaded (like main.cpp "sans"),
    // FontStack::init() MUST still scan system fonts and pick a true monospace font!
    nisaba::text::FontSystem font_sys;
    (void)font_sys.load_font_file("/usr/share/obsidian_shell/fonts/Inter-Regular.ttf", 0, "sans");
    vater::render::FontStack fonts;
    fonts.init(font_sys, "DejaVu Sans Mono", {"Noto Sans Mono", "Cascadia Mono", "Noto Sans Arabic"}, 14.0f, 1.15f);

    const auto pface = fonts.primary_face(false, false);
    CHECK(pface.font != nullptr, "Primary monospace font resolved");
    if (pface.font) {
        const float adv_i = pface.font->glyph_advance(pface.font->glyph_index(U'i'), 14.0f);
        const float adv_m = pface.font->glyph_advance(pface.font->glyph_index(U'M'), 14.0f);
        CHECK(adv_i > 4.0f && std::abs(adv_i - adv_m) < 0.25f,
              "Primary terminal font is strictly monospace (adv('i') == adv('M'))");
    }

    const auto theme = obsidian::ui::AppTheme::dark();
    std::array<std::optional<uint32_t>, 16> no_ov{};
    const auto pal = vater::render::TerminalPalette::from_theme(theme, no_ov, vater::vt::DynamicColorState{});
    const auto bg_u8 = pal.default_bg.to_color_u8();

    vater::vt::Terminal term(40, 8);
    // Feed ASCII + truecolor bg + procedural box-drawing + Arabic BiDi text!
    term.feed("\x1b[48;2;200;40;60mRED_BG\x1b[0m\r\n");
    term.feed("\xE2\x94\x8C\xE2\x94\x80\xE2\x94\x80\xE2\x94\x90\r\n"); // ┌──┐
    term.feed("\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 \xD8\xA8\xD8\xA7\xD9\x84\xD8\xB9\xD8\xA7\xD9\x84\xD9\x85\r\n"); // مرحبا بالعالم

    vater::vt::ViewportSnapshot snap;
    CHECK(term.snapshot(snap), "Snapshot captured for golden CPU render");

    const uint32_t w = 420;
    const uint32_t h = 200;
    auto pm_opt = nisaba::Pixmap::create(w, h);
    CHECK(pm_opt.has_value(), "Created 420x200 software Pixmap");

    nisaba::Canvas sw_canvas(*pm_opt);
    vater::render::CpuTerminalRenderer renderer;
    vater::render::RenderOverlayState ov{};
    ov.background_alpha = 1.0f;
    ov.pane_focused = true;
    ov.cursor_blink_on = true;

    const auto bounds = nisaba::Rect::from_xywh(0.0f, 0.0f, float(w), float(h)).value();
    renderer.render_pane(sw_canvas, bounds, snap, term, fonts, pal, ov);

    // Verify truecolor red background pixel at top-left cell (2, 2)
    const uint8_t* p0 = pm_opt->as_ref().row_raw(2) + 2 * 4;
    CHECK(p0[0] > 180 && p0[1] < 60 && p0[2] < 80, "Truecolor red cell background rasterized accurately");

    // Verify procedural box-drawing & Arabic/ASCII glyphs painted significant foreground pixels
    const size_t lit = count_non_bg_pixels(*pm_opt, bg_u8.r, bg_u8.g, bg_u8.b);
    CHECK(lit > 800, "Golden CPU render produced non-trivial rasterized text + box drawing + Arabic BiDi pixels");

    // Verify BiDi shaping produces RTL visual mapping on Arabic row (row 2)
    vater::render::ShapedLine ar_shaped;
    vater::render::LineShaper::shape_line(snap.lines[2], fonts, true, true, ar_shaped);
    CHECK(ar_shaped.has_rtl, "LineShaper detected Arabic RTL line");
    CHECK(!ar_shaped.glyphs.empty(), "LineShaper produced shaped Arabic glyphs");
    CHECK(ar_shaped.glyphs.front().visual_col > 20,
          "Implicit BiDi right-aligned pure RTL Arabic line on 40-col grid");

    std::printf("test_golden_render: %d passed, %d failed (lit pixels=%zu)\n", g_passed, g_failed, lit);
    return g_failed == 0 ? 0 : 1;
}

