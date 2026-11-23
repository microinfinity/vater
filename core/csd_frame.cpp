/// @file csd_frame.cpp
/// @brief Exact 1:1 Sovereign Client-Side Decoration (CSD) Window Frame & Window Controls.
/// Ported directly from `window_controls.cpp` and `window_header.cpp`.

#include "csd_frame.hpp"
#include "nisaba/canvas/paint.hpp"
#include "nisaba/canvas/pixmap.hpp"
#include "nisaba/shaders/shader.hpp"
#include "nisaba/svg/svg.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace obsidian::ui {

namespace {

inline constexpr std::string_view kWindowControlCloseSvg = R"svg(<svg width="14" height="14" viewBox="0 0 14 14" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M3.967 3.967L10.033 10.033M10.033 3.967L3.967 10.033" stroke="currentColor" stroke-width="1.12" stroke-linecap="round"/></svg>)svg";
inline constexpr std::string_view kWindowControlMinimizeSvg = R"svg(<svg width="14" height="14" viewBox="0 0 14 14" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M3.267 6.4H10.733" stroke="currentColor" stroke-width="1.12" stroke-linecap="round"/></svg>)svg";
inline constexpr std::string_view kWindowControlMaximizeSvg = R"svg(<svg width="14" height="14" viewBox="0 0 14 14" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M7.5 3.6H10.4V6.5M6.5 10.4H3.6V7.5" stroke="currentColor" stroke-width="1.12" stroke-linecap="round" stroke-linejoin="round"/></svg>)svg";
inline constexpr std::string_view kWindowControlRestoreSvg = R"svg(<svg width="14" height="14" viewBox="0 0 14 14" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M8.45 3.05V5.55H10.95M5.55 10.95V8.45H3.05" stroke="currentColor" stroke-width="1.2" stroke-linecap="round" stroke-linejoin="round"/></svg>)svg";
inline constexpr std::string_view kWindowControlDialogCloseSvg = R"svg(<svg width="22" height="22" viewBox="0 0 22 22" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M7.75 7.75L14.25 14.25M14.25 7.75L7.75 14.25" stroke="currentColor" stroke-width="1.25" stroke-linecap="round"/></svg>)svg";

void draw_window_control_glyph(nisaba::gpu::GpuCanvas& canvas, std::string_view artwork,
                               const nisaba::Rect& bounds, nisaba::Color col) {
    if (artwork.empty()) return;
    std::string_view xml;
    if (artwork == "close") xml = kWindowControlCloseSvg;
    else if (artwork == "minimize") xml = kWindowControlMinimizeSvg;
    else if (artwork == "maximize") xml = kWindowControlMaximizeSvg;
    else if (artwork == "restore") xml = kWindowControlRestoreSvg;
    else if (artwork == "dialog-close") xml = kWindowControlDialogCloseSvg;
    else return;

    static nisaba::svg::SvgCache cache(64);
    static std::unordered_map<std::string, nisaba::svg::SvgDocument> docs;
    std::string k(artwork);
    auto it = docs.find(k);
    if (it == docs.end()) {
        auto parsed = nisaba::svg::SvgDocument::parse(xml);
        if (!parsed) return;
        it = docs.emplace(std::move(k), std::move(*parsed)).first;
    }
    nisaba::svg::SvgTheme svg_theme;
    svg_theme.current_color = col;
    svg_theme.tint_color = col;
    cache.draw(canvas, it->second, bounds, svg_theme, 1.0f);
}

// Exact EdgeLight tables from window_controls.cpp:50-75
struct EdgeLight {
    double depth;
    double added;
};

constexpr EdgeLight kDarkTopLight[] = {
    {0.25, 58.0}, {0.75, 38.0}, {1.25, 14.0}, {1.75, 11.0}, {2.25, 8.0},
    {2.75, 7.0},  {3.25, 5.5},  {3.75, 4.5},  {4.25, 3.0},  {5.25, 3.0},
    {6.25, 2.0},  {7.25, 1.0},  {8.00, 0.0}
};

constexpr EdgeLight kDarkBottomLight[] = {
    {0.25, 82.0}, {0.75, 41.0}, {1.25, 11.0}, {1.75, 9.0},  {2.25, 7.0},
    {2.75, 5.0},  {3.25, 4.0},  {4.25, 3.0},  {5.25, 2.0},  {6.25, 1.0},
    {8.00, 0.0}
};

