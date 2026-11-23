/// @file cell_renderer.cpp
/// @brief L4 Renderer — 3-Pass GPU Batched Terminal Cell Renderer implementation.

#include "render/cell_renderer.hpp"

#include "nisaba/canvas/paint.hpp"
#include "nisaba/canvas/pixmap.hpp"
#include "nisaba/gpu/context.hpp"
#include "nisaba/path/path_builder.hpp"

#include <algorithm>
#include <cmath>

namespace vater::render {

uint32_t GpuCellRenderer::pack_premul_rgba8(const nisaba::Color& c) noexcept {
    const float a = std::clamp(c.alpha(), 0.0f, 1.0f);
    const auto r8 = static_cast<uint8_t>(std::clamp(std::round(c.red() * a * 255.0f), 0.0f, 255.0f));
    const auto g8 = static_cast<uint8_t>(std::clamp(std::round(c.green() * a * 255.0f), 0.0f, 255.0f));
    const auto b8 = static_cast<uint8_t>(std::clamp(std::round(c.blue() * a * 255.0f), 0.0f, 255.0f));
    const auto a8 = static_cast<uint8_t>(std::clamp(std::round(a * 255.0f), 0.0f, 255.0f));
    return static_cast<uint32_t>(r8) |
           (static_cast<uint32_t>(g8) << 8) |
           (static_cast<uint32_t>(b8) << 16) |
           (static_cast<uint32_t>(a8) << 24);
}

void GpuCellRenderer::push_solid_quad(std::vector<nisaba::gpu::Vertex>& out,
                                      float x0, float y0, float x1, float y1,
                                      uint32_t rgba) {
    out.emplace_back(x0, y0, 0.5f, 1.0f, rgba);
    out.emplace_back(x0, y1, 0.5f, 1.0f, rgba);
    out.emplace_back(x1, y1, 0.5f, 1.0f, rgba);
    out.emplace_back(x0, y0, 0.5f, 1.0f, rgba);
    out.emplace_back(x1, y1, 0.5f, 1.0f, rgba);
    out.emplace_back(x1, y0, 0.5f, 1.0f, rgba);
}

void GpuCellRenderer::push_textured_quad(std::vector<nisaba::gpu::Vertex>& out,
                                         float x0, float y0, float x1, float y1,
                                         float u0, float v0, float u1, float v1,
                                         uint32_t rgba) {
    out.emplace_back(x0, y0, u0, v0, rgba);
    out.emplace_back(x1, y1, u1, v1, rgba);
    out.emplace_back(x1, y0, u1, v0, rgba);
    out.emplace_back(x0, y0, u0, v0, rgba);
    out.emplace_back(x0, y1, u0, v1, rgba);
    out.emplace_back(x1, y1, u1, v1, rgba);
}

int GpuCellRenderer::visual_to_logical_col(int view_row, int visual_col) const noexcept {
    if (view_row < 0 || view_row >= static_cast<int>(row_v2l_.size())) return visual_col;
    const auto& map = row_v2l_[static_cast<size_t>(view_row)];
    if (visual_col < 0 || visual_col >= static_cast<int>(map.size())) return visual_col;
    return map[static_cast<size_t>(visual_col)];
}

void GpuCellRenderer::render_inline_images(nisaba::gpu::GpuCanvas& canvas,
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

void GpuCellRenderer::render_pane(nisaba::gpu::GpuCanvas& canvas,
                                  const nisaba::Rect& bounds,
                                  const vt::ViewportSnapshot& snap,
                                  const vt::Terminal& terminal,
                                  const FontStack& fonts,
                                  const TerminalPalette& palette,
                                  const RenderOverlayState& overlay) {
    auto* ctx = canvas.context();
    if (!ctx) return;

    const auto& m = fonts.metrics();
    const float cw = m.cell_w;
    const float ch = m.cell_h;
    const float origin_x = std::floor(bounds.left());
    const float origin_y = std::floor(bounds.top());

    canvas.save();
    canvas.clip_rect(bounds);

    bg_verts_.clear();
    glyph_verts_.clear();
    decor_verts_.clear();

    const int num_rows = std::min(snap.rows, static_cast<int>(snap.lines.size()));
    row_v2l_.resize(static_cast<size_t>(num_rows));

    // Optional reverse-video full pane fill
    if (snap.reverse_video) {
        const nisaba::Color rev_bg = obsidian::ui::detail::with_alpha(palette.default_fg, overlay.background_alpha);
        push_solid_quad(bg_verts_, bounds.left(), bounds.top(), bounds.right(), bounds.bottom(),
                        pack_premul_rgba8(rev_bg));
    }

    // Inline images with z_index < 0 render behind text
    render_inline_images(canvas, bounds, snap, terminal, m, true);

    const int sb_total = snap.alt_screen ? 0 : static_cast<int>(snap.scrollback_size);

    struct ProceduralItem {
        char32_t cp;
        float x, y, w, h;
        nisaba::Color col;
        bool is_bitmap;
    };
    std::vector<ProceduralItem> procedural_items;

    struct CurlyUnderline {
        float x, y, w;
        nisaba::Color col;
    };
    std::vector<CurlyUnderline> curly_underlines;

    for (int r = 0; r < num_rows; ++r) {
        const auto& line = snap.lines[static_cast<size_t>(r)];
        const int abs_row = sb_total - snap.scroll_offset + r;
        const float row_y = origin_y + static_cast<float>(r) * ch;

        LineShaper::shape_line(line, fonts, snap.bidi_implicit, overlay.enable_ligatures, shaped_line_);
        row_v2l_[static_cast<size_t>(r)] = shaped_line_.visual_to_logical;

        const int line_cols = std::min(snap.cols, static_cast<int>(line.cells.size()));

        // ── Pass 1: Coalesced Cell Background Runs (one quad per same-color run) ──
        int c = 0;
        while (c < line_cols) {
            const int lc = (c < static_cast<int>(shaped_line_.visual_to_logical.size()))
                               ? shaped_line_.visual_to_logical[static_cast<size_t>(c)]
                               : c;
            const auto& cell = line.cells[static_cast<size_t>(lc)];
            const bool inv = ((cell.attrs & vt::cell_attr::kInverse) != 0) ^ snap.reverse_video;
            const bool is_sel = overlay.selection.contains(abs_row, lc);
            const bool has_bg = is_sel || inv || !vt::is_color_default(cell.bg);

            if (!has_bg) { ++c; continue; }

            const nisaba::Color bg_col = is_sel
                                             ? palette.selection_bg
                                             : (inv ? palette.resolve_fg(cell.fg, (cell.attrs & vt::cell_attr::kBold) != 0, false)
                                                    : palette.resolve_bg(cell.bg));
            const uint32_t bg_packed = pack_premul_rgba8(bg_col);

            int run_end = c + 1;
            while (run_end < line_cols) {
                const int nl = (run_end < static_cast<int>(shaped_line_.visual_to_logical.size()))
                                   ? shaped_line_.visual_to_logical[static_cast<size_t>(run_end)]
                                   : run_end;
                const auto& nc = line.cells[static_cast<size_t>(nl)];
                const bool ninv = ((nc.attrs & vt::cell_attr::kInverse) != 0) ^ snap.reverse_video;
                const bool nsel = overlay.selection.contains(abs_row, nl);
                if (!(nsel || ninv || !vt::is_color_default(nc.bg))) break;
                const nisaba::Color nbg = nsel
                                              ? palette.selection_bg
                                              : (ninv ? palette.resolve_fg(nc.fg, (nc.attrs & vt::cell_attr::kBold) != 0, false)
                                                      : palette.resolve_bg(nc.bg));
                if (pack_premul_rgba8(nbg) != bg_packed) break;
                ++run_end;
            }

            const float rx0 = origin_x + static_cast<float>(c) * cw;
            const float rx1 = origin_x + static_cast<float>(run_end) * cw;
            push_solid_quad(bg_verts_, rx0, row_y, rx1, row_y + ch, bg_packed);
            c = run_end;
        }

        // Search highlights on this row
        for (size_t mi = 0; mi < overlay.search_matches.size(); ++mi) {
            const auto& sm = overlay.search_matches[mi];
            if (sm.abs_row != abs_row) continue;
            const nisaba::Color mcol = (static_cast<int>(mi) == overlay.active_search_match)
                                           ? palette.search_current_bg
                                           : palette.search_match_bg;
            const float mx0 = origin_x + static_cast<float>(sm.col_start) * cw;
            const float mx1 = origin_x + static_cast<float>(std::max(sm.col_start + 1, sm.col_end)) * cw;
            push_solid_quad(bg_verts_, mx0, row_y, mx1, row_y + ch, pack_premul_rgba8(mcol));
        }

        // ── Pass 2: Batched Atlas Glyphs ──
        for (const auto& sg : shaped_line_.glyphs) {
            if ((sg.attrs & vt::cell_attr::kInvisible) != 0) continue;
            const bool bold = (sg.attrs & vt::cell_attr::kBold) != 0;
            const bool dim = (sg.attrs & vt::cell_attr::kDim) != 0;
            const bool inv = ((sg.attrs & vt::cell_attr::kInverse) != 0) ^ snap.reverse_video;

            nisaba::Color fg_col = inv
                                       ? palette.resolve_bg(sg.bg)
                                       : palette.resolve_fg(sg.fg, bold, dim);

            const float gx = origin_x + static_cast<float>(sg.visual_col) * cw + sg.x_offset;
            const float gy = row_y + sg.y_offset;

            if (sg.is_procedural_box) {
                procedural_items.push_back({sg.fallback_cp, gx, gy, cw * sg.cell_span, ch, fg_col, false});
                continue;
            }
            if (sg.use_bitmap_fallback || !sg.font) {
                procedural_items.push_back({sg.fallback_cp, gx, gy, cw, ch, fg_col, true});
                continue;
            }

            const float baseline_y = gy + m.baseline;
            int32_t int_x = 0, int_y = 0;
            const auto* entry = atlas_.ensure_glyph(
                *ctx, *sg.font, sg.font_id, sg.glyph_id, m.font_size,
                gx, baseline_y, sg.synthetic_bold, sg.synthetic_italic,
                int_x, int_y);
            if (!entry || entry->width == 0 || entry->height == 0) continue;

            const float qx0 = static_cast<float>(int_x + entry->offsetX);
            const float qy0 = static_cast<float>(int_y + entry->offsetY);
            const float qx1 = qx0 + static_cast<float>(entry->width);
            const float qy1 = qy0 + static_cast<float>(entry->height);
            push_textured_quad(glyph_verts_, qx0, qy0, qx1, qy1,
                               entry->u0, entry->v0, entry->u1, entry->v1,
                               pack_premul_rgba8(fg_col));
        }

        // ── Pass 3: Decorations (Underlines / Strike / Overline / Hyperlink hover) ──
        for (int vc = 0; vc < line_cols; ++vc) {
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

            auto eff_u = ustyle;
            nisaba::Color ucol = fg_col;
            if (!vt::is_color_default(ext.underline_color)) {
                ucol = palette.resolve_fg(ext.underline_color, false, false);
            }
            if (ext.hyperlink_id != 0 && ext.hyperlink_id == overlay.hovered_hyperlink_id) {
                eff_u = vt::UnderlineStyle::Single;
                ucol = palette.cursor_color;
            }

            const uint32_t u_rgba = pack_premul_rgba8(ucol);
            const float uy = row_y + m.underline_y;
            const float ut = m.underline_thickness;

            switch (eff_u) {
                case vt::UnderlineStyle::Single:
                    push_solid_quad(decor_verts_, cx, uy, cx + cw, uy + ut, u_rgba);
                    break;
                case vt::UnderlineStyle::Double:
                    push_solid_quad(decor_verts_, cx, uy - ut, cx + cw, uy, u_rgba);
                    push_solid_quad(decor_verts_, cx, uy + ut, cx + cw, uy + ut * 2.0f, u_rgba);
                    break;
                case vt::UnderlineStyle::Dotted:
                    for (float dx = 0.0f; dx < cw; dx += ut * 2.5f) {
                        push_solid_quad(decor_verts_, cx + dx, uy, std::min(cx + cw, cx + dx + ut), uy + ut, u_rgba);
                    }
                    break;
                case vt::UnderlineStyle::Dashed:
                    for (float dx = 0.0f; dx < cw; dx += ut * 5.0f) {
                        push_solid_quad(decor_verts_, cx + dx, uy, std::min(cx + cw, cx + dx + ut * 3.0f), uy + ut, u_rgba);
                    }
                    break;
                case vt::UnderlineStyle::Curly:
                    curly_underlines.push_back({cx, uy, cw, ucol});
                    break;
                default:
                    break;
            }

            if (strike) {
                push_solid_quad(decor_verts_, cx, row_y + m.strike_y, cx + cw, row_y + m.strike_y + ut,
                                pack_premul_rgba8(fg_col));
            }
            if (over) {
                push_solid_quad(decor_verts_, cx, row_y + 0.5f, cx + cw, row_y + 0.5f + ut,
                                pack_premul_rgba8(fg_col));
            }
        }
    }

    // Add Cursor to decor pass
    if (snap.cursor_visible && (!snap.cursor_blink || overlay.cursor_blink_on || !overlay.pane_focused)) {
        const float cx = origin_x + static_cast<float>(snap.cursor.col) * cw;
        const float cy = origin_y + static_cast<float>(snap.cursor.row) * ch;
        const uint32_t c_rgba = pack_premul_rgba8(
            overlay.pane_focused ? obsidian::ui::detail::with_alpha(palette.cursor_color, 0.82f)
                                 : obsidian::ui::detail::with_alpha(palette.cursor_color, 0.45f));

        if (!overlay.pane_focused) {
            const float bw = 1.5f;
            push_solid_quad(decor_verts_, cx, cy, cx + cw, cy + bw, c_rgba);
            push_solid_quad(decor_verts_, cx, cy + ch - bw, cx + cw, cy + ch, c_rgba);
            push_solid_quad(decor_verts_, cx, cy, cx + bw, cy + ch, c_rgba);
            push_solid_quad(decor_verts_, cx + cw - bw, cy, cx + cw, cy + ch, c_rgba);
        } else if (snap.cursor_shape == vt::CursorShape::Bar) {
            push_solid_quad(decor_verts_, cx, cy, cx + std::max(2.0f, cw * 0.18f), cy + ch, c_rgba);
        } else if (snap.cursor_shape == vt::CursorShape::Underline) {
            const float uh = std::max(2.0f, ch * 0.14f);
            push_solid_quad(decor_verts_, cx, cy + ch - uh, cx + cw, cy + ch, c_rgba);
        } else {
            push_solid_quad(decor_verts_, cx, cy, cx + cw, cy + ch, c_rgba);
        }
    }

    // ── Flush Pass 1: Background Runs (single GPU call) ──
    if (!bg_verts_.empty()) {
        nisaba::gpu::Paint p;
        p.innerColor = nisaba::gpu::Color(1.0f, 1.0f, 1.0f, 1.0f);
        p.outerColor = nisaba::gpu::Color(1.0f, 1.0f, 1.0f, 1.0f);
        ctx->drawTriangles(p, bg_verts_.data(), static_cast<int>(bg_verts_.size()), 0.0f, 4);
    }

    // ── Flush Pass 2: Atlas Glyphs (single GPU call) ──
    if (!glyph_verts_.empty()) {
        TerminalAtlas::flush_if_dirty(*ctx);
        nisaba::gpu::Paint gp;
        gp.image = ctx->fontTextureId();
        gp.innerColor = nisaba::gpu::Color(1.0f, 1.0f, 1.0f, 1.0f);
        gp.outerColor = nisaba::gpu::Color(1.0f, 1.0f, 1.0f, 1.0f);
        ctx->drawTriangles(gp, glyph_verts_.data(), static_cast<int>(glyph_verts_.size()), 1.0f, 3);
    }

    // Procedural box-drawing / Powerline / Braille / 8x16 fallback glyphs
    for (const auto& pi : procedural_items) {
        if (pi.is_bitmap) {
            TerminalAtlas::draw_bitmap_fallback(canvas, pi.cp, pi.x, pi.y, pi.w, pi.h, pi.col);
        } else {
            TerminalAtlas::draw_procedural_symbol(canvas, pi.cp, pi.x, pi.y, pi.w, pi.h, pi.col);
        }
    }

    // ── Flush Pass 3: Decorations & Cursor (single GPU call) ──
    if (!decor_verts_.empty()) {
        nisaba::gpu::Paint dp;
        dp.innerColor = nisaba::gpu::Color(1.0f, 1.0f, 1.0f, 1.0f);
        dp.outerColor = nisaba::gpu::Color(1.0f, 1.0f, 1.0f, 1.0f);
        ctx->drawTriangles(dp, decor_verts_.data(), static_cast<int>(decor_verts_.size()), 0.0f, 4);
    }

    // Curly underlines (vector paths)
    for (const auto& cu : curly_underlines) {
        nisaba::PathBuilder pb;
        const float amp = std::max(1.3f, m.underline_thickness * 1.15f);
        const float step = std::max(2.0f, cu.w * 0.25f);
        pb.move_to(cu.x, cu.y);
        bool up = true;
        for (float x = cu.x + step; x <= cu.x + cu.w + 0.5f; x += step) {
            pb.line_to(std::min(x, cu.x + cu.w), cu.y + (up ? -amp : amp));
            up = !up;
        }
        if (auto path = pb.finish()) {
            nisaba::Stroke st;
            st.width = m.underline_thickness;
            canvas.stroke_path(*path, nisaba::Paint(cu.col), st);
        }
    }

    // Inline images with z_index >= 0 render over text
    render_inline_images(canvas, bounds, snap, terminal, m, false);

    canvas.restore();
}

} // namespace vater::render

