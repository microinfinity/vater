#pragma once
/// @file quake_mode.hpp
/// @brief L5 UI (M5) — Drop-Down Quake Mode Overlay via `nisaba::backend_os::LayerSurface`.

#include "nisaba/backend_os/layer_surface.hpp"
#include "nisaba/backend_os/platform.hpp"

#include <memory>

namespace vater::ui {

/// Manages a drop-down Quake overlay surface via `Platform` + `LayerSurface`
/// (Wayland `wlr-layer-shell` or X11 fallback, zero direct Wayland/X11 calls!).
class QuakeSurface {
public:
    QuakeSurface() = default;
    ~QuakeSurface() = default;

    /// Creates a top-anchored overlay surface occupying `height_fraction` (default 45%) of screen height.
    bool create(nisaba::backend_os::Platform& platform, float height_fraction = 0.45f);

    [[nodiscard]] bool is_active() const noexcept { return surface_ != nullptr; }
    [[nodiscard]] nisaba::backend_os::LayerSurface* surface() noexcept { return surface_.get(); }

    void toggle_visible();
    [[nodiscard]] bool is_visible() const noexcept { return visible_; }

private:
    std::unique_ptr<nisaba::backend_os::LayerSurface> surface_;
    int32_t expanded_height_{420};
    bool visible_{true};
};

} // namespace vater::ui