template <size_t N>
double edge_light_at(const EdgeLight (&table)[N], double depth) noexcept {
    if (depth <= table[0].depth) return table[0].added;
    for (size_t i = 1; i < N; ++i) {
        if (depth <= table[i].depth) {
            const double t = (depth - table[i - 1].depth) / (table[i].depth - table[i - 1].depth);
            return table[i - 1].added + t * (table[i].added - table[i - 1].added);
        }
    }
    return 0.0;
}

struct EdgePixel {
    double grey;
    double alpha;
};

// Exact per-device-pixel edge generator from window_controls.cpp:89-188
// Cached by (device_side, dark) so it computes once and blits in O(1) on the GPU.
const nisaba::Pixmap& get_cached_plate_edge(int device_side, double scale, bool dark) {
    static std::unordered_map<uint64_t, nisaba::Pixmap> cache;
    const uint64_t key = (static_cast<uint64_t>(device_side) << 1) | (dark ? 1ULL : 0ULL);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    const int width = device_side + 2;
    const int height = device_side + 2;
    auto pix_opt = nisaba::Pixmap::allocate(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    nisaba::Pixmap pix = std::move(*pix_opt);
    pix.fill(nisaba::Color::TRANSPARENT);

    const double halfWidth = device_side / 2.0;
    const double halfHeight = device_side / 2.0;
    const double radius = halfHeight;
    const double lineWidth = 0.5 * scale;
    const double reach = dark ? (8.0 * scale) : (lineWidth + 1.0);

    auto to_byte = [](double v) -> uint8_t {
        return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
    };

    auto mut = pix.as_mut();
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const double px = x - 1 + 0.5 - halfWidth;
            const double py = y - 1 + 0.5 - halfHeight;
            const double qx = std::abs(px) - (halfWidth - radius);
            const double qy = std::abs(py) - (halfHeight - radius);
            double distance = 0.0;
            double upright = 0.0;
            if (qx > 0.0 && qy > 0.0) {
                const double length = std::hypot(qx, qy);
                distance = length - radius;
                upright = (qy * qy) / (length * length);
            } else if (qx > qy) {
                distance = qx - radius;
                upright = 0.0;
            } else {
                distance = qy - radius;
                upright = 1.0;
            }

            if (distance > 1.5 || -distance > reach) continue;

            const double down = (py + halfHeight) / static_cast<double>(device_side);
            EdgePixel shown{0.0, 0.0};

            if (dark) {
                // paintDarkEdge (window_controls.cpp:139-175):
                // Half-point dark line just outside the sides, and specular top/bottom edge light inside
                const double band = std::clamp(std::min(distance + 0.5, lineWidth) - std::max(distance - 0.5, 0.0), 0.0, 1.0);
                const double dark_line = band * 0.65 * (1.0 - upright);

                const double inside = std::clamp(0.5 - distance, 0.0, 1.0);
                const double depth = std::max(-distance, 0.0) / scale;
                const double light = inside * upright *
                    (down < 0.5 ? edge_light_at(kDarkTopLight, depth) / (255.0 - 50.0)
                                : edge_light_at(kDarkBottomLight, depth) / (255.0 - 67.0));

                shown.grey = light * (1.0 - dark_line);
                shown.alpha = dark_line + light * (1.0 - dark_line);
            } else {
                // paintLightRim (window_controls.cpp:179-188):
                // Half-point rim just inside outline, darkest at the middle of its height
                const double band = std::clamp(std::min(distance + 0.5, 0.0) - std::max(distance - 0.5, -lineWidth), 0.0, 1.0);
                const double alpha = (down < 0.5)
                    ? (0.10 + (0.32 - 0.10) * down / 0.5)
                    : (0.32 + (0.14 - 0.32) * (down - 0.5) / 0.5);
                shown.grey = 0.0;
                shown.alpha = band * alpha;
            }

            uint8_t g = to_byte(shown.grey);
            uint8_t a = to_byte(shown.alpha);
            if (g > a) g = a; // valid premultiplied RGBA
            mut.set_pixel(static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                          nisaba::PremultipliedColorU8::from_rgba_unchecked(g, g, g, a));
        }
    }

    auto [ins, _] = cache.emplace(key, std::move(pix));
    return ins->second;
}

} // namespace

CsdFrame::CsdFrame() = default;

