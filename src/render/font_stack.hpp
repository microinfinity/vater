#pragma once
/// @file font_stack.hpp
/// @brief L4 Renderer — Sovereign Font Stack (Mono primary -> Arabic -> CJK -> Emoji -> font_8x16 fallback).

#include "nisaba/text/font_system.hpp"
#include "nisaba/text/ttf_font.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vater::render {

struct CellMetrics {
    float cell_w{9.0f};
    float cell_h{18.0f};
    float baseline{14.0f};
    float underline_y{15.5f};
    float underline_thickness{1.2f};
    float strike_y{9.0f};
    float font_size{14.0f};
};

struct ResolvedGlyph {
    uint32_t font_id{0};
    const nisaba::text::TtfFont* font{nullptr};
    uint16_t glyph_id{0};
    bool synthetic_bold{false};
    bool synthetic_italic{false};
    bool use_bitmap_fallback{false};
};

/// Manages primary monospace font (Regular/Bold/Italic/BoldItalic), multilingual fallback chain
/// (Arabic, CJK, Emoji), UI sans font, and built-in `font_8x16` emergency fallback.
class FontStack {
public:
    FontStack() = default;

    /// Initializes FontSystem discovery and builds primary + fallback font chains.
    bool init(nisaba::text::FontSystem& font_sys,
              const std::string& primary_family,
              const std::vector<std::string>& fallbacks,
              float font_size,
              float line_height_mult = 1.15f);

    /// Updates font size & recomputes crisp pixel-snapped cell metrics.
    void set_size(float font_size, float line_height_mult = 1.15f);

    /// Resolves the best font & glyph ID for a codepoint with requested bold/italic styling.
    [[nodiscard]] ResolvedGlyph resolve(char32_t cp, bool bold, bool italic) const noexcept;

    /// Returns the primary font face matching bold/italic (for OpenType GSUB ligature runs).
    [[nodiscard]] nisaba::text::FontMatchResult primary_face(bool bold, bool italic) const noexcept;

    [[nodiscard]] const CellMetrics& metrics() const noexcept { return metrics_; }
    [[nodiscard]] nisaba::text::FontSystem* font_system() const noexcept { return font_sys_; }
    [[nodiscard]] bool is_ready() const noexcept { return font_sys_ != nullptr && primary_regular_.font != nullptr; }

private:
    nisaba::text::FontSystem* font_sys_{nullptr};
    nisaba::text::FontMatchResult primary_regular_{};
    nisaba::text::FontMatchResult primary_bold_{};
    nisaba::text::FontMatchResult primary_italic_{};
    nisaba::text::FontMatchResult primary_bold_italic_{};

    std::vector<nisaba::text::Family> family_chain_;
    std::string primary_family_;
    float line_height_mult_{1.15f};
    CellMetrics metrics_{};
};

} // namespace vater::render

