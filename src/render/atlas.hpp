#pragma once
/// @file atlas.hpp
/// @brief L4 Renderer — Terminal GPU Glyph Atlas (on `nisaba::gpu::SovereignGlyphAtlas`)
/// + Procedural Box-Drawing / Block / Braille / Powerline Vector Geometry.

#include "nisaba/gpu/context.hpp"
#include "nisaba/gpu/glyph_atlas.hpp"
#include "nisaba/gpu/gpu_canvas.hpp"
#include "nisaba/math/rect.hpp"
#include "nisaba/text/glyph_cache.hpp"
#include "nisaba/text/ttf_font.hpp"

#include <cstdint>

namespace vater::render {

/// Manages glyph lookup & rasterization into `nisaba::gpu::Context`'s `SovereignGlyphAtlas`
/// with 4-bin subpixel positioning and automatic atlas growth/flush.
class TerminalAtlas {
public:
    TerminalAtlas() = default;

    /// Ensures a glyph is packed in `ctx.atlas()` and returns its atlas entry + pixel offsets.
    /// Automatically grows/flushes atlas if full.
    const nisaba::gpu::AtlasGlyphEntry* ensure_glyph(
        nisaba::gpu::Context& ctx,
        const nisaba::text::TtfFont& font,
        uint32_t font_id,
        uint16_t glyph_id,
        float font_size,
        float target_x,
        float target_y,
        bool synth_bold,
        bool synth_italic,
        int32_t& out_int_x,
        int32_t& out_int_y);

    /// Flushes dirty regions of `ctx.atlas()` to GPU texture `ctx.fontTextureId()`.
    static void flush_if_dirty(nisaba::gpu::Context& ctx);

    /// Renders procedural Box-Drawing (U+2500..257F), Block Elements (U+2580..259F),
    /// Braille (U+2800..28FF), and Powerline arrows (U+E0B0..E0B3) with exact pixel-joined geometry.
    static void draw_procedural_symbol(nisaba::ICanvas& canvas,
                                       char32_t cp,
                                       float x, float y,
                                       float cell_w, float cell_h,
                                       const nisaba::Color& color);

    /// Renders a fallback 8x16 bitmap glyph from `nisaba::font::FONT_8X16` onto any `ICanvas`.
    static void draw_bitmap_fallback(nisaba::ICanvas& canvas,
                                     char32_t cp,
                                     float x, float y,
                                     float cell_w, float cell_h,
                                     const nisaba::Color& color);
};

} // namespace vater::render

