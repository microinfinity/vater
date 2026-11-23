#pragma once
/// @file tab_bar.hpp
/// @brief L5 UI — Obsidian CSD-Integrated Tab Bar (tabs, active pill, OSC 133 exit status dot, close/new buttons).

#include "nisaba/gpu/gpu_canvas.hpp"
#include "nisaba/math/rect.hpp"
#include "theme.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vater::ui {

struct TabItemView {
    std::string title;
    int last_exit_code{-1}; // -1 = none, 0 = ok (green), >0 = error (red)
    bool active{false};
    bool has_bell{false};
    bool recording{false};
};

struct TabBarClick {
    enum class Kind { None, SelectTab, CloseTab, NewTab } kind{Kind::None};
    int tab_index{-1};
};

class TabBar {
public:
    TabBar() = default;

    void render(nisaba::gpu::GpuCanvas& canvas,
                float width, float titlebar_h,
                const std::vector<TabItemView>& tabs,
                const obsidian::ui::AppTheme& theme,
                const obsidian::ui::UnitScale& scale);

    [[nodiscard]] TabBarClick handle_click(float x, float y) const noexcept;
    void handle_hover(float x, float y) noexcept;
    [[nodiscard]] bool is_over_interactive(float x, float y) const noexcept;

private:
    struct TabHitRect {
        nisaba::Rect tab_rect{};
        nisaba::Rect close_rect{};
    };

    std::vector<TabHitRect> tab_rects_;
    nisaba::Rect new_tab_rect_{};
    int hovered_tab_{-1};
    bool hovered_new_{false};
};

} // namespace vater::ui