void CsdFrame::set_window_and_platform(nisaba::backend_os::Window* win, nisaba::backend_os::Platform* plat) {
    window_ = win;
    platform_ = plat;

    if (window_) {
        is_maximized_ = window_->isMaximized();
        is_focused_ = window_->isActivated();

        window_->onMaximized().connect([this](bool max) {
            is_maximized_ = max;
        });

        window_->onFocus().connect([this](bool focus) {
            is_focused_ = focus;
        });

        window_->onStateChanged().connect([this](nisaba::backend_os::WindowState state) {
            is_maximized_ = nisaba::backend_os::hasWindowState(state, nisaba::backend_os::WindowState::Maximized);
            is_focused_ = nisaba::backend_os::hasWindowState(state, nisaba::backend_os::WindowState::Activated);
        });
    }
}

void CsdFrame::update(float /*dt*/) {
    if (window_) {
        is_maximized_ = window_->isMaximized();
        is_focused_ = window_->isActivated();
    }
}

bool CsdFrame::is_maximized() const noexcept {
    return is_maximized_;
}

bool CsdFrame::is_focused() const noexcept {
    return is_focused_;
}

float CsdFrame::get_corner_radius(const AppTheme& theme, const UnitScale& scale) const noexcept {
    return is_maximized_ ? 0.0f : (theme.windowRadius * scale.u);
}

float CsdFrame::get_titlebar_height(const UnitScale& scale) const noexcept {
    return 48.0f * scale.u; // kHeaderHeight = 48 in original mainwindow.cpp
}

void CsdFrame::render_frame_background(nisaba::gpu::GpuCanvas& canvas, float width, float height,
                                       const AppTheme& theme, const UnitScale& scale,
                                       float window_opacity) {
    const float r = get_corner_radius(theme, scale);
    const float eff_op = std::clamp(window_opacity, 0.0f, 1.0f);
    auto win_rect = *nisaba::Rect::from_xywh(0.0f, 0.0f, width, height);
    nisaba::Color bg = detail::with_alpha(theme.background, eff_op);

    if (!is_maximized_) {
        if (eff_op > 0.002f) {
            canvas.fill_round_rect(win_rect, r, r, nisaba::Paint(bg));
        }

        uint8_t rim_a = static_cast<uint8_t>(std::round((theme.darkMode ? 24.0f : 22.0f) * std::clamp(eff_op / 0.30f, 0.0f, 1.0f)));
        if (rim_a > 0) {
            nisaba::Color border_col = theme.darkMode
                ? nisaba::Color::from_rgba8(255, 255, 255, rim_a)
                : nisaba::Color::from_rgba8(0, 0, 0, rim_a);
            canvas.stroke_round_rect(win_rect, r, r, nisaba::Paint(border_col), nisaba::Stroke{1.0f});
        }
    } else {
        if (eff_op > 0.002f) {
            canvas.fill_rect(win_rect, nisaba::Paint(bg));
        }
    }
}

