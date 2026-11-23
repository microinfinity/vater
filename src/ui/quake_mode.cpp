/// @file quake_mode.cpp
/// @brief L5 UI (M5) — Drop-Down Quake Mode Overlay implementation via `nisaba::backend_os::LayerSurface`.

#include "ui/quake_mode.hpp"

#include <algorithm>
#include <cmath>

namespace vater::ui {

bool QuakeSurface::create(nisaba::backend_os::Platform& platform, float height_fraction) {
    height_fraction = std::clamp(height_fraction, 0.2f, 0.9f);

    int32_t out_w = 1280;
    int32_t out_h = 800;
    if (auto primary = platform.getPrimaryOutput()) {
        const auto geom = primary->logicalGeometry();
        if (geom.width > 0) out_w = static_cast<int32_t>(geom.width);
        if (geom.height > 0) out_h = static_cast<int32_t>(geom.height);
    }

    expanded_height_ = std::max(240, static_cast<int32_t>(std::round(static_cast<float>(out_h) * height_fraction)));

    nisaba::backend_os::LayerSurfaceConfig cfg;
    cfg.namespace_id = "vater-quake";
    cfg.layer = nisaba::backend_os::ShellLayer::Overlay;
    cfg.anchor = nisaba::backend_os::ShellAnchor::TopAll;
    cfg.width = out_w;
    cfg.height = expanded_height_;
    cfg.exclusive_zone = 0;
    cfg.keyboard_mode = nisaba::backend_os::KeyboardMode::Exclusive;
    cfg.transparent = true;
    cfg.vsync = true;

    auto res = nisaba::backend_os::LayerSurface::create(platform, cfg);
    if (!res.isOk()) return false;
    surface_ = std::move(res.value());
    visible_ = true;
    return true;
}

void QuakeSurface::toggle_visible() {
    if (!surface_) return;
    visible_ = !visible_;
    if (visible_) {
        surface_->setSize(0, expanded_height_);
        surface_->setKeyboardMode(nisaba::backend_os::KeyboardMode::Exclusive);
        surface_->resetInputRegion();
    } else {
        surface_->setSize(0, 1);
        surface_->setKeyboardMode(nisaba::backend_os::KeyboardMode::None);
        surface_->setInputRegion({}); // Click-through when retracted
    }
}

} // namespace vater::ui

