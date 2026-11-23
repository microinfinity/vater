/// @file cpu_renderer.cpp
/// @brief L4 Renderer — Software Fallback Renderer via `nisaba::Canvas` + `nisaba::Pixmap`.

#include "render/cpu_renderer.hpp"
#include "render/atlas.hpp"

#include "nisaba/canvas/paint.hpp"
#include "nisaba/path/path_builder.hpp"

#include <algorithm>
#include <cmath>

namespace vater::render {

namespace {

void blit_alpha_mask(nisaba::PixmapMut& dst,
                     int32_t dx, int32_t dy,
                     const nisaba::text::CachedGlyph& glyph,
                     const nisaba::Color& color,
                     int32_t clip_x0, int32_t clip_y0,
                     int32_t clip_x1, int32_t clip_y1) noexcept {
    if (glyph.width == 0 || glyph.height == 0 || glyph.data.empty()) return;
    const auto c8 = color.to_color_u8();
    const int32_t gw = static_cast<int32_t>(glyph.width);
    const int32_t gh = static_cast<int32_t>(glyph.height);

    const int32_t y_start = std::max(0, clip_y0 - dy);
    const int32_t y_end = std::min(gh, clip_y1 - dy);
    const int32_t x_start = std::max(0, clip_x0 - dx);
    const int32_t x_end = std::min(gw, clip_x1 - dx);
    if (x_start >= x_end || y_start >= y_end) return;

    for (int32_t gy = y_start; gy < y_end; ++gy) {
        const uint8_t* src_row = glyph.data.data() + static_cast<size_t>(gy) * glyph.width;
        uint8_t* dst_row = dst.row_raw(static_cast<uint32_t>(dy + gy));
        for (int32_t gx = x_start; gx < x_end; ++gx) {
            const uint8_t cov = src_row[gx];
            if (cov == 0) continue;
            const uint32_t sa = (static_cast<uint32_t>(cov) * c8.a + 127u) / 255u;
            if (sa == 0) continue;
            uint8_t* px = dst_row + static_cast<size_t>(dx + gx) * 4u;
            const uint32_t inv_sa = 255u - sa;
            px[0] = static_cast<uint8_t>((c8.r * sa + px[0] * inv_sa + 127u) / 255u);
            px[1] = static_cast<uint8_t>((c8.g * sa + px[1] * inv_sa + 127u) / 255u);
            px[2] = static_cast<uint8_t>((c8.b * sa + px[2] * inv_sa + 127u) / 255u);
            px[3] = static_cast<uint8_t>(std::min<uint32_t>(255u, sa + (px[3] * inv_sa + 127u) / 255u));
        }
    }
}

void draw_underline(nisaba::ICanvas& canvas,
                    vt::UnderlineStyle style,
                    float x, float y, float w, float thickness,
                    const nisaba::Color& col) {
    if (style == vt::UnderlineStyle::None) return;
    nisaba::Paint paint(col);
    switch (style) {
        case vt::UnderlineStyle::Single:
            if (auto r = nisaba::Rect::from_xywh(x, y, w, thickness)) canvas.fill_rect(*r, paint);
            break;
        case vt::UnderlineStyle::Double:
            if (auto r1 = nisaba::Rect::from_xywh(x, y - thickness, w, thickness)) canvas.fill_rect(*r1, paint);
            if (auto r2 = nisaba::Rect::from_xywh(x, y + thickness, w, thickness)) canvas.fill_rect(*r2, paint);
            break;
        case vt::UnderlineStyle::Curly: {
            nisaba::PathBuilder pb;
            const float amp = std::max(1.2f, thickness * 1.1f);
            const float step = std::max(2.0f, w * 0.25f);
            pb.move_to(x, y);
            bool up = true;
            for (float cx = x + step; cx <= x + w + 0.5f; cx += step) {
                pb.line_to(std::min(cx, x + w), y + (up ? -amp : amp));
                up = !up;
            }
            if (auto path = pb.finish()) {
                nisaba::Stroke st;
                st.width = thickness;
                canvas.stroke_path(*path, paint, st);
            }
            break;
        }
        case vt::UnderlineStyle::Dotted:
            for (float cx = x; cx < x + w; cx += thickness * 2.5f) {
                if (auto r = nisaba::Rect::from_xywh(cx, y, thickness, thickness)) canvas.fill_rect(*r, paint);
            }
            break;
        case vt::UnderlineStyle::Dashed:
            for (float cx = x; cx < x + w; cx += thickness * 5.0f) {
                if (auto r = nisaba::Rect::from_xywh(cx, y, std::min(thickness * 3.0f, x + w - cx), thickness)) {
                    canvas.fill_rect(*r, paint);
                }
            }
            break;
        default:
            break;
    }
}

} // namespace

void CpuTerminalRenderer::render_inline_images(nisaba::Canvas& canvas,
                                               const nisaba::Rect& bounds,
                                               const vt::ViewportSnapshot& snap,
                                               const vt::Terminal& terminal,
                                               const CellMetrics& m,
                                               bool behind_text) {
    terminal.copy_graphics(cached_placements_, cached_images_, graphics_gen_);
    if (cached_placements_.empty()) return;

    for (const auto& pl : cached_placements_) {
        if (pl.is_alt_screen != snap.alt_screen) continue;
        const bool is_behind = (pl.z_index < 0);
        if (is_behind != behind_text) continue;

        auto it = cached_images_.find(pl.image_id);
        if (it == cached_images_.end()) continue;
        const auto& img = it->second;
        if (img.width == 0 || img.height == 0 || img.rgba.empty()) continue;

        const int draw_row = pl.row + snap.scroll_offset;
        const float px = std::floor(bounds.left()) + static_cast<float>(pl.col) * m.cell_w;
        const float py = std::floor(bounds.top()) + static_cast<float>(draw_row) * m.cell_h;
        float pw = 0.0f;
        float ph = 0.0f;
        if (pl.cols > 0 && pl.rows > 0) {
            pw = static_cast<float>(pl.cols) * m.cell_w;
            ph = static_cast<float>(pl.rows) * m.cell_h;
        } else if (pl.cols > 0) {
            pw = static_cast<float>(pl.cols) * m.cell_w;
            ph = pw * (static_cast<float>(img.height) / static_cast<float>(img.width));
        } else if (pl.rows > 0) {
            ph = static_cast<float>(pl.rows) * m.cell_h;
            pw = ph * (static_cast<float>(img.width) / static_cast<float>(img.height));
        } else {
            pw = static_cast<float>(img.width);
            ph = static_cast<float>(img.height);
        }

        if (py + ph < bounds.top() || py > bounds.bottom()) continue;

        auto pm_ref = nisaba::PixmapRef::from_bytes(
            img.rgba.data(), img.rgba.size(), img.width, img.height,
            nisaba::PixelFormat::RGBA8888);
        if (!pm_ref) continue;

        auto src_r = nisaba::Rect::from_xywh(0.0f, 0.0f, static_cast<float>(img.width), static_cast<float>(img.height));
        auto dst_r = nisaba::Rect::from_xywh(px, py, pw, ph);
        if (src_r && dst_r) {
            canvas.draw_pixmap_rect(*pm_ref, *src_r, *dst_r);
        }
    }
}

void CpuTerminalRenderer::render_pane(nisaba::Canvas& canvas,
                                      const nisaba::Rect& bounds,
                                      const vt::ViewportSnapshot& snap,
                                      const vt::Terminal& terminal,
                                      const FontStack& fonts,
                                      const TerminalPalette& palette,
                                      const RenderOverlayState& overlay) {
    const auto& m = fonts.metrics();
    const float cw = m.cell_w;
    const float ch = m.cell_h;
    const float origin_x = std::floor(bounds.left());
    const float origin_y = std::floor(bounds.top());

    const int32_t clip_x0 = std::max(0, static_cast<int32_t>(std::floor(bounds.left())));
    const int32_t clip_y0 = std::max(0, static_cast<int32_t>(std::floor(bounds.top())));
    const int32_t clip_x1 = std::min(static_cast<int32_t>(canvas.width()), static_cast<int32_t>(std::ceil(bounds.right())));
    const int32_t clip_y1 = std::min(static_cast<int32_t>(canvas.height()), static_cast<int32_t>(std::ceil(bounds.bottom())));

    canvas.save();
    canvas.clip_rect(bounds);

    const nisaba::Color pane_bg = obsidian::ui::detail::with_alpha(
        snap.reverse_video ? palette.default_fg : palette.default_bg,
        overlay.background_alpha);
    canvas.fill_rect(bounds, nisaba::Paint(pane_bg));

    // Inline images with z_index < 0 render behind text
    render_inline_images(canvas, bounds, snap, terminal, m, true);

    const int sb_total = snap.alt_screen ? 0 : static_cast<int>(snap.scrollback_size);

    for (int r = 0; r < snap.rows && r < static_cast<int>(snap.lines.size()); ++r) {
        const auto& line = snap.lines[static_cast<size_t>(r)];
        const int abs_row = sb_total - snap.scroll_offset + r;
        const float row_y = origin_y + static_cast<float>(r) * ch;

        LineShaper::shape_line(line, fonts, snap.bidi_implicit, overlay.enable_ligatures, shaped_line_);

        // ── Pass 1: Coalesced Cell Background Runs ──
        int c = 0;
        while (c < snap.cols && c < static_cast<int>(line.cells.size())) {
            const int lcol = (c < static_cast<int>(shaped_line_.visual_to_logical.size()))
                                 ? shaped_line_.visual_to_logical[static_cast<size_t>(c)]
                                 : c;
            const auto& cell = line.cells[static_cast<size_t>(lcol)];
            const bool inv = ((cell.attrs & vt::cell_attr::kInverse) != 0) ^ snap.reverse_video;
            const bool is_sel = overlay.selection.contains(abs_row, lcol);

            bool has_bg = is_sel || inv || !vt::is_color_default(cell.bg);
            nisaba::Color bg_col = is_sel
                                       ? palette.selection_bg
                                       : (inv ? palette.resolve_fg(cell.fg, (cell.attrs & vt::cell_attr::kBold) != 0, false)
                                              : palette.resolve_bg(cell.bg));

            if (!has_bg) { ++c; continue; }

            int run_end = c + 1;
            while (run_end < snap.cols && run_end < static_cast<int>(line.cells.size())) {
                const int nl = (run_end < static_cast<int>(shaped_line_.visual_to_logical.size()))
                                   ? shaped_line_.visual_to_logical[static_cast<size_t>(run_end)]
                                   : run_end;
                const auto& nc = line.cells[static_cast<size_t>(nl)];
                const bool ninv = ((nc.attrs & vt::cell_attr::kInverse) != 0) ^ snap.reverse_video;
                const bool nsel = overlay.selection.contains(abs_row, nl);
                const bool nhas = nsel || ninv || !vt::is_color_default(nc.bg);
                if (!nhas) break;
                const nisaba::Color nbg = nsel
                                              ? palette.selection_bg
                                              : (ninv ? palette.resolve_fg(nc.fg, (nc.attrs & vt::cell_attr::kBold) != 0, false)
                                                      : palette.resolve_bg(nc.bg));
                if (nbg.to_color_u8() != bg_col.to_color_u8()) break;
                ++run_end;
            }

            if (auto rect = nisaba::Rect::from_xywh(
                    origin_x + static_cast<float>(c) * cw, row_y,
                    static_cast<float>(run_end - c) * cw, ch)) {
                canvas.fill_rect(*rect, nisaba::Paint(bg_col));
            }
            c = run_end;
        }

        // Search match highlights on this row
        for (size_t mi = 0; mi < overlay.search_matches.size(); ++mi) {
            const auto& sm = overlay.search_matches[mi];
            if (sm.abs_row != abs_row) continue;
            const nisaba::Color mcol = (static_cast<int>(mi) == overlay.active_search_match)
                                           ? palette.search_current_bg
                                           : palette.search_match_bg;
            if (auto rect = nisaba::Rect::from_xywh(
                    origin_x + static_cast<float>(sm.col_start) * cw, row_y,
                    static_cast<float>(std::max(1, sm.col_end - sm.col_start)) * cw, ch)) {
                canvas.fill_rect(*rect, nisaba::Paint(mcol));
            }
        }

        // ── Pass 2: Glyphs (Font Cache / Procedural Symbols / 8x16 Bitmap Fallback) ──
        for (const auto& sg : shaped_line_.glyphs) {
            if ((sg.attrs & vt::cell_attr::kInvisible) != 0) continue;
            const bool bold = (sg.attrs & vt::cell_attr::kBold) != 0;
            const bool dim = (sg.attrs & vt::cell_attr::kDim) != 0;
            const bool inv = ((sg.attrs & vt::cell_attr::kInverse) != 0) ^ snap.reverse_video;
            const nisaba::Color fg_col = inv
                                             ? palette.resolve_bg(sg.bg)
                                             : palette.resolve_fg(sg.fg, bold, dim);

            const float gx = origin_x + static_cast<float>(sg.visual_col) * cw + sg.x_offset;
            const float gy = row_y + sg.y_offset;

            if (sg.is_procedural_box) {
                TerminalAtlas::draw_procedural_symbol(canvas, sg.fallback_cp, gx, gy, cw * sg.cell_span, ch, fg_col);
                continue;
            }
            if (sg.use_bitmap_fallback || !sg.font) {
                TerminalAtlas::draw_bitmap_fallback(canvas, sg.fallback_cp, gx, gy, cw, ch, fg_col);
                continue;
            }

            const float baseline_y = gy + m.baseline;
            const auto* cg = glyph_cache_.get_or_render(
                *sg.font, sg.font_id, sg.glyph_id, m.font_size,
                gx, baseline_y, sg.synthetic_bold, sg.synthetic_italic);
            if (cg) {
                const int32_t dx = static_cast<int32_t>(std::floor(gx)) + cg->offset_x;
                const int32_t dy = static_cast<int32_t>(std::floor(baseline_y)) + cg->offset_y;
                blit_alpha_mask(canvas.pixmap(), dx, dy, *cg, fg_col, clip_x0, clip_y0, clip_x1, clip_y1);
            }
        }

        // ── Pass 3: Decorations (Underlines, Strikethrough, Overline, Hyperlink hover) ──
        for (int vc = 0; vc < snap.cols && vc < static_cast<int>(line.cells.size()); ++vc) {
            const int lc = (vc < static_cast<int>(shaped_line_.visual_to_logical.size()))
                               ? shaped_line_.visual_to_logical[static_cast<size_t>(vc)]
                               : vc;
            const auto& cell = line.cells[static_cast<size_t>(lc)];
            const auto ustyle = vt::get_underline_style(cell.attrs);
            const bool strike = (cell.attrs & vt::cell_attr::kStrikethrough) != 0;
            const bool over = (cell.attrs & vt::cell_attr::kOverline) != 0;
            const bool has_ext = (cell.ext != 0);
            if (ustyle == vt::UnderlineStyle::None && !strike && !over && !has_ext) continue;

            const bool bold = (cell.attrs & vt::cell_attr::kBold) != 0;
            const bool dim = (cell.attrs & vt::cell_attr::kDim) != 0;
            const nisaba::Color fg_col = palette.resolve_fg(cell.fg, bold, dim);
            const float cx = origin_x + static_cast<float>(vc) * cw;

            vt::ExtCellData ext{};
            if (has_ext) ext = terminal.get_ext_data(cell.ext);

            auto effective_u = ustyle;
            nisaba::Color ucol = fg_col;
            if (!vt::is_color_default(ext.underline_color)) {
                ucol = palette.resolve_fg(ext.underline_color, false, false);
            }
            if (ext.hyperlink_id != 0 && ext.hyperlink_id == overlay.hovered_hyperlink_id) {
                effective_u = vt::UnderlineStyle::Single;
                ucol = palette.cursor_color;
            }

            if (effective_u != vt::UnderlineStyle::None) {
                draw_underline(canvas, effective_u, cx, row_y + m.underline_y, cw, m.underline_thickness, ucol);
            }
            if (strike) {
                if (auto sr = nisaba::Rect::from_xywh(cx, row_y + m.strike_y, cw, m.underline_thickness)) {
                    canvas.fill_rect(*sr, nisaba::Paint(fg_col));
                }
            }
            if (over) {
                if (auto or_ = nisaba::Rect::from_xywh(cx, row_y + 0.5f, cw, m.underline_thickness)) {
                    canvas.fill_rect(*or_, nisaba::Paint(fg_col));
                }
            }
        }
    }

    // ── Cursor ──
    if (snap.cursor_visible && (!snap.cursor_blink || overlay.cursor_blink_on || !overlay.pane_focused)) {
        const float cx = origin_x + static_cast<float>(snap.cursor.col) * cw;
        const float cy = origin_y + static_cast<float>(snap.cursor.row) * ch;
        nisaba::Paint cp(palette.cursor_color);
        if (!overlay.pane_focused) {
            nisaba::Stroke st;
            st.width = 1.5f;
            if (auto cr = nisaba::Rect::from_xywh(cx + 0.5f, cy + 0.5f, cw - 1.0f, ch - 1.0f)) {
                canvas.stroke_rect(*cr, cp, st);
            }
        } else if (snap.cursor_shape == vt::CursorShape::Bar) {
            if (auto cr = nisaba::Rect::from_xywh(cx, cy, std::max(2.0f, cw * 0.18f), ch)) {
                canvas.fill_rect(*cr, cp);
            }
        } else if (snap.cursor_shape == vt::CursorShape::Underline) {
            const float uh = std::max(2.0f, ch * 0.14f);
            if (auto cr = nisaba::Rect::from_xywh(cx, cy + ch - uh, cw, uh)) {
                canvas.fill_rect(*cr, cp);
            }
        } else {
            nisaba::Paint bp(obsidian::ui::detail::with_alpha(palette.cursor_color, 0.85f));
            if (auto cr = nisaba::Rect::from_xywh(cx, cy, cw, ch)) {
                canvas.fill_rect(*cr, bp);
            }
        }
    }

    // Inline images with z_index >= 0 render over text
    render_inline_images(canvas, bounds, snap, terminal, m, false);

    canvas.restore();
}

} // namespace vater::render