void CsdFrame::paint_window_control(nisaba::gpu::GpuCanvas& canvas, const nisaba::Rect& bounds,
                                    const std::string& artwork, bool dark, bool active,
                                    bool enabled, bool hovered, bool pressed, float u) {
    const float side = std::round(std::min(bounds.width(), bounds.height()));
    if (side <= 0.0f) return;
    const float radius = side * 0.5f;
    const float px = std::round(bounds.left() + (bounds.width() - side) * 0.5f);
    const float py = std::round(bounds.top() + (bounds.height() - side) * 0.5f);
    auto plate = *nisaba::Rect::from_xywh(px, py, side, side);

    const float alpha_mult = enabled ? 1.0f : 0.35f;
    auto white_a = [alpha_mult](float a) -> nisaba::Color {
        return nisaba::Color::from_rgba_unchecked(1.0f, 1.0f, 1.0f, std::clamp(a * alpha_mult, 0.0f, 1.0f));
    };
    auto black_a = [alpha_mult](float a) -> nisaba::Color {
        return nisaba::Color::from_rgba_unchecked(0.0f, 0.0f, 0.0f, std::clamp(a * alpha_mult, 0.0f, 1.0f));
    };

    // 1. Outer shadow in Light Mode (window_controls.cpp:204-206: shadowSigma = side * 0.1, offset = side * 0.06, alpha = 0.04)
    if (!dark) {
        nisaba::effects::DropShadow sh(0.0f, side * 0.06f, side * 0.10f, black_a(0.04f));
        canvas.draw_round_rect_shadow(plate, radius, radius, sh);
    }

    // 2. Plate Body (window_controls.cpp:210-239)
    // Filled square inside the round clip so the outline's pixels take coverage once
    canvas.save();
    canvas.clip_rrect(plate, nisaba::BorderRadius::all(radius));

    const float body = dark ? 0.25f : 1.0f;

    // Horizontal middle glow: {0: white(body*0.04), 0.5: white(body*0.20), 1: white(body*0.04)}
    if (auto h_grad = nisaba::LinearGradient::create(
            nisaba::Point{plate.left(), plate.top()},
            nisaba::Point{plate.right(), plate.top()},
            {
                nisaba::GradientStop::create(0.0f, white_a(body * 0.04f)),
                nisaba::GradientStop::create(0.5f, white_a(body * 0.20f)),
                nisaba::GradientStop::create(1.0f, white_a(body * 0.04f)),
            })) {
        canvas.fill_rect(plate, nisaba::Paint(nisaba::Shader(*h_grad)));
    }

    // Vertical lower-half glow: {0.3: white(0), 0.75: white(body*0.32)}
    if (auto v_grad = nisaba::LinearGradient::create(
            nisaba::Point{plate.left(), plate.top()},
            nisaba::Point{plate.left(), plate.bottom()},
            {
                nisaba::GradientStop::create(0.30f, white_a(0.0f)),
                nisaba::GradientStop::create(0.75f, white_a(body * 0.32f)),
            })) {
        canvas.fill_rect(plate, nisaba::Paint(nisaba::Shader(*v_grad)));
    }

    // Light mode top/bottom inner highlight gradient (window_controls.cpp:222-234)
    if (!dark) {
        if (auto l_grad = nisaba::LinearGradient::create(
                nisaba::Point{plate.left(), plate.top()},
                nisaba::Point{plate.left(), plate.bottom()},
                {
                    nisaba::GradientStop::create(0.000f, white_a(1.0f)),
                    nisaba::GradientStop::create(0.035f, white_a(1.0f)),
                    nisaba::GradientStop::create(0.070f, white_a(0.50f)),
                    nisaba::GradientStop::create(0.120f, white_a(0.25f)),
                    nisaba::GradientStop::create(0.250f, white_a(0.0f)),
                    nisaba::GradientStop::create(0.850f, white_a(0.0f)),
                    nisaba::GradientStop::create(0.930f, white_a(0.30f)),
                    nisaba::GradientStop::create(0.955f, white_a(0.60f)),
                    nisaba::GradientStop::create(0.970f, white_a(1.0f)),
                    nisaba::GradientStop::create(1.000f, white_a(1.0f)),
                })) {
            canvas.fill_rect(plate, nisaba::Paint(nisaba::Shader(*l_grad)));
        }
    }

    // Hover / Pressed overlay (0.04 hovered, 0.08 pressed)
    const bool lit = enabled && (hovered || pressed);
    if (lit) {
        const float ov = pressed ? 0.08f : 0.04f;
        canvas.fill_rect(plate, nisaba::Paint(dark ? white_a(ov) : black_a(ov)));
    }
    canvas.restore();

    // 3. Per-pixel SDF Edge / Rim (paintDarkEdge in dark mode, paintLightRim in light mode)
    const int dev_side = std::max(4, static_cast<int>(std::lround(side)));
    const nisaba::Pixmap& edge_pix = get_cached_plate_edge(dev_side, static_cast<double>(u), dark);
    nisaba::PixmapPaint edge_paint;
    edge_paint.opacity = alpha_mult;
    canvas.draw_pixmap(static_cast<int32_t>(std::lround(plate.left())) - 1,
                       static_cast<int32_t>(std::lround(plate.top())) - 1,
                       edge_pix.as_ref(), edge_paint);

    // 4. SVG Artwork Glyph rendered via Nisaba SvgDocument & SvgCache
    // inkColor(dark, strong = active || hovered):
    //   (dark ? #FFFFFF : #0C0C0C).withAlpha(strong ? 1.0 : 0.5)
    if (!artwork.empty()) {
        const bool strong = active || hovered;
        const float ink_alpha = (strong ? 1.0f : 0.5f) * alpha_mult;
        const uint8_t ia = static_cast<uint8_t>(std::clamp(ink_alpha * 255.0f + 0.5f, 0.0f, 255.0f));
        const nisaba::Color ink = dark
            ? nisaba::Color::from_rgba8(255, 255, 255, ia)
            : nisaba::Color::from_rgba8(12, 12, 12, ia);

        if (artwork == "dialog-close") {
            draw_window_control_glyph(canvas, artwork, plate, ink);
        } else {
            const float art_sz = std::round(14.0f * side / 22.0f);
            const float ix = std::round(bounds.left() + (bounds.width() - art_sz) * 0.5f);
            const float iy = std::round(bounds.top() + (bounds.height() - art_sz) * 0.5f);
            auto icon_rect = *nisaba::Rect::from_xywh(ix, iy, art_sz, art_sz);
            draw_window_control_glyph(canvas, artwork, icon_rect, ink);
        }
    }
}

