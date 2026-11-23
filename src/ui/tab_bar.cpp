/// @file tab_bar.cpp
/// @brief L5 UI — Obsidian CSD-Integrated Tab Bar implementation.

#include "ui/tab_bar.hpp"

#include "nisaba/canvas/paint.hpp"

#include <algorithm>

namespace vater::ui {

void TabBar::render(nisaba::gpu::GpuCanvas& canvas,
                    float width, float titlebar_h,
                    const std::vector<TabItemView>& tabs,
                    const obsidian::ui::AppTheme& theme,
                    const obsidian::ui::UnitScale& scale) {
    const float u = scale.u;
    const float start_x = 14.0f * u;    // Left-aligned tabs (CSD controls are top-right in core/csd_frame.cpp)
    const float right_pad = 104.0f * u; // Clear of Obsidian top-right CSD window controls (Min/Max/Close)
    const float new_btn_w = 26.0f * u;
    const float gap = 6.0f * u;
    const float avail_w = std::max(60.0f * u, width - start_x - right_pad - new_btn_w - gap);
    const float tab_h = 28.0f * u;
    const float tab_y = (titlebar_h - tab_h) * 0.5f;

    const int n = static_cast<int>(tabs.size());
    const float tab_w = (n > 0)
                            ? std::clamp((avail_w - gap * static_cast<float>(std::max(0, n - 1))) / static_cast<float>(n),
                                         92.0f * u, 210.0f * u)
                            : 140.0f * u;

    tab_rects_.resize(static_cast<size_t>(n));
    float cx = start_x;

    for (int i = 0; i < n; ++i) {
        const auto& t = tabs[static_cast<size_t>(i)];
        auto tr = nisaba::Rect::from_xywh(cx, tab_y, tab_w, tab_h).value_or(nisaba::Rect{});
        auto cr = nisaba::Rect::from_xywh(cx + tab_w - 22.0f * u, tab_y + 5.0f * u, 18.0f * u, 18.0f * u).value_or(nisaba::Rect{});
        tab_rects_[static_cast<size_t>(i)] = TabHitRect{tr, cr};

        nisaba::Color fill = t.active
                                 ? obsidian::ui::detail::with_alpha(theme.secondBackground, theme.darkMode ? 0.85f : 0.92f)
                                 : (i == hovered_tab_ ? theme.mutedPlate
                                                      : obsidian::ui::detail::with_alpha(theme.secondBackground, 0.25f));
        canvas.fill_round_rect(tr, 8.0f * u, 8.0f * u, nisaba::Paint(fill));
        if (t.active) {
            nisaba::Stroke st;
            st.width = 1.0f;
            canvas.stroke_round_rect(tr, 8.0f * u, 8.0f * u,
                                     nisaba::Paint(obsidian::ui::detail::with_alpha(theme.highlight, 0.45f)), st);
        }

        // Status dot: recording (red) / OSC 133 exit code (green/red) / bell (orange)
        float text_x = cx + 10.0f * u;
        if (t.recording || t.last_exit_code >= 0 || t.has_bell) {
            nisaba::Color dot_col = obsidian::ui::accent::green;
            if (t.recording || t.last_exit_code > 0) dot_col = obsidian::ui::accent::red;
            else if (t.has_bell) dot_col = obsidian::ui::accent::orange;
            canvas.fill_circle(cx + 12.0f * u, tab_y + tab_h * 0.5f, 3.5f * u, nisaba::Paint(dot_col));
            text_x = cx + 21.0f * u;
        }

        // Truncated title
        std::string label = t.title.empty() ? ("Tab " + std::to_string(i + 1)) : t.title;
        const size_t max_chars = static_cast<size_t>(std::max(4.0f, (tab_w - 42.0f * u) / (7.0f * u)));
        if (label.size() > max_chars) label = label.substr(0, max_chars - 1) + "~";

        canvas.draw_text(text_x, tab_y + 18.5f * u, label,
                         nisaba::Paint(t.active ? theme.text : theme.disabledText),
                         12.0f * u, "sans");

        // Close 'x' glyph
        if (n > 1 || i == hovered_tab_) {
            canvas.draw_text(cr.left() + 4.0f * u, cr.top() + 13.0f * u, "x",
                             nisaba::Paint(theme.disabledText), 11.5f * u, "sans");
        }

        cx += tab_w + gap;
    }

    // '+' New Tab button
    new_tab_rect_ = nisaba::Rect::from_xywh(cx, tab_y + 1.0f * u, new_btn_w, tab_h - 2.0f * u).value_or(nisaba::Rect{});
    canvas.fill_round_rect(new_tab_rect_, 7.0f * u, 7.0f * u,
                           nisaba::Paint(hovered_new_ ? theme.dialogButtonBackground : theme.mutedPlate));
    canvas.draw_text(new_tab_rect_.left() + 8.5f * u, new_tab_rect_.top() + 18.0f * u, "+",
                     nisaba::Paint(theme.text), 14.0f * u, "sans");
}

void TabBar::handle_hover(float x, float y) noexcept {
    hovered_tab_ = -1;
    hovered_new_ = obsidian::ui::rect_contains(new_tab_rect_, x, y);
    for (size_t i = 0; i < tab_rects_.size(); ++i) {
        if (obsidian::ui::rect_contains(tab_rects_[i].tab_rect, x, y)) {
            hovered_tab_ = static_cast<int>(i);
            break;
        }
    }
}

bool TabBar::is_over_interactive(float x, float y) const noexcept {
    if (obsidian::ui::rect_contains(new_tab_rect_, x, y)) return true;
    for (const auto& hr : tab_rects_) {
        if (obsidian::ui::rect_contains(hr.tab_rect, x, y)) return true;
    }
    return false;
}

TabBarClick TabBar::handle_click(float x, float y) const noexcept {
    if (obsidian::ui::rect_contains(new_tab_rect_, x, y)) {
        return TabBarClick{TabBarClick::Kind::NewTab, -1};
    }
    for (size_t i = 0; i < tab_rects_.size(); ++i) {
        if (obsidian::ui::rect_contains(tab_rects_[i].close_rect, x, y)) {
            return TabBarClick{TabBarClick::Kind::CloseTab, static_cast<int>(i)};
        }
        if (obsidian::ui::rect_contains(tab_rects_[i].tab_rect, x, y)) {
            return TabBarClick{TabBarClick::Kind::SelectTab, static_cast<int>(i)};
        }
    }
    return TabBarClick{TabBarClick::Kind::None, -1};
}

} // namespace vater::ui

