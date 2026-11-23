#pragma once
/// @file crt_effect.hpp
/// @brief L4 Renderer — Optional CRT / Retro Scanline & Phosphor Glow Shader via `nisaba::gpu::RuntimeEffect`.

#include "nisaba/gpu/gpu_canvas.hpp"
#include "nisaba/gpu/runtime_effect.hpp"
#include "nisaba/math/rect.hpp"

#include <memory>

namespace vater::render {

/// Optional post-overlay CRT scanlines + subtle vignette + phosphor warmth via Nisaba RuntimeEffect.
class CrtEffect {
public:
    CrtEffect() = default;

    void set_enabled(bool e) noexcept { enabled_ = e; }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    void toggle() noexcept { enabled_ = !enabled_; }

    /// Renders the CRT scanline/vignette overlay inside `bounds` (compiles RuntimeEffect lazily).
    void render_overlay(nisaba::gpu::GpuCanvas& canvas, const nisaba::Rect& bounds, float time_sec);

private:
    void ensure_compiled();

    bool enabled_{false};
    bool compile_attempted_{false};
    std::shared_ptr<nisaba::gpu::RuntimeEffect> effect_;
};

} // namespace vater::render