void CsdFrame::render_titlebar(nisaba::gpu::GpuCanvas& canvas, float width, float /*height*/,
                               const AppTheme& theme, const UnitScale& scale,
                               const std::string& page_title, float sidebar_width) {
    const float u = scale.u;
    const float tb_h = get_titlebar_height(scale);
    rect_titlebar_ = *nisaba::Rect::from_xywh(0.0f, 0.0f, width, tb_h);

    const float btn_size = 22.0f * u;
    const float btn_spacing = 6.0f * u;
    const float margin_x = 12.0f * u;
    const float row_y = 12.0f * u;

    // Top-right window controls: Close (rightmost), then Maximize, then Minimize
    rect_close_ = *nisaba::Rect::from_xywh(width - margin_x - btn_size, row_y, btn_size, btn_size);
    rect_max_   = *nisaba::Rect::from_xywh(width - margin_x - btn_size - (btn_size + btn_spacing), row_y, btn_size, btn_size);
    rect_min_   = *nisaba::Rect::from_xywh(width - margin_x - btn_size - 2.0f * (btn_size + btn_spacing), row_y, btn_size, btn_size);

    paint_window_control(canvas, rect_close_, "close", theme.darkMode, is_focused_, true,
                         hovered_button_ == CsdButtonType::Close,
                         pressed_button_ == CsdButtonType::Close, u);

    paint_window_control(canvas, rect_min_, "minimize", theme.darkMode, is_focused_, true,
                         hovered_button_ == CsdButtonType::Minimize,
                         pressed_button_ == CsdButtonType::Minimize, u);

    paint_window_control(canvas, rect_max_, is_maximized_ ? "restore" : "maximize",
                         theme.darkMode, is_focused_, true,
                         hovered_button_ == CsdButtonType::Maximize,
                         pressed_button_ == CsdButtonType::Maximize, u);

    // Page Title at x = kSidebarWidth + 12 = 252, baseline = 28, font size = points(11) = 14.67px
    const float page_x = (sidebar_width + 12.0f) * u;
    const float baseline_y = 28.0f * u;
    nisaba::Color title_col = is_focused_ ? theme.text : theme.disabledText;
    canvas.draw_text(page_x, baseline_y, page_title, nisaba::Paint(title_col), AppTheme::points(11.0f) * u, "sans");
}

nisaba::backend_os::WindowEdge CsdFrame::hit_test_edge(float x, float y, float width, float height, const UnitScale& scale) const {
    if (is_maximized_) return nisaba::backend_os::WindowEdge::NoneEdge;

    float t = 6.0f * scale.u;
    float c = 14.0f * scale.u;

    if (x < 0.0f || x > width || y < 0.0f || y > height) {
        return nisaba::backend_os::WindowEdge::NoneEdge;
    }

    if (x <= c && y <= c) return nisaba::backend_os::WindowEdge::TopLeft;
    if (x >= width - c && y <= c) return nisaba::backend_os::WindowEdge::TopRight;
    if (x <= c && y >= height - c) return nisaba::backend_os::WindowEdge::BottomLeft;
    if (x >= width - c && y >= height - c) return nisaba::backend_os::WindowEdge::BottomRight;

    if (y <= t) return nisaba::backend_os::WindowEdge::Top;
    if (y >= height - t) return nisaba::backend_os::WindowEdge::Bottom;
    if (x <= t) return nisaba::backend_os::WindowEdge::Left;
    if (x >= width - t) return nisaba::backend_os::WindowEdge::Right;

    return nisaba::backend_os::WindowEdge::NoneEdge;
}

