#pragma once
/// @file damage.hpp
/// @brief L4 Renderer — Bridge between Terminal dirty rows / blinking cursor and `nisaba::vspr::TiledSpanTracker`.

#include "nisaba/math/rect.hpp"
#include "nisaba/math/screen_int_rect.hpp"
#include "nisaba/vspr/tiled_span_tracker.hpp"

#include <cstdint>
#include <vector>

namespace vater::render {

/// Bridges grid row/cell damage into `nisaba::vspr::TiledSpanTracker<16>` and produces
/// compact disjoint damage rectangles for `Window::setDamage()` & `GpuCanvas::flush(damage)`.
class DamageBridge {
public:
    void resize(uint32_t width, uint32_t height);

    void mark_full() noexcept;
    void mark_rect(const nisaba::Rect& r) noexcept;
    void mark_cell(const nisaba::Rect& pane_origin, int row, int col, float cell_w, float cell_h) noexcept;
    void mark_row(const nisaba::Rect& pane_origin, int row, float cell_h) noexcept;

    [[nodiscard]] bool has_damage() const noexcept { return full_dirty_ || tracker_.has_damage(); }

    /// Extracts coalesced screen-space damage rects (both `ScreenIntRect` for GpuCanvas
    /// and `nisaba::Rect` for Window::setDamage) and clears tracked state.
    void consume(std::vector<nisaba::ScreenIntRect>& out_screen_rects,
                 std::vector<nisaba::Rect>& out_float_rects);

private:
    nisaba::vspr::TiledSpanTracker<16> tracker_{};
    uint32_t width_{0};
    uint32_t height_{0};
    bool full_dirty_{true};
};

} // namespace vater::render

