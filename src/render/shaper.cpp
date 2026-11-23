/// @file shaper.cpp
/// @brief L4 Renderer — Terminal Line Shaper (BiDi Arabic + OpenType GSUB Ligatures).

#include "render/shaper.hpp"
#include "nisaba/text/bidi.hpp"
#include "vt/utf8_decoder.hpp"

#include <algorithm>
#include <numeric>

namespace vater::render {

namespace {

bool is_ligature_ascii(char32_t cp) noexcept {
    // Programming ligature candidates (ASCII operators & symbols)
    if (cp < 0x21u || cp > 0x7Eu) return false;
    switch (cp) {
        case '!': case '#': case '$': case '%': case '&': case '*':
        case '+': case '-': case '.': case '/': case ':': case ';':
        case '<': case '=': case '>': case '?': case '@': case '\\':
        case '^': case '_': case '|': case '~':
            return true;
        default:
            return false;
    }
}

bool row_contains_rtl(const std::vector<vt::Cell>& cells) noexcept {
    for (const auto& c : cells) {
        if (c.cp >= 0x0590u && c.cp < vt::kGraphemeBase) {
            const auto cls = nisaba::text::Bidi::classify_char(c.cp);
            if (cls == nisaba::text::CharType::RTL || cls == nisaba::text::CharType::ArabicLetter) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

void LineShaper::shape_line(const vt::GridRow& row,
                            const FontStack& fonts,
                            bool enable_bidi,
                            bool enable_ligatures,
                            ShapedLine& out) {
    const int cols = static_cast<int>(row.cells.size());
    out.cols = cols;
    out.has_rtl = false;
    out.glyphs.clear();
    out.visual_to_logical.resize(static_cast<size_t>(cols));
    out.logical_to_visual.resize(static_cast<size_t>(cols));
    std::iota(out.visual_to_logical.begin(), out.visual_to_logical.end(), 0);
    std::iota(out.logical_to_visual.begin(), out.logical_to_visual.end(), 0);

    if (cols == 0) return;

    // ── Fast path: Pure LTR line (no RTL/Arabic codepoints) ──
    const bool need_bidi = enable_bidi && row_contains_rtl(row.cells);
    if (!need_bidi) {
        int c = 0;
        while (c < cols) {
            const vt::Cell& cell = row.cells[static_cast<size_t>(c)];
            if (cell.is_wide_spacer()) {
                ++c;
                continue;
            }

            // Skip spaces that carry no visual glyph (backgrounds/decorations handled in separate passes)
            if (cell.cp == U' ' && !cell.is_grapheme()) {
                ++c;
                continue;
            }

            const bool bold = (cell.attrs & vt::cell_attr::kBold) != 0;
            const bool italic = (cell.attrs & vt::cell_attr::kItalic) != 0;

            // Procedural box-drawing / block elements / Braille / Powerline
            if (is_procedural_symbol(cell.cp)) {
                ShapedGlyph g{};
                g.visual_col = c;
                g.logical_col = c;
                g.cell_span = cell.is_wide() ? 2 : 1;
                g.fallback_cp = cell.cp;
                g.fg = cell.fg;
                g.bg = cell.bg;
                g.attrs = cell.attrs;
                g.ext = cell.ext;
                g.is_procedural_box = true;
                out.glyphs.push_back(g);
                c += g.cell_span;
                continue;
            }

            // OpenType GSUB ligature run for contiguous same-style operator characters
            if (enable_ligatures && is_ligature_ascii(cell.cp)) {
                const auto pface = fonts.primary_face(bold, italic);
                const auto* gsub = (pface.font && pface.font->gsub() && pface.font->gsub()->is_valid())
                                       ? pface.font->gsub()
                                       : nullptr;
                if (gsub) {
                    int run_end = c;
                    while (run_end < cols && run_end - c < 8) {
                        const auto& rc = row.cells[static_cast<size_t>(run_end)];
                        if (!is_ligature_ascii(rc.cp) || rc.fg != cell.fg || rc.attrs != cell.attrs) break;
                        ++run_end;
                    }
                    const int run_len = run_end - c;
                    if (run_len >= 2) {
                        std::vector<uint16_t> gids;
                        gids.reserve(static_cast<size_t>(run_len));
                        bool all_present = true;
                        for (int k = c; k < run_end; ++k) {
                            const uint16_t gid = pface.font->glyph_index(row.cells[static_cast<size_t>(k)].cp);
                            if (gid == 0) { all_present = false; break; }
                            gids.push_back(gid);
                        }
                        if (all_present) {
                            std::vector<uint16_t> shaped_gids = gids;
                            if (gsub->apply_ligatures(shaped_gids) && shaped_gids != gids && !shaped_gids.empty()) {
                                // Emit shaped ligature glyphs aligned across the run's cell span
                                const int n_out = static_cast<int>(shaped_gids.size());
                                for (int j = 0; j < n_out; ++j) {
                                    const int vcol = c + (j * run_len) / n_out;
                                    const int span = (j + 1 == n_out) ? (run_end - vcol) : std::max(1, run_len / n_out);
                                    ShapedGlyph g{};
                                    g.visual_col = vcol;
                                    g.logical_col = vcol;
                                    g.cell_span = span;
                                    g.font_id = pface.font_id;
                                    g.font = pface.font;
                                    g.glyph_id = shaped_gids[static_cast<size_t>(j)];
                                    g.fallback_cp = cell.cp;
                                    g.fg = cell.fg;
                                    g.bg = cell.bg;
                                    g.attrs = cell.attrs;
                                    g.ext = cell.ext;
                                    g.synthetic_bold = pface.synthetic_bold;
                                    g.synthetic_italic = pface.synthetic_italic;
                                    out.glyphs.push_back(g);
                                }
                                c = run_end;
                                continue;
                            }
                        }
                    }
                }
            }

            // Standard single cell glyph
            const ResolvedGlyph rg = fonts.resolve(cell.cp, bold, italic);
            ShapedGlyph g{};
            g.visual_col = c;
            g.logical_col = c;
            g.cell_span = cell.is_wide() ? 2 : 1;
            g.font_id = rg.font_id;
            g.font = rg.font;
            g.glyph_id = rg.glyph_id;
            g.fallback_cp = cell.cp;
            g.fg = cell.fg;
            g.bg = cell.bg;
            g.attrs = cell.attrs;
            g.ext = cell.ext;
            g.synthetic_bold = rg.synthetic_bold;
            g.synthetic_italic = rg.synthetic_italic;
            g.use_bitmap_fallback = rg.use_bitmap_fallback;
            out.glyphs.push_back(g);
            c += g.cell_span;
        }
        return;
    }

    // ── Implicit Per-Line Visual BiDi + Arabic Contextual Shaping (Terminal-WG BiDi) ──
    out.has_rtl = true;

    // Trim trailing default spaces so RTL lines don't get pushed all the way to column 0
    int content_end = cols;
    while (content_end > 0 && row.cells[static_cast<size_t>(content_end - 1)].cp == U' ' &&
           !row.cells[static_cast<size_t>(content_end - 1)].is_wide_spacer()) {
        --content_end;
    }
    if (content_end == 0) return;

    // Encode logical cells [0..content_end) to UTF-8 and record byte_offset -> logical_col map
    std::string utf8_line;
    utf8_line.reserve(static_cast<size_t>(content_end) * 3);
    std::vector<int> byte_to_col;
    byte_to_col.reserve(static_cast<size_t>(content_end) * 3);

    char enc[4];
    for (int c = 0; c < content_end; ++c) {
        const auto& cell = row.cells[static_cast<size_t>(c)];
        if (cell.is_wide_spacer()) continue;
        const char32_t cp = (cell.cp < vt::kGraphemeBase) ? cell.cp : U' ';
        const size_t n = vt::encode_utf8(cp, enc);
        utf8_line.append(enc, n);
        for (size_t k = 0; k < n; ++k) byte_to_col.push_back(c);
    }

    // Run Nisaba UAX #9 BiDi segmentation + Arabic contextual shaping (Isolated/Initial/Medial/Final + Lam-Alif)
    const auto base_dir = nisaba::text::Bidi::detect_base_direction(utf8_line);
    const auto runs = nisaba::text::Bidi::segment_runs(utf8_line, base_dir);
    const auto shaped = nisaba::text::Bidi::shape_text(utf8_line, runs);

    // Reorder runs visually according to base_dir & odd/even BiDi levels (UAX #9 L2)
    struct VisualItem {
        nisaba::text::ShapedChar sc;
        int logical_col{0};
    };

    // Map shaped items back to logical columns and group by directional runs
    std::vector<VisualItem> items;
    items.reserve(shaped.size());
    for (const auto& sc : shaped) {
        int lcol = 0;
        if (sc.byte_index < byte_to_col.size()) {
            lcol = byte_to_col[sc.byte_index];
        }
        items.push_back(VisualItem{sc, lcol});
    }

    // Visual reordering (UAX #9 Rule L2: reverse maximal contiguous runs of level >= L)
    uint8_t max_level = 0;
    for (const auto& it : items) max_level = std::max(max_level, it.sc.level);
    for (int lvl = static_cast<int>(max_level); lvl >= 1; --lvl) {
        size_t i = 0;
        while (i < items.size()) {
            if (items[i].sc.level >= static_cast<uint8_t>(lvl)) {
                size_t j = i;
                while (j < items.size() && items[j].sc.level >= static_cast<uint8_t>(lvl)) ++j;
                std::reverse(items.begin() + static_cast<ptrdiff_t>(i),
                             items.begin() + static_cast<ptrdiff_t>(j));
                i = j;
            } else {
                ++i;
            }
        }
    }

    // Count non-mark visual cells to compute right-aligned start column when base_dir == RightToLeft
    int visual_width = 0;
    for (const auto& it : items) {
        if (!it.sc.is_mark) {
            const auto& cell = row.cells[static_cast<size_t>(it.logical_col)];
            visual_width += cell.is_wide() ? 2 : 1;
        }
    }
    int vcol = (base_dir == nisaba::text::Direction::RightToLeft)
                   ? std::max(0, cols - visual_width)
                   : 0;
    int last_base_vcol = vcol;

    for (const auto& it : items) {
        const auto& cell = row.cells[static_cast<size_t>(it.logical_col)];
        const bool bold = (cell.attrs & vt::cell_attr::kBold) != 0;
        const bool italic = (cell.attrs & vt::cell_attr::kItalic) != 0;

        if (it.sc.is_mark) {
            // Attach diacritic (Tashkeel) onto preceding base visual column
            const ResolvedGlyph rg = fonts.resolve(it.sc.codepoint, bold, italic);
            ShapedGlyph g{};
            g.visual_col = last_base_vcol;
            g.logical_col = it.logical_col;
            g.cell_span = 1;
            g.font_id = rg.font_id;
            g.font = rg.font;
            g.glyph_id = rg.glyph_id;
            g.fallback_cp = it.sc.codepoint;
            g.fg = cell.fg;
            g.bg = cell.bg;
            g.attrs = cell.attrs;
            g.ext = cell.ext;
            g.synthetic_bold = rg.synthetic_bold;
            g.synthetic_italic = rg.synthetic_italic;
            g.use_bitmap_fallback = rg.use_bitmap_fallback;
            out.glyphs.push_back(g);
            continue;
        }

        const int span = cell.is_wide() ? 2 : 1;
        if (vcol >= cols) break;

        out.visual_to_logical[static_cast<size_t>(vcol)] = it.logical_col;
        out.logical_to_visual[static_cast<size_t>(it.logical_col)] = vcol;
        last_base_vcol = vcol;

        if (it.sc.codepoint != U' ') {
            ShapedGlyph g{};
            g.visual_col = vcol;
            g.logical_col = it.logical_col;
            g.cell_span = span;
            g.fallback_cp = it.sc.codepoint;
            g.fg = cell.fg;
            g.bg = cell.bg;
            g.attrs = cell.attrs;
            g.ext = cell.ext;
            if (is_procedural_symbol(it.sc.codepoint)) {
                g.is_procedural_box = true;
            } else {
                const ResolvedGlyph rg = fonts.resolve(it.sc.codepoint, bold, italic);
                g.font_id = rg.font_id;
                g.font = rg.font;
                g.glyph_id = rg.glyph_id;
                g.synthetic_bold = rg.synthetic_bold;
                g.synthetic_italic = rg.synthetic_italic;
                g.use_bitmap_fallback = rg.use_bitmap_fallback;
            }
            out.glyphs.push_back(g);
        }

        vcol += span;
    }
}

} // namespace vater::render