bool CsdFrame::handle_mouse_move(float x, float y, float width, float height, const UnitScale& scale) {
    mouse_x_ = x;
    mouse_y_ = y;

    hovered_button_ = CsdButtonType::None;
    if (rect_contains(rect_close_, x, y)) {
        hovered_button_ = CsdButtonType::Close;
    } else if (rect_contains(rect_min_, x, y)) {
        hovered_button_ = CsdButtonType::Minimize;
    } else if (rect_contains(rect_max_, x, y)) {
        hovered_button_ = CsdButtonType::Maximize;
    }

    if (platform_ && !is_maximized_) {
        auto edge = hit_test_edge(x, y, width, height, scale);
        hovered_edge_ = edge;

        nisaba::backend_os::SystemCursor target_cursor = nisaba::backend_os::SystemCursor::Default;
        switch (edge) {
            case nisaba::backend_os::WindowEdge::Top:
            case nisaba::backend_os::WindowEdge::Bottom:
                target_cursor = nisaba::backend_os::SystemCursor::ResizeVertical;
                break;
            case nisaba::backend_os::WindowEdge::Left:
            case nisaba::backend_os::WindowEdge::Right:
                target_cursor = nisaba::backend_os::SystemCursor::ResizeHorizontal;
                break;
            case nisaba::backend_os::WindowEdge::TopLeft:
                target_cursor = nisaba::backend_os::SystemCursor::ResizeTopLeft;
                break;
            case nisaba::backend_os::WindowEdge::TopRight:
                target_cursor = nisaba::backend_os::SystemCursor::ResizeTopRight;
                break;
            case nisaba::backend_os::WindowEdge::BottomLeft:
                target_cursor = nisaba::backend_os::SystemCursor::ResizeBottomLeft;
                break;
            case nisaba::backend_os::WindowEdge::BottomRight:
                target_cursor = nisaba::backend_os::SystemCursor::ResizeBottomRight;
                break;
            default:
                target_cursor = nisaba::backend_os::SystemCursor::Default;
                break;
        }

        if (target_cursor != current_cursor_) {
            current_cursor_ = target_cursor;
            platform_->setCursor(current_cursor_);
        }
    } else if (platform_ && current_cursor_ != nisaba::backend_os::SystemCursor::Default) {
        current_cursor_ = nisaba::backend_os::SystemCursor::Default;
        platform_->setCursor(current_cursor_);
    }

    return hovered_button_ != CsdButtonType::None;
}

bool CsdFrame::handle_mouse_down(float x, float y, int button) {
    if (!window_) return false;

    if (button == 1) {
        if (hovered_edge_ != nisaba::backend_os::WindowEdge::NoneEdge && !is_maximized_) {
            window_->beginResize(hovered_edge_, x, y, button);
            return true;
        }

        if (rect_contains(rect_close_, x, y)) {
            pressed_button_ = CsdButtonType::Close;
            return true;
        }
        if (rect_contains(rect_min_, x, y)) {
            pressed_button_ = CsdButtonType::Minimize;
            return true;
        }
        if (rect_contains(rect_max_, x, y)) {
            pressed_button_ = CsdButtonType::Maximize;
            return true;
        }

        if (rect_contains(rect_titlebar_, x, y)) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_click_time_).count();
            float dist = std::hypot(x - last_click_x_, y - last_click_y_);

            if (elapsed_ms > 0 && elapsed_ms < 400 && dist < 12.0f) {
                window_->toggleMaximize();
                is_maximized_ = window_->isMaximized();
                last_click_time_ = {};
                return true;
            }

            last_click_time_ = now;
            last_click_x_ = x;
            last_click_y_ = y;

            window_->beginMove(x, y, button);
            return true;
        }
    } else if (button == 3) {
        if (rect_contains(rect_titlebar_, x, y)) {
            window_->showWindowMenu(x, y, button);
            return true;
        }
    }

    return false;
}

bool CsdFrame::handle_mouse_up(float x, float y, int button) {
    if (!window_ || button != 1) return false;

    if (pressed_button_ != CsdButtonType::None) {
        CsdButtonType act = pressed_button_;
        pressed_button_ = CsdButtonType::None;

        if (act == CsdButtonType::Close && rect_contains(rect_close_, x, y)) {
            window_->onClose().emit();
            return true;
        }
        if (act == CsdButtonType::Minimize && rect_contains(rect_min_, x, y)) {
            window_->setMinimized(true);
            return true;
        }
        if (act == CsdButtonType::Maximize && rect_contains(rect_max_, x, y)) {
            window_->toggleMaximize();
            is_maximized_ = window_->isMaximized();
            return true;
        }
        return true;
    }

    return false;
}

} // namespace obsidian::ui
