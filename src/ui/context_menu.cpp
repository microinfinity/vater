/// @file context_menu.cpp
/// @brief L5 UI — Obsidian Glassmorphic Right-Click Context Menu implementation.

#include "ui/context_menu.hpp"

#include "nisaba/canvas/paint.hpp"
#include "nisaba/effects/drop_shadow.hpp"
#include "nisaba/math/border_radius.hpp"

#include <algorithm>
#include <cmath>

namespace vater::ui {

void ContextMenu::open(float anchor_x, float anchor_y, std::vector<ContextMenuItem> items) {
    anchor_x_ = anchor_x;
    anchor_y_ = anchor_y;
    items_ = std::move(items);
    hovered_idx_ = -1;
    open_ = !items_.empty();
}

void ContextMenu::close() noexcept {
    open_ = false;
    hovered_idx_ = -1;
    items_.clear();
}

float ContextMenu::item_height(const ContextMenuItem& it, float u) const noexcept {
    return it.is_separator ? (9.0f * u) : (28.0f * u);
}

nisaba::Rect ContextMenu::compute_bounds(float win_w, float win_h, float u) const noexcept {
    const float menu_w = std::min(std::max(180.0f * u, 244.0f * u), std::max(120.0f, win_w - 16.0f * u));
    const float pad_y = 6.0f * u;
    float total_h = pad_y * 2.0f;
    for (const auto& it : items_) {
        total_h += item_height(it, u);
    }
    const float menu_h = std::min(total_h, std::max(64.0f, win_h - 16.0f * u));
    const float margin = 8.0f * u;

    float x = anchor_x_;
    float y = anchor_y_;
    if (x + menu_w > win_w - margin) x = win_w - menu_w - margin;
    if (y + menu_h > win_h - margin) y = win_h - menu_h - margin;
    x = std::max(margin, x);
    y = std::max(margin, y);

    return nisaba::Rect::from_xywh(std::floor(x), std::floor(y), menu_w, menu_h).value_or(nisaba::Rect{});
}

int ContextMenu::hit_test_item(float x, float y, float win_w, float win_h, float u) const noexcept {
    if (!open_) return -1;
    const auto bounds = compute_bounds(win_w, win_h, u);
    if (!obsidian::ui::rect_contains(bounds, x, y)) return -1;

    float cur_y = bounds.top() + 6.0f * u;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const float h = item_height(items_[static_cast<size_t>(i)], u);
        if (y >= cur_y && y < cur_y + h) {
            if (items_[static_cast<size_t>(i)].is_separator || !items_[static_cast<size_t>(i)].enabled) {
                return -1;
            }
            return i;
        }
        cur_y += h;
    }
    return -1;
}

bool ContextMenu::handle_mouse_move(float x, float y, float win_w, float win_h, float u) noexcept {
    if (!open_) return false;
    const int hit = hit_test_item(x, y, win_w, win_h, u);
    const bool changed = (hit != hovered_idx_);
    hovered_idx_ = hit;
    return changed || obsidian::ui::rect_contains(compute_bounds(win_w, win_h, u), x, y);
}

bool ContextMenu::handle_mouse_down(float x, float y, int button, float win_w, float win_h, float u,
                                    config::Action& out_action, std::function<void()>& out_custom) {
    out_action = config::Action::None;
    out_custom = {};
    if (!open_) return false;

    const auto bounds = compute_bounds(win_w, win_h, u);
    if (!obsidian::ui::rect_contains(bounds, x, y)) {
        close();
        return true;
    }

    if (button == 1) {
        const int idx = hit_test_item(x, y, win_w, win_h, u);
        if (idx >= 0 && idx < static_cast<int>(items_.size())) {
            out_action = items_[static_cast<size_t>(idx)].action;
            out_custom = items_[static_cast<size_t>(idx)].custom_fn;
            close();
            return true;
        }
    }
    return true;
}

void ContextMenu::move_selection(int dir) noexcept {
    if (items_.empty()) return;
    const int n = static_cast<int>(items_.size());
    int cur = hovered_idx_;
    for (int step = 0; step < n; ++step) {
        if (cur < 0) {
            cur = (dir > 0) ? 0 : (n - 1);
        } else {
            cur = (cur + dir + n) % n;
        }
        const auto& it = items_[static_cast<size_t>(cur)];
        if (!it.is_separator && it.enabled) {
            hovered_idx_ = cur;
            return;
        }
    }
}

