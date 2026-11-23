#pragma once
/// @file csd_frame.hpp
/// @brief Exact 1:1 Sovereign Client-Side Decoration (CSD) Window Frame & Window Controls
/// Implements `WindowControls` (22px circular plates at top-left, 6px spacing,
/// permanent glyphs for close/minimize/maximize/restore, and Light/Dark plate shading).

#include "nisaba/backend_os/window.hpp"
#include "nisaba/backend_os/platform.hpp"
#include "nisaba/gpu/gpu_canvas.hpp"
#include "theme.hpp"
#include <chrono>
#include <string>

namespace obsidian::ui {

enum class CsdButtonType {
    None,
    Close,
    Minimize,
    Maximize
};

class CsdFrame {
public:
    CsdFrame();
    ~CsdFrame() = default;

    /// Connect to native Nisaba OS Window and Platform
    void set_window_and_platform(nisaba::backend_os::Window* win, nisaba::backend_os::Platform* plat);

    /// Per-frame state update
    void update(float dt);

    /// Renders the main window rounded surface (windowRadius = 14) and subtle outer rim/shadow
    void render_frame_background(nisaba::gpu::GpuCanvas& canvas, float width, float height,
                                 const AppTheme& theme, const UnitScale& scale,
                                 float window_opacity = 0.85f);

    /// Renders the Obsidian header bar (48px height), top-left window controls (Close, Minimize, Maximize),
    /// and the page title at x = 252, baseline = 28.
    void render_titlebar(nisaba::gpu::GpuCanvas& canvas, float width, float height,
                         const AppTheme& theme, const UnitScale& scale,
                         const std::string& page_title = "", float sidebar_width = 240.0f);

    /// Renders a single Obsidian window control plate ("close", "minimize", "maximize", "restore", "dialog-close")
    static void paint_window_control(nisaba::gpu::GpuCanvas& canvas, const nisaba::Rect& bounds,
                                     const std::string& artwork, bool dark, bool active,
                                     bool enabled, bool hovered, bool pressed, float u);

    /// Handles mouse down. Returns true if handled by CSD (titlebar drag, button click, resize handle)
    bool handle_mouse_down(float x, float y, int button);

    /// Handles mouse move (hover highlights, cursor changes over resize edges)
    bool handle_mouse_move(float x, float y, float width, float height, const UnitScale& scale);

    /// Handles mouse up (button release)
    bool handle_mouse_up(float x, float y, int button);

    [[nodiscard]] bool is_maximized() const noexcept;
    [[nodiscard]] bool is_focused() const noexcept;
    [[nodiscard]] float get_corner_radius(const AppTheme& theme, const UnitScale& scale) const noexcept;
    [[nodiscard]] float get_titlebar_height(const UnitScale& scale) const noexcept;

    /// Hit-test resize handles. Returns WindowEdge or NoneEdge
    [[nodiscard]] nisaba::backend_os::WindowEdge hit_test_edge(float x, float y, float width, float height, const UnitScale& scale) const;

private:
    nisaba::backend_os::Window* window_{nullptr};
    nisaba::backend_os::Platform* platform_{nullptr};

    bool is_maximized_{false};
    bool is_focused_{true};

    float mouse_x_{-1.0f};
    float mouse_y_{-1.0f};

    CsdButtonType hovered_button_{CsdButtonType::None};
    CsdButtonType pressed_button_{CsdButtonType::None};

    nisaba::backend_os::WindowEdge hovered_edge_{nisaba::backend_os::WindowEdge::NoneEdge};
    nisaba::backend_os::SystemCursor current_cursor_{nisaba::backend_os::SystemCursor::Default};

    // Button geometries (top-left: Close, Minimize, Maximize)
    nisaba::Rect rect_close_;
    nisaba::Rect rect_min_;
    nisaba::Rect rect_max_;
    nisaba::Rect rect_titlebar_;

    // Double click detection on titlebar
    std::chrono::steady_clock::time_point last_click_time_{};
    float last_click_x_{-100.0f};
    float last_click_y_{-100.0f};
};

} // namespace obsidian::ui
