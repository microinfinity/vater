#pragma once
/// @file cell_renderer.hpp
/// @brief L4 Renderer — 3-Pass GPU Batched Terminal Cell Renderer + Inline Kitty/Sixel Image Layer.
/// Pass 1: Coalesced same-background runs in a single batched draw call.
/// Pass 2: All glyphs from `SovereignGlyphAtlas` (4-bin subpixel) in a single batched draw call.
/// Pass 3: Underlines (single/double/curly/dotted/dashed), strikethrough, cursor, selection, hyperlink hover.
/// Plus: Inline Kitty Graphics & Sixel image placements with GPU texture caching.

#include "nisaba/gpu/gpu_canvas.hpp"
#include "nisaba/math/rect.hpp"
#include "nisaba/vspr/primitive_instancer.hpp"
#include "render/atlas.hpp"
#include "render/cpu_renderer.hpp"
#include "render/font_stack.hpp"
#include "render/palette.hpp"
#include "render/shaper.hpp"
#include "vt/graphics/kitty_graphics.hpp"
#include "vt/terminal.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace vater::render {

/// GPU-accelerated 3-pass terminal pane renderer on Nisaba GPU engine.
class GpuCellRenderer {
public:
    GpuCellRenderer() = default;
    ~GpuCellRenderer() = default;

    /// Renders an entire terminal pane into `canvas` inside `bounds` in 3 batched passes + inline images.
    /// Also caches the visual<->logical column map per row for accurate BiDi mouse/selection hit-testing.
    void render_pane(nisaba::gpu::GpuCanvas& canvas,
                     const nisaba::Rect& bounds,
                     const vt::ViewportSnapshot& snap,
                     const vt::Terminal& terminal,
                     const FontStack& fonts,
                     const TerminalPalette& palette,
                     const RenderOverlayState& overlay);

    /// Maps a screen-space visual column on `view_row` back to its logical column (honors BiDi).
    [[nodiscard]] int visual_to_logical_col(int view_row, int visual_col) const noexcept;

private:
    void render_inline_images(nisaba::gpu::GpuCanvas& canvas,
                              const nisaba::Rect& bounds,
                              const vt::ViewportSnapshot& snap,
                              const vt::Terminal& terminal,
                              const CellMetrics& m,
                              bool behind_text);

    static uint32_t pack_premul_rgba8(const nisaba::Color& c) noexcept;
    static void push_solid_quad(std::vector<nisaba::gpu::Vertex>& out,
                                float x0, float y0, float x1, float y1,
                                uint32_t premul_rgba);
    static void push_textured_quad(std::vector<nisaba::gpu::Vertex>& out,
                                   float x0, float y0, float x1, float y1,
                                   float u0, float v0, float u1, float v1,
                                   uint32_t premul_rgba);

    TerminalAtlas atlas_{};
    nisaba::vspr::PrimitiveInstancer instancer_{};

    // Reused per-frame vertex buffers (zero allocation after warmup)
    std::vector<nisaba::gpu::Vertex> bg_verts_;
    std::vector<nisaba::gpu::Vertex> glyph_verts_;
    std::vector<nisaba::gpu::Vertex> decor_verts_;

    // Cached visual->logical BiDi map from last frame
    std::vector<std::vector<int>> row_v2l_;
    ShapedLine shaped_line_{};

    // Cached copy of inline images (synced only when GraphicsStore::generation() changes)
    uint64_t graphics_gen_{0};
    std::vector<vt::ImagePlacement> cached_placements_;
    std::unordered_map<uint32_t, vt::DecodedImage> cached_images_;
};

} // namespace vater::render