bool ContextMenu::handle_key(int keysym, config::Action& out_action, std::function<void()>& out_custom) {
    out_action = config::Action::None;
    out_custom = {};
    if (!open_) return false;

    if (keysym == 0xFF1B) { // Escape
        close();
        return true;
    }
    if (keysym == 0xFF52) { // Up
        move_selection(-1);
        return true;
    }
    if (keysym == 0xFF54) { // Down
        move_selection(+1);
        return true;
    }
    if (keysym == 0xFF0D) { // Enter
        if (hovered_idx_ >= 0 && hovered_idx_ < static_cast<int>(items_.size())) {
            const auto& it = items_[static_cast<size_t>(hovered_idx_)];
            if (!it.is_separator && it.enabled) {
                out_action = it.action;
                out_custom = it.custom_fn;
            }
        }
        close();
        return true;
    }
    return true;
}

void ContextMenu::render(nisaba::gpu::GpuCanvas& canvas, float win_w, float win_h,
                         const obsidian::ui::AppTheme& theme, const obsidian::ui::UnitScale& scale) const {
    if (!open_ || items_.empty()) return;

    const float u = scale.u;
    const auto bounds = compute_bounds(win_w, win_h, u);
    const float radius = 10.0f * u;

    // 1. Hardware GPU Backdrop Blur + Obsidian Drop Shadow + Translucent Glass Fill
    canvas.draw_backdrop_blur(bounds, nisaba::BorderRadius::circular(radius), 12.0f * u);

    const auto shadow = nisaba::effects::DropShadow::medium(6.0f * u, 10.0f * u);

    const nisaba::Color bg = obsidian::ui::detail::with_alpha(theme.secondBackground, 0.92f);
    canvas.draw_round_rect_with_shadow(bounds, radius, radius, nisaba::Paint(bg), shadow);

    nisaba::Stroke border_st;
    border_st.width = 1.0f;
    canvas.stroke_round_rect(bounds, radius, radius, nisaba::Paint(theme.separator), border_st);

    // 2. Render Menu Items & Separators
    float cur_y = bounds.top() + 6.0f * u;
    const float pad_x = 6.0f * u;
    const float inner_w = bounds.width() - pad_x * 2.0f;

    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const auto& it = items_[static_cast<size_t>(i)];
        const float h = item_height(it, u);

        if (it.is_separator) {
            if (auto sep = nisaba::Rect::from_xywh(bounds.left() + 10.0f * u,
                                                   cur_y + h * 0.5f,
                                                   bounds.width() - 20.0f * u,
                                                   1.0f)) {
                canvas.fill_rect(*sep, nisaba::Paint(theme.separator));
            }
            cur_y += h;
            continue;
        }

        const bool is_hovered = (i == hovered_idx_) && it.enabled;
        if (is_hovered) {
            const nisaba::Color pill_col = it.danger
                ? obsidian::ui::detail::with_alpha(obsidian::ui::accent::red, 0.28f)
                : obsidian::ui::detail::with_alpha(theme.highlight, 0.26f);
            if (auto pill = nisaba::Rect::from_xywh(bounds.left() + pad_x, cur_y + 1.0f * u,
                                                    inner_w, h - 2.0f * u)) {
                canvas.fill_round_rect(*pill, 6.0f * u, 6.0f * u, nisaba::Paint(pill_col));
            }
        }

        const nisaba::Color text_col = !it.enabled
            ? theme.disabledText
            : (it.danger && is_hovered ? obsidian::ui::accent::red : theme.text);

        // Checked indicator dot on the left gutter
        if (it.checked) {
            const float dot_r = 3.0f * u;
            canvas.fill_circle(bounds.left() + 15.0f * u, cur_y + h * 0.5f, dot_r,
                               nisaba::Paint(theme.highlight));
        }

        const float baseline_y = cur_y + h * 0.67f;
        canvas.draw_text(bounds.left() + 25.0f * u, baseline_y, it.label,
                         nisaba::Paint(text_col), 12.5f * u, "sans");

        if (!it.shortcut.empty()) {
            const float approx_w = static_cast<float>(it.shortcut.size()) * 6.4f * u;
            const float sx = std::max(bounds.left() + bounds.width() * 0.56f,
                                      bounds.right() - 14.0f * u - approx_w);
            canvas.draw_text(sx, baseline_y, it.shortcut,
                             nisaba::Paint(theme.disabledText), 11.0f * u, "sans");
        }

        cur_y += h;
    }
}

} // namespace vater::ui
