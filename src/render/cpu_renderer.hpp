#pragma once
/// @file cpu_renderer.hpp
/// @brief L4 Renderer — Software Fallback Renderer via `nisaba::Canvas` + `nisaba::Pixmap` + SIMD.
/// Used when GPU acceleration is unavailable AND for deterministic headless golden pixel tests (§9).

#include "nisaba/canvas/canvas.hpp"
#include "nisaba/canvas/pixmap.hpp"
#include "nisaba/math/rect.hpp"
#include "nisaba/text/glyph_cache.hpp"
#include "render/font_stack.hpp"
#include "render/palette.hpp"
#include "render/shaper.hpp"
#include "vt/grid.hpp"
#include "vt/terminal.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace vater::render {

struct SelectionRange {
    bool active{false};
    bool rectangular{false};
    int start_abs_row{0};
    int start_col{0};
    int end_abs_row{0};
    int end_col{0};

    [[nodiscard]] bool contains(int abs_row, int col) const noexcept {
        if (!active) return false;
        int r0 = start_abs_row, c0 = start_col, r1 = end_abs_row, c1 = end_col;
        if (r0 > r1 || (r0 == r1 && c0 > c1)) {
            std::swap(r0, r1);
            std::swap(c0, c1);
        }
        if (abs_row < r0 || abs_row > r1) return false;
        if (rectangular) {
            const int lo = std::min(c0, c1);
            const int hi = std::max(c0, c1);
            return col >= lo && col <= hi;
        }
        if (r0 == r1) return col >= c0 && col <= c1;
        if (abs_row == r0) return col >= c0;
        if (abs_row == r1) return col <= c1;
        return true;
    }
};

struct RenderOverlayState {
    SelectionRange selection{};
    std::vector<vt::SearchMatch> search_matches;
    int active_search_match{-1};
    uint32_t hovered_hyperlink_id{0};
    bool pane_focused{true};
    bool cursor_blink_on{true};
    bool enable_ligatures{true};
    float background_alpha{1.0f};
};

/// Pure-CPU terminal renderer operating directly on a `nisaba::Pixmap`.
class CpuTerminalRenderer {
public:
    CpuTerminalRenderer() = default;

    void render_pane(nisaba::Canvas& canvas,
                     const nisaba::Rect& bounds,
                     const vt::ViewportSnapshot& snap,
                     const vt::Terminal& terminal,
                     const FontStack& fonts,
                     const TerminalPalette& palette,
                     const RenderOverlayState& overlay);

private:
    void render_inline_images(nisaba::Canvas& canvas,
                              const nisaba::Rect& bounds,
                              const vt::ViewportSnapshot& snap,
                              const vt::Terminal& terminal,
                              const CellMetrics& m,
                              bool behind_text);

    nisaba::text::GlyphCache glyph_cache_{4096};
    ShapedLine shaped_line_{};
    uint64_t graphics_gen_{0};
    std::vector<vt::ImagePlacement> cached_placements_;
    std::unordered_map<uint32_t, vt::DecodedImage> cached_images_;
};

} // namespace vater::render

