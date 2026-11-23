/// @file atlas.cpp
/// @brief L4 Renderer — Terminal GPU Glyph Atlas & Procedural Box/Block/Braille/Powerline Renderer.

#include "render/atlas.hpp"

#include "nisaba/canvas/paint.hpp"
#include "nisaba/path/path_builder.hpp"
#include "nisaba/text/font_8x16.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vater::render {

const nisaba::gpu::AtlasGlyphEntry* TerminalAtlas::ensure_glyph(
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
    int32_t& out_int_y) {
    auto [int_x, x_bin] = nisaba::text::compute_subpixel_bin(target_x);
    auto [int_y, y_bin] = nisaba::text::compute_subpixel_bin(target_y);
    out_int_x = int_x;
    out_int_y = int_y;

    uint32_t size_bits = 0;
    std::memcpy(&size_bits, &font_size, sizeof(float));
    const uint8_t style_flags = static_cast<uint8_t>((synth_bold ? 1u : 0u) | (synth_italic ? 2u : 0u));

    nisaba::text::CacheKey key{font_id, glyph_id, size_bits, x_bin, y_bin, style_flags, 0};
    auto& atlas = ctx.atlas();
    if (const auto* entry = atlas.find(key)) {
        return entry;
    }

    const nisaba::text::CachedGlyph* cg = ctx.glyphCache().get_or_render(
        font, font_id, glyph_id, font_size, target_x, target_y, synth_bold, synth_italic);
    if (!cg) return nullptr;

    auto inserted = atlas.insert(key, cg);
    if (!inserted) {
        if (auto* r = ctx.renderer()) {
            r->flush();
            if (atlas.width() < 2048 || atlas.height() < 2048) {
                atlas.resize(2048, 2048);
                ctx.glyphCache().clear();
            } else {
                atlas.reset();
                ctx.glyphCache().clear();
            }
        }
        inserted = atlas.insert(key, cg);
    }
    return atlas.find(key);
}

void TerminalAtlas::flush_if_dirty(nisaba::gpu::Context& ctx) {
    auto& atlas = ctx.atlas();
    if (atlas.isDirty() && ctx.renderer() && ctx.fontTextureId() != 0) {
        atlas.flushToGpu(ctx.renderer(), ctx.fontTextureId());
    }
}

void TerminalAtlas::draw_bitmap_fallback(nisaba::ICanvas& canvas,
                                         char32_t cp,
                                         float x, float y,
                                         float cell_w, float cell_h,
                                         const nisaba::Color& color) {
    const char ch = (cp >= 32 && cp <= 126) ? static_cast<char>(cp) : '?';
    const uint8_t* rows = nisaba::font::get_glyph_bitmap(ch);
    const float pw = cell_w / 8.0f;
    const float ph = cell_h / 16.0f;
    nisaba::Paint paint(color);

    for (int r = 0; r < 16; ++r) {
        const uint8_t bits = rows[r];
        if (bits == 0) continue;
        for (int c = 0; c < 8; ++c) {
            if (bits & (0x80u >> c)) {
                if (auto rect = nisaba::Rect::from_xywh(
                        x + static_cast<float>(c) * pw,
                        y + static_cast<float>(r) * ph,
                        std::ceil(pw), std::ceil(ph))) {
                    canvas.fill_rect(*rect, paint);
                }
            }
        }
    }
}

