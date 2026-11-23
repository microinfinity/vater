/// @file font_stack.cpp
/// @brief L4 Renderer — Sovereign Font Stack implementation on Nisaba FontSystem.

#include "render/font_stack.hpp"

#include <algorithm>
#include <cmath>

namespace vater::render {

namespace {

bool is_actually_monospace(const nisaba::text::TtfFont* f) noexcept {
    if (!f || !f->is_valid()) return false;
    if (f->is_monospace()) return true;
    const uint16_t gi = f->glyph_index(U'i');
    const uint16_t gm = f->glyph_index(U'M');
    if (gi == 0 || gm == 0) return false;
    const float ai = f->glyph_advance(gi, 16.0f);
    const float am = f->glyph_advance(gm, 16.0f);
    return ai > 0.5f && std::abs(ai - am) < 0.25f;
}

} // namespace

bool FontStack::init(nisaba::text::FontSystem& font_sys,
                     const std::string& primary_family,
                     const std::vector<std::string>& fallbacks,
                     float font_size,
                     float line_height_mult) {
    font_sys_ = &font_sys;
    primary_family_ = primary_family;
    line_height_mult_ = line_height_mult;

    // IMPORTANT: `canvas.create_font("sans", ...)` already loads 1 UI font into FontSystem,
    // so check `< 16` instead of `== 0` to guarantee system monospace & fallback fonts are indexed!
    if (font_sys.font_count() < 16) {
        font_sys.scan_system_fonts(true);
    }

    // Build ordered fallback family chain: primary -> user fallbacks -> static crisp Linux Mono/Symbols/Arabic/CJK/Emoji
    family_chain_.clear();
    if (!primary_family.empty()) {
        family_chain_.emplace_back(std::string_view(primary_family));
    }
    for (const auto& fb : fallbacks) {
        if (!fb.empty()) family_chain_.emplace_back(std::string_view(fb));
    }
    static constexpr const char* kSystemFallbacks[] = {
        "DejaVu Sans Mono", "Noto Sans Mono", "Liberation Mono", "Cascadia Mono",
        "Cascadia Code", "JetBrains Mono", "Fira Code", "Ubuntu Mono",
        "Symbols Nerd Font Mono", "Noto Sans Arabic", "Noto Kufi Arabic",
        "DejaVu Sans", "Noto Sans Symbols2", "Noto Sans CJK SC", "Noto Color Emoji"
    };
    for (const char* name : kSystemFallbacks) {
        family_chain_.emplace_back(std::string_view(name));
    }
    family_chain_.push_back(nisaba::text::Family::monospace());

    primary_regular_ = font_sys.match_font_chain_styled(
        family_chain_, nisaba::text::Family::monospace(),
        nisaba::text::Weight::Normal, nisaba::text::Style::Normal);

    // Guardrail: never allow a proportional UI font (like Inter) as primary terminal font!
    if (!is_actually_monospace(primary_regular_.font)) {
        for (const auto& fam : family_chain_) {
            auto cand = font_sys.match_font_styled(fam, nisaba::text::Weight::Normal, nisaba::text::Style::Normal);
            if (is_actually_monospace(cand.font)) {
                primary_regular_ = cand;
                break;
            }
        }
    }

    // Match bold/italic from the SAME primary family as primary_regular_ for identical cell advance!
    if (primary_regular_.font) {
        const nisaba::text::Family chosen_fam(primary_regular_.font->family_name());
        primary_bold_ = font_sys.match_font_styled(
            chosen_fam, nisaba::text::Weight::Bold, nisaba::text::Style::Normal);
        primary_italic_ = font_sys.match_font_styled(
            chosen_fam, nisaba::text::Weight::Normal, nisaba::text::Style::Italic);
        primary_bold_italic_ = font_sys.match_font_styled(
            chosen_fam, nisaba::text::Weight::Bold, nisaba::text::Style::Italic);

        if (!is_actually_monospace(primary_bold_.font)) {
            primary_bold_ = primary_regular_;
            primary_bold_.synthetic_bold = true;
        }
        if (!is_actually_monospace(primary_italic_.font)) {
            primary_italic_ = primary_regular_;
            primary_italic_.synthetic_italic = true;
        }
        if (!is_actually_monospace(primary_bold_italic_.font)) {
            primary_bold_italic_ = primary_bold_;
            primary_bold_italic_.synthetic_italic = true;
        }
    }

    set_size(font_size, line_height_mult);
    return primary_regular_.font != nullptr;
}

void FontStack::set_size(float font_size, float line_height_mult) {
    metrics_.font_size = std::clamp(font_size, 6.0f, 96.0f);
    line_height_mult_ = std::clamp(line_height_mult, 0.8f, 2.5f);

    if (primary_regular_.font) {
        const auto* f = primary_regular_.font;
        const float adv_0 = f->glyph_advance(f->glyph_index(U'0'), metrics_.font_size);
        const float adv_m = f->glyph_advance(f->glyph_index(U'M'), metrics_.font_size);
        const float raw_w = (adv_0 > 0.5f) ? adv_0 : ((adv_m > 0.5f) ? adv_m : (metrics_.font_size * 0.6f));
        const float asc = f->ascent(metrics_.font_size);
        const float desc = std::abs(f->descent(metrics_.font_size));
        const float raw_h = std::max(asc + desc, metrics_.font_size * 1.05f) * line_height_mult_;

        metrics_.cell_w = std::max(4.0f, std::round(raw_w));
        metrics_.cell_h = std::max(8.0f, std::round(raw_h));
        const float extra_leading = std::max(0.0f, metrics_.cell_h - (asc + desc));
        metrics_.baseline = std::round(asc + extra_leading * 0.5f);
        metrics_.underline_y = std::min(metrics_.cell_h - 1.5f, metrics_.baseline + std::max(1.5f, desc * 0.35f));
        metrics_.underline_thickness = std::max(1.0f, std::round(metrics_.font_size / 12.0f));
        metrics_.strike_y = std::round(metrics_.baseline - asc * 0.33f);
    } else {
        const float scale = std::max(1.0f, std::round(metrics_.font_size / 14.0f));
        metrics_.cell_w = 8.0f * scale;
        metrics_.cell_h = 16.0f * scale;
        metrics_.baseline = 13.0f * scale;
        metrics_.underline_y = 14.5f * scale;
        metrics_.underline_thickness = scale;
        metrics_.strike_y = 8.0f * scale;
    }
}

nisaba::text::FontMatchResult FontStack::primary_face(bool bold, bool italic) const noexcept {
    if (bold && italic) return primary_bold_italic_;
    if (bold) return primary_bold_;
    if (italic) return primary_italic_;
    return primary_regular_;
}

ResolvedGlyph FontStack::resolve(char32_t cp, bool bold, bool italic) const noexcept {
    ResolvedGlyph out{};
    if (!font_sys_) {
        out.use_bitmap_fallback = true;
        return out;
    }

    const auto primary = primary_face(bold, italic);
    if (primary.font) {
        const uint16_t gid = primary.font->glyph_index(cp);
        if (gid != 0) {
            out.font_id = primary.font_id;
            out.font = primary.font;
            out.glyph_id = gid;
            out.synthetic_bold = primary.synthetic_bold;
            out.synthetic_italic = primary.synthetic_italic;
            return out;
        }
    }

    const auto weight = bold ? nisaba::text::Weight::Bold : nisaba::text::Weight::Normal;
    const auto style = italic ? nisaba::text::Style::Italic : nisaba::text::Style::Normal;
    auto [fid, gid] = font_sys_->find_glyph_in_chain_or_fallback(
        family_chain_, primary.font_id, weight, style, nisaba::text::Stretch::Normal, cp);
    if (gid != 0) {
        if (const auto* f = font_sys_->get_font(fid)) {
            out.font_id = fid;
            out.font = f;
            out.glyph_id = gid;
            out.synthetic_bold = bold && (f->weight() < nisaba::text::Weight::SemiBold);
            out.synthetic_italic = italic && (f->style() == nisaba::text::Style::Normal);
            return out;
        }
    }

    out.use_bitmap_fallback = true;
    return out;
}

} // namespace vater::render
