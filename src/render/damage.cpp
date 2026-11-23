/// @file damage.cpp
/// @brief L4 Renderer — VSPR TiledSpanTracker damage bridge implementation.

#include "render/damage.hpp"

namespace vater::render {

void DamageBridge::resize(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    tracker_.init(width, height);
    full_dirty_ = true;
}

void DamageBridge::mark_full() noexcept {
    full_dirty_ = true;
}

void DamageBridge::mark_rect(const nisaba::Rect& r) noexcept {
    if (full_dirty_ || width_ == 0 || height_ == 0) return;
    tracker_.mark_dirty(r);
}

void DamageBridge::mark_cell(const nisaba::Rect& pane_origin, int row, int col,
                             float cell_w, float cell_h) noexcept {
    if (full_dirty_ || row < 0 || col < 0) return;
    const float x = pane_origin.left() + static_cast<float>(col) * cell_w;
    const float y = pane_origin.top() + static_cast<float>(row) * cell_h;
    if (auto r = nisaba::Rect::from_xywh(x - 1.0f, y - 1.0f, cell_w + 2.0f, cell_h + 2.0f)) {
        tracker_.mark_dirty(*r);
    }
}

void DamageBridge::mark_row(const nisaba::Rect& pane_origin, int row, float cell_h) noexcept {
    if (full_dirty_ || row < 0) return;
    const float y = pane_origin.top() + static_cast<float>(row) * cell_h;
    if (auto r = nisaba::Rect::from_xywh(pane_origin.left(), y - 1.0f, pane_origin.width(), cell_h + 2.0f)) {
        tracker_.mark_dirty(*r);
    }
}

void DamageBridge::consume(std::vector<nisaba::ScreenIntRect>& out_screen_rects,
                           std::vector<nisaba::Rect>& out_float_rects) {
    out_screen_rects.clear();
    out_float_rects.clear();
    if (width_ == 0 || height_ == 0) return;

    if (full_dirty_) {
        if (auto full_i = nisaba::ScreenIntRect::from_xywh(0, 0, width_, height_)) {
            out_screen_rects.push_back(*full_i);
        }
        if (auto full_f = nisaba::Rect::from_xywh(0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_))) {
            out_float_rects.push_back(*full_f);
        }
        full_dirty_ = false;
        tracker_.clear();
        return;
    }

    tracker_.generate_damage_rects(out_screen_rects, 16);
    out_float_rects.reserve(out_screen_rects.size());
    for (const auto& sr : out_screen_rects) {
        if (auto fr = nisaba::Rect::from_xywh(
                static_cast<float>(sr.x()), static_cast<float>(sr.y()),
                static_cast<float>(sr.width()), static_cast<float>(sr.height()))) {
            out_float_rects.push_back(*fr);
        }
    }
    tracker_.clear();
}

} // namespace vater::render