void TerminalAtlas::draw_procedural_symbol(nisaba::ICanvas& canvas,
                                           char32_t cp,
                                           float x, float y,
                                           float cell_w, float cell_h,
                                           const nisaba::Color& color) {
    nisaba::Paint paint(color);
    const float cx = x + std::floor(cell_w * 0.5f);
    const float cy = y + std::floor(cell_h * 0.5f);
    const float t_light = std::max(1.0f, std::round(cell_w * 0.12f));
    const float t_heavy = std::max(2.0f, std::round(cell_w * 0.24f));

    auto fill_box = [&](float rx, float ry, float rw, float rh) {
        if (rw > 0.0f && rh > 0.0f) {
            if (auto r = nisaba::Rect::from_xywh(rx, ry, rw, rh)) {
                canvas.fill_rect(*r, paint);
            }
        }
    };

    // ── Braille Patterns (U+2800..U+28FF) ──
    if (cp >= 0x2800u && cp <= 0x28FFu) {
        const uint8_t bits = static_cast<uint8_t>(cp - 0x2800u);
        if (bits == 0) return;
        const float dot_w = std::max(1.5f, std::round(cell_w * 0.24f));
        const float dot_h = std::max(1.5f, std::round(cell_h * 0.115f));
        const float cols_x[2] = {
            std::round(x + cell_w * 0.28f - dot_w * 0.5f),
            std::round(x + cell_w * 0.72f - dot_w * 0.5f)
        };
        const float rows_y[4] = {
            std::round(y + cell_h * 0.14f - dot_h * 0.5f),
            std::round(y + cell_h * 0.38f - dot_h * 0.5f),
            std::round(y + cell_h * 0.62f - dot_h * 0.5f),
            std::round(y + cell_h * 0.86f - dot_h * 0.5f),
        };
        static constexpr uint8_t kDotPos[8][2] = {
            {0, 0}, {0, 1}, {0, 2}, {1, 0}, {1, 1}, {1, 2}, {0, 3}, {1, 3}
        };
        for (int i = 0; i < 8; ++i) {
            if (bits & (1u << i)) {
                fill_box(cols_x[kDotPos[i][0]], rows_y[kDotPos[i][1]], dot_w, dot_h);
            }
        }
        return;
    }

    // ── Powerline Triangles (U+E0B0..U+E0B3) ──
    if (cp >= 0xE0B0u && cp <= 0xE0B3u) {
        nisaba::PathBuilder pb;
        if (cp == 0xE0B0u) { // Filled right triangle
            pb.move_to(x, y);
            pb.line_to(x + cell_w, cy);
            pb.line_to(x, y + cell_h);
            pb.close();
            if (auto path = pb.finish()) canvas.fill_path(*path, paint);
        } else if (cp == 0xE0B2u) { // Filled left triangle
            pb.move_to(x + cell_w, y);
            pb.line_to(x, cy);
            pb.line_to(x + cell_w, y + cell_h);
            pb.close();
            if (auto path = pb.finish()) canvas.fill_path(*path, paint);
        } else if (cp == 0xE0B1u) { // Outline right chevron
            pb.move_to(x, y);
            pb.line_to(x + cell_w, cy);
            pb.line_to(x, y + cell_h);
            if (auto path = pb.finish()) {
                nisaba::Stroke st;
                st.width = t_light;
                canvas.stroke_path(*path, paint, st);
            }
        } else if (cp == 0xE0B3u) { // Outline left chevron
            pb.move_to(x + cell_w, y);
            pb.line_to(x, cy);
            pb.line_to(x + cell_w, y + cell_h);
            if (auto path = pb.finish()) {
                nisaba::Stroke st;
                st.width = t_light;
                canvas.stroke_path(*path, paint, st);
            }
        }
        return;
    }

    // ── Block Elements (U+2580..U+259F) ──
    if (cp >= 0x2580u && cp <= 0x259Fu) {
        if (cp == 0x2580u) { fill_box(x, y, cell_w, cell_h * 0.5f); return; } // Upper half
        if (cp >= 0x2581u && cp <= 0x2588u) { // Lower 1/8..8/8
            const float frac = static_cast<float>(cp - 0x2580u) / 8.0f;
            const float h = cell_h * frac;
            fill_box(x, y + cell_h - h, cell_w, h);
            return;
        }
        if (cp >= 0x2589u && cp <= 0x258Fu) { // Left 7/8..1/8
            const float frac = static_cast<float>(0x2590u - cp) / 8.0f;
            fill_box(x, y, cell_w * frac, cell_h);
            return;
        }
        if (cp == 0x2590u) { fill_box(x + cell_w * 0.5f, y, cell_w * 0.5f, cell_h); return; } // Right half
        if (cp == 0x2591u || cp == 0x2592u || cp == 0x2593u) { // Light/Medium/Dark shade
            const float alpha = (cp == 0x2591u) ? 0.25f : ((cp == 0x2592u) ? 0.50f : 0.75f);
            nisaba::Paint sp(nisaba::Color::from_rgba_unchecked(
                color.red(), color.green(), color.blue(), color.alpha() * alpha));
            if (auto r = nisaba::Rect::from_xywh(x, y, cell_w, cell_h)) canvas.fill_rect(*r, sp);
            return;
        }
        if (cp == 0x2594u) { fill_box(x, y, cell_w, cell_h / 8.0f); return; }
        if (cp == 0x2595u) { fill_box(x + cell_w * 7.0f / 8.0f, y, cell_w / 8.0f, cell_h); return; }
        // Quadrants (U+2596..U+259F)
        const float hw = cell_w * 0.5f;
        const float hh = cell_h * 0.5f;
        auto quad = [&](bool ul, bool ur, bool ll, bool lr) {
            if (ul) fill_box(x, y, hw, hh);
            if (ur) fill_box(x + hw, y, hw, hh);
            if (ll) fill_box(x, y + hh, hw, hh);
            if (lr) fill_box(x + hw, y + hh, hw, hh);
        };
        switch (cp) {
            case 0x2596u: quad(0,0,1,0); return;
            case 0x2597u: quad(0,0,0,1); return;
            case 0x2598u: quad(1,0,0,0); return;
            case 0x2599u: quad(1,0,1,1); return;
            case 0x259Au: quad(1,0,0,1); return;
            case 0x259Bu: quad(1,1,1,0); return;
            case 0x259Cu: quad(1,1,0,1); return;
            case 0x259Du: quad(0,1,0,0); return;
            case 0x259Eu: quad(0,1,1,0); return;
            case 0x259Fu: quad(0,1,1,1); return;
            default: break;
        }
    }

    // ── Rounded Box Corners (U+256D..U+2570: ╭ ╮ ╯ ╰) ──
    if (cp >= 0x256Du && cp <= 0x2570u) {
        nisaba::PathBuilder pb;
        if (cp == 0x256Du) { // ╭ right + down
            pb.move_to(x + cell_w, cy);
            pb.quad_to(cx, cy, cx, y + cell_h);
        } else if (cp == 0x256Eu) { // ╮ left + down
            pb.move_to(x, cy);
            pb.quad_to(cx, cy, cx, y + cell_h);
        } else if (cp == 0x256Fu) { // ╯ left + up
            pb.move_to(x, cy);
            pb.quad_to(cx, cy, cx, y);
        } else { // ╰ right + up
            pb.move_to(x + cell_w, cy);
            pb.quad_to(cx, cy, cx, y);
        }
        if (auto path = pb.finish()) {
            nisaba::Stroke st;
            st.width = t_light;
            canvas.stroke_path(*path, paint, st);
        }
        return;
    }

    // ── Box Drawing (U+2500..U+257F) ──
    // Arms: 0=none, 1=light, 2=heavy, 3=double
    uint8_t left = 0, right = 0, up = 0, down = 0;
    switch (cp) {
        case 0x2500u: left = right = 1; break; // ─
        case 0x2501u: left = right = 2; break; // ━
        case 0x2502u: up = down = 1; break;    // │
        case 0x2503u: up = down = 2; break;    // ┃
        case 0x250Cu: right = down = 1; break; // ┌
        case 0x250Du: right = 2; down = 1; break; // ┍
        case 0x250Eu: right = 1; down = 2; break; // ┎
        case 0x250Fu: right = down = 2; break; // ┏
        case 0x2510u: left = down = 1; break;  // ┐
        case 0x2511u: left = 2; down = 1; break;  // ┑
        case 0x2512u: left = 1; down = 2; break;  // ┒
        case 0x2513u: left = down = 2; break;  // ┓
        case 0x2514u: right = up = 1; break;   // └
        case 0x2515u: right = 2; up = 1; break;   // ┕
        case 0x2516u: right = 1; up = 2; break;   // ┖
        case 0x2517u: right = up = 2; break;   // ┗
        case 0x2518u: left = up = 1; break;    // ┘
        case 0x2519u: left = 2; up = 1; break;    // ┙
        case 0x251Au: left = 1; up = 2; break;    // ┚
        case 0x251Bu: left = up = 2; break;    // ┛
        case 0x251Cu: up = down = right = 1; break; // ├
        case 0x251Du: up = down = 1; right = 2; break; // ┝
        case 0x251Eu: up = 2; down = right = 1; break; // ┞
        case 0x251Fu: down = 2; up = right = 1; break; // ┟
        case 0x2520u: up = down = 2; right = 1; break; // ┠
        case 0x2521u: up = right = 2; down = 1; break; // ┡
        case 0x2522u: down = right = 2; up = 1; break; // ┢
        case 0x2523u: up = down = right = 2; break; // ┣
        case 0x2524u: up = down = left = 1; break;  // ┤
        case 0x2525u: up = down = 1; left = 2; break;  // ┥
        case 0x2526u: up = 2; down = left = 1; break;  // ┦
        case 0x2527u: down = 2; up = left = 1; break;  // ┧
        case 0x2528u: up = down = 2; left = 1; break;  // ┨
        case 0x2529u: up = left = 2; down = 1; break;  // ┩
        case 0x252Au: down = left = 2; up = 1; break;  // ┪
        case 0x252Bu: up = down = left = 2; break;  // ┫
        case 0x252Cu: left = right = down = 1; break; // ┬
        case 0x252Du: left = 2; right = down = 1; break; // ┭
        case 0x252Eu: right = 2; left = down = 1; break; // ┮
        case 0x252Fu: left = right = 2; down = 1; break; // ┯
        case 0x2530u: down = 2; left = right = 1; break; // ┰
        case 0x2531u: left = down = 2; right = 1; break; // ┱
        case 0x2532u: right = down = 2; left = 1; break; // ┲
        case 0x2533u: left = right = down = 2; break; // ┳
        case 0x2534u: left = right = up = 1; break;   // ┴
        case 0x2535u: left = 2; right = up = 1; break;   // ┵
        case 0x2536u: right = 2; left = up = 1; break;   // ┶
        case 0x2537u: left = right = 2; up = 1; break;   // ┷
        case 0x2538u: up = 2; left = right = 1; break;   // ┸
        case 0x2539u: left = up = 2; right = 1; break;   // ┹
        case 0x253Au: right = up = 2; left = 1; break;   // ┺
        case 0x253Bu: left = right = up = 2; break;   // ┻
        case 0x253Cu: left = right = up = down = 1; break; // ┼
        case 0x253Du: left = 2; right = up = down = 1; break;
        case 0x253Eu: right = 2; left = up = down = 1; break;
        case 0x253Fu: left = right = 2; up = down = 1; break;
        case 0x2540u: up = 2; left = right = down = 1; break;
        case 0x2541u: down = 2; left = right = up = 1; break;
        case 0x2542u: up = down = 2; left = right = 1; break;
        case 0x254Bu: left = right = up = down = 2; break; // ╋
        case 0x2550u: left = right = 3; break; // ═
        case 0x2551u: up = down = 3; break;    // ║
        case 0x2552u: right = 3; down = 1; break; // ╒
        case 0x2553u: right = 1; down = 3; break; // ╓
        case 0x2554u: right = down = 3; break; // ╔
        case 0x2555u: left = 3; down = 1; break;  // ╕
        case 0x2556u: left = 1; down = 3; break;  // ╖
        case 0x2557u: left = down = 3; break;  // ╗
        case 0x2558u: right = 3; up = 1; break;   // ╘
        case 0x2559u: right = 1; up = 3; break;   // ╙
        case 0x255Au: right = up = 3; break;   // ╚
        case 0x255Bu: left = 3; up = 1; break;    // ╛
        case 0x255Cu: left = 1; up = 3; break;    // ╜
        case 0x255Du: left = up = 3; break;    // ╝
        case 0x255Eu: up = down = 1; right = 3; break; // ╞
        case 0x255Fu: up = down = 3; right = 1; break; // ╟
        case 0x2560u: up = down = right = 3; break; // ╠
        case 0x2561u: up = down = 1; left = 3; break;  // ╡
        case 0x2562u: up = down = 3; left = 1; break;  // ╢
        case 0x2563u: up = down = left = 3; break;  // ╣
        case 0x2564u: left = right = 3; down = 1; break; // ╤
        case 0x2565u: left = right = 1; down = 3; break; // ╥
        case 0x2566u: left = right = down = 3; break; // ╦
        case 0x2567u: left = right = 3; up = 1; break;   // ╧
        case 0x2568u: left = right = 1; up = 3; break;   // ╨
        case 0x2569u: left = right = up = 3; break;   // ╩
        case 0x256Au: left = right = 3; up = down = 1; break; // ╪
        case 0x256Bu: left = right = 1; up = down = 3; break; // ╫
        case 0x256Cu: left = right = up = down = 3; break; // ╬
        case 0x2574u: left = 1; break;
        case 0x2575u: up = 1; break;
        case 0x2576u: right = 1; break;
        case 0x2577u: down = 1; break;
        case 0x2578u: left = 2; break;
        case 0x2579u: up = 2; break;
        case 0x257Au: right = 2; break;
        case 0x257Bu: down = 2; break;
        case 0x257Cu: left = 1; right = 2; break;
        case 0x257Du: up = 1; down = 2; break;
        case 0x257Eu: left = 2; right = 1; break;
        case 0x257Fu: up = 2; down = 1; break;
        default:
            left = right = 1;
            break;
    }

    auto arm_w = [&](uint8_t s) { return (s == 2) ? t_heavy : t_light; };
    const float dbl_gap = std::max(1.5f, t_light * 1.4f);

    if (left == 3) {
        fill_box(x, cy - dbl_gap, cx - x + t_light * 0.5f, t_light);
        fill_box(x, cy + dbl_gap - t_light, cx - x + t_light * 0.5f, t_light);
    } else if (left > 0) {
        const float t = arm_w(left);
        fill_box(x, cy - t * 0.5f, cx - x + t * 0.5f, t);
    }

    if (right == 3) {
        fill_box(cx - t_light * 0.5f, cy - dbl_gap, (x + cell_w) - cx + t_light * 0.5f, t_light);
        fill_box(cx - t_light * 0.5f, cy + dbl_gap - t_light, (x + cell_w) - cx + t_light * 0.5f, t_light);
    } else if (right > 0) {
        const float t = arm_w(right);
        fill_box(cx - t * 0.5f, cy - t * 0.5f, (x + cell_w) - cx + t * 0.5f, t);
    }

    if (up == 3) {
        fill_box(cx - dbl_gap, y, t_light, cy - y + t_light * 0.5f);
        fill_box(cx + dbl_gap - t_light, y, t_light, cy - y + t_light * 0.5f);
    } else if (up > 0) {
        const float t = arm_w(up);
        fill_box(cx - t * 0.5f, y, t, cy - y + t * 0.5f);
    }

    if (down == 3) {
        fill_box(cx - dbl_gap, cy - t_light * 0.5f, t_light, (y + cell_h) - cy + t_light * 0.5f);
        fill_box(cx + dbl_gap - t_light, cy - t_light * 0.5f, t_light, (y + cell_h) - cy + t_light * 0.5f);
    } else if (down > 0) {
        const float t = arm_w(down);
        fill_box(cx - t * 0.5f, cy - t * 0.5f, t, (y + cell_h) - cy + t * 0.5f);
    }
}

} // namespace vater::render

