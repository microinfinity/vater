#pragma once
/// @file context_menu.hpp
/// @brief L5 UI — Obsidian Glassmorphic Right-Click Context Menu overlay.

#include "config/schema.hpp"
#include "nisaba/gpu/gpu_canvas.hpp"
#include "nisaba/math/rect.hpp"
#include "theme.hpp"

#include <functional>
#include <string>
#include <vector>

namespace vater::ui {

struct ContextMenuItem {
    std::string label;
    std::string shortcut;
    config::Action action{config::Action::None};
    std::function<void()> custom_fn;
    bool enabled{true};
    bool checked{false};
    bool is_separator{false};
    bool danger{false};

    static ContextMenuItem separator() {
        ContextMenuItem it{};
        it.is_separator = true;
        it.enabled = false;
        return it;
    }
};

/// Floating Obsidian Glass right-click context menu with keyboard & pointer navigation.
class ContextMenu {
public:
    ContextMenu() = default;

    void open(float anchor_x, float anchor_y, std::vector<ContextMenuItem> items);
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept { return open_; }

    /// Computes clamped bounding box for the menu inside (win_w, win_h).
    [[nodiscard]] nisaba::Rect compute_bounds(float win_w, float win_h, float u) const noexcept;

    /// Returns item index at (x, y), or -1 if none / separator / outside.
    [[nodiscard]] int hit_test_item(float x, float y, float win_w, float win_h, float u) const noexcept;

    /// Updates hover state. Returns true if pointer is over the menu or hover changed.
    bool handle_mouse_move(float x, float y, float win_w, float win_h, float u) noexcept;

    /// Handles mouse press while menu is open (always consumes click: invokes item or dismisses).
    bool handle_mouse_down(float x, float y, int button, float win_w, float win_h, float u,
                           config::Action& out_action, std::function<void()>& out_custom);

    /// Handles Up/Down/Enter/Escape while menu is open.
    bool handle_key(int keysym, config::Action& out_action, std::function<void()>& out_custom);

    void render(nisaba::gpu::GpuCanvas& canvas, float win_w, float win_h,
                const obsidian::ui::AppTheme& theme, const obsidian::ui::UnitScale& scale) const;

private:
    [[nodiscard]] float item_height(const ContextMenuItem& it, float u) const noexcept;
    void move_selection(int dir) noexcept;

    bool open_{false};
    float anchor_x_{0.0f};
    float anchor_y_{0.0f};
    std::vector<ContextMenuItem> items_;
    int hovered_idx_{-1};
};

} // namespace vater::ui

