#pragma once
/// @file shaper.hpp
/// @brief L4 Renderer — Terminal Line Shaper: Implicit Visual BiDi + Arabic Contextual Joining
/// (via `nisaba::text::Bidi`) + OpenType GSUB Ligatures (via `OpenTypeGsub`) + Logical<->Visual Column Map.

#include "render/font_stack.hpp"
#include "vt/cell.hpp"
#include "vt/grid.hpp"

#include <cstdint>
#include <vector>

namespace vater::render {

/// A single shaped glyph ready for atlas lookup & GPU/CPU rendering at exact cell column(s).
struct ShapedGlyph {
    int visual_col{0};       // Visual cell column on screen (0..cols-1)
    int logical_col{0};      // Original logical column in GridRow
    int cell_span{1};        // 1 for narrow, 2 for wide, N for multi-cell ligature
    uint32_t font_id{0};
    const nisaba::text::TtfFont* font{nullptr};
    uint16_t glyph_id{0};
    char32_t fallback_cp{0}; // Original codepoint for box-drawing/bitmap fallback
    uint32_t fg{0};
    uint32_t bg{0};
    uint16_t attrs{0};
    uint16_t ext{0};
    bool synthetic_bold{false};
    bool synthetic_italic{false};
    bool use_bitmap_fallback{false};
    bool is_procedural_box{false}; // Box-drawing / Powerline / Braille drawn as crisp paths
    float x_offset{0.0f};          // Extra offset (e.g. Arabic combining mark)
    float y_offset{0.0f};
};

/// Shaped representation of a single terminal row, including logical<->visual BiDi map for selection/mouse.
struct ShapedLine {
    int cols{0};
    bool has_rtl{false};
    std::vector<ShapedGlyph> glyphs;
    std::vector<int> visual_to_logical; // visual_to_logical[vcol] = lcol
    std::vector<int> logical_to_visual; // logical_to_visual[lcol] = vcol
};

/// Terminal line shaper combining Terminal-WG implicit BiDi, Arabic contextual shaping,
/// OpenType GSUB ligatures, and procedural box-drawing detection.
class LineShaper {
public:
    /// Shapes `row` into `out`.
    /// - When `enable_bidi` is true: detects RTL/Arabic runs via `nisaba::text::Bidi`,
    ///   applies contextual Arabic forms + Lam-Alif + bracket mirroring, and visually reorders
    ///   RTL spans inside the fixed-width cell grid while preserving LTR logical storage.
    /// - When `enable_ligatures` is true: applies primary font's OpenType GSUB ligature table
    ///   on contiguous same-style ASCII programming operator runs (`->`, `=>`, `===`, `!==`, `<=`, etc.).
    static void shape_line(const vt::GridRow& row,
                           const FontStack& fonts,
                           bool enable_bidi,
                           bool enable_ligatures,
                           ShapedLine& out);

    /// Returns true if codepoint is Box Drawing (U+2500..257F), Block Elements (U+2580..259F),
    /// Braille Patterns (U+2800..28FF), or Powerline triangles (U+E0B0..E0B3).
    [[nodiscard]] static constexpr bool is_procedural_symbol(char32_t cp) noexcept {
        return (cp >= 0x2500u && cp <= 0x259Fu) ||
               (cp >= 0x2800u && cp <= 0x28FFu) ||
               (cp >= 0xE0B0u && cp <= 0xE0B3u);
    }
};

} // namespace vater::render

