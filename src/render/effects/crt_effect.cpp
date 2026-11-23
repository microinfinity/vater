/// @file crt_effect.cpp
/// @brief L4 Renderer — Optional CRT / Retro Scanline RuntimeEffect implementation.

#include "render/effects/crt_effect.hpp"

#include "nisaba/canvas/paint.hpp"

#include <cmath>
#include <cstring>
#include <vector>

namespace vater::render {

void CrtEffect::ensure_compiled() {
    if (compile_attempted_) return;
    compile_attempted_ = true;

    static constexpr const char* kCrtSksl = R"(
        uniform vec2 u_res;
        uniform float u_time;

        vec4 main(vec2 fragCoord) {
            vec2 uv = fragCoord / max(u_res, vec2(1.0, 1.0));
            float scan = 0.5 + 0.5 * sin(fragCoord.y * 3.14159265 + u_time * 1.5);
            float line_dark = (1.0 - scan) * 0.14;
            vec2 d = (uv - vec2(0.5, 0.5)) * 2.0;
            float vig = clamp(dot(d, d) * 0.18, 0.0, 0.35);
            float alpha = clamp(line_dark + vig, 0.0, 0.45);
            return vec4(0.02, 0.05, 0.04, alpha);
        }
    )";

    auto res = nisaba::gpu::RuntimeEffect::MakeForShader(kCrtSksl);
    if (res) {
        effect_ = res.effect;
    }
}

void CrtEffect::render_overlay(nisaba::gpu::GpuCanvas& canvas, const nisaba::Rect& bounds, float time_sec) {
    if (!enabled_) return;
    ensure_compiled();

    if (effect_ && canvas.context()) {
        std::vector<uint8_t> uniforms(effect_->uniformSize(), 0);
        if (const auto* u_res = effect_->findUniform("u_res")) {
            float res[2] = {bounds.width(), bounds.height()};
            if (u_res->offset + sizeof(res) <= uniforms.size()) {
                std::memcpy(uniforms.data() + u_res->offset, res, sizeof(res));
            }
        }
        if (const auto* u_time = effect_->findUniform("u_time")) {
            if (u_time->offset + sizeof(float) <= uniforms.size()) {
                std::memcpy(uniforms.data() + u_time->offset, &time_sec, sizeof(float));
            }
        }
        if (auto shader = effect_->make_shader(uniforms)) {
            auto* ctx = canvas.context();
            ctx->save();
            ctx->beginPath();
            ctx->rect(bounds.left(), bounds.top(), bounds.width(), bounds.height());
            ctx->fillShader(shader);
            ctx->fill();
            ctx->restore();
            return;
        }
    }

    // Fallback scanlines if RuntimeEffect compilation is unavailable on current GL driver
    nisaba::Paint scan_paint(nisaba::Color::from_rgba8(0, 14, 10, 26));
    for (float y = bounds.top(); y < bounds.bottom(); y += 3.0f) {
        if (auto r = nisaba::Rect::from_xywh(bounds.left(), y, bounds.width(), 1.0f)) {
            canvas.fill_rect(*r, scan_paint);
        }
    }
}

} // namespace vater::render

