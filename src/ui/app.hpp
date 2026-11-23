#pragma once
/// @file app.hpp
/// @brief L5 Shell UI — Main Vater Terminal Application integrating CSD Frame, Live Obsidian Theme,
/// TabBar, PaneTree Splits (nisaba-layout), GPU 3-pass + CPU Fallback Renderers, VSPR Damage,
/// Input Encoder, Selection, Clipboard/DnD, Search Overlay, Command Palette, Preview Panel,
/// CRT RuntimeEffect, Quake Mode, and `vaterd` Persistent Sessions.

#include "config/schema.hpp"
#include "csd_frame.hpp"
#include "mux/pane_tree.hpp"
#include "nisaba/backend_os/platform.hpp"
#include "nisaba/backend_os/window.hpp"
#include "nisaba/gpu/gpu_canvas.hpp"
#include "render/cell_renderer.hpp"
#include "render/cpu_renderer.hpp"
#include "render/damage.hpp"
#include "render/effects/crt_effect.hpp"
#include "render/font_stack.hpp"
#include "render/palette.hpp"
#include "theme.hpp"
#include "theme_client.hpp"
#include "ui/command_palette.hpp"
#include "ui/context_menu.hpp"
#include "ui/input_encoder.hpp"
#include "ui/preview_panel.hpp"
#include "ui/quake_mode.hpp"
#include "ui/search_overlay.hpp"
#include "ui/tab_bar.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vater::ui {

struct AppLaunchOptions {
    std::string config_path;
    std::string working_dir;
    std::string command;
    std::vector<std::string> command_args;
    bool quake_mode{false};
    bool software_renderer{false};
    bool attach_daemon{false};
    uint32_t attach_session_id{0};
    std::string daemon_socket;
    std::string preview_file;
    std::string playback_file;
    bool force_dark{false};
    bool force_light{false};
    bool static_demo_m0{false}; // Optional M0 static rendering mode
};

/// Main Vater Application Controller (L5).
class VaterApp {
public:
    VaterApp(AppLaunchOptions opts, float width, float height);
    ~VaterApp();

    VaterApp(const VaterApp&) = delete;
    VaterApp& operator=(const VaterApp&) = delete;

    /// Connects Platform, Window (or Quake LayerSurface), and initializes fonts & first session.
    void initialize(nisaba::backend_os::Platform* platform,
                    nisaba::backend_os::Window* window,
                    nisaba::gpu::GpuCanvas* canvas);

    void resize(float width, float height);
    void update(float dt);
    void render(nisaba::gpu::GpuCanvas& canvas,
                std::vector<nisaba::ScreenIntRect>& out_damage_screen,
                std::vector<nisaba::Rect>& out_damage_float);

    // Input handlers (wired to Platform signals)
    void handle_mouse_move(float x, float y);
    void handle_mouse_down(float x, float y, int button);
    void handle_mouse_up(float x, float y, int button);
    void handle_scroll(float x, float y, float dy);
    void handle_key_down(int keysym, int mods);
    void handle_key_up(int keysym, int mods);
    void handle_text_input(std::string_view utf8);
    void handle_focus(bool focused);
    void handle_drop(const nisaba::backend_os::DropEvent& ev);

    void execute_action(config::Action act);

    [[nodiscard]] bool should_quit() const noexcept { return should_quit_; }
    [[nodiscard]] bool needs_redraw() const noexcept { return needs_redraw_.load(std::memory_order_acquire); }
    void request_redraw() noexcept { needs_redraw_.store(true, std::memory_order_release); }

private:
    struct Tab {
        mux::PaneTree tree;
        std::vector<mux::PaneLayoutRect> layout;
        std::vector<mux::DividerRect> dividers;
    };

    void apply_system_theme(const obsidian::ThemeEvent& ev);
    void reload_config();
    void update_palette();
    void recompute_layout(bool debounce_pty_resize);
    void flush_pending_pty_resizes();

    std::unique_ptr<mux::Session> create_session(int cols, int rows, const std::string& cwd);
    void create_new_tab(const std::string& cwd = {});
    void close_active_pane();
    void split_active_pane(mux::SplitAxis axis);
    void paste_from_clipboard(nisaba::backend_os::ClipboardType type, bool confirmed = false);
    void copy_selection_to_clipboard();
    void open_hyperlink(const std::string& uri, bool confirmed = false);
    void open_context_menu(float x, float y);
    bool hit_test_divider(float x, float y, mux::DividerRect& out) const noexcept;
    bool compute_scrollbar_rects(const mux::PaneLayoutRect& pane,
                                 const vt::ViewportSnapshot& snap,
                                 nisaba::Rect& out_track,
                                 nisaba::Rect& out_thumb) const noexcept;
    void scrub_scrollbar(const mux::PaneLayoutRect& pane, float y);
    void update_drag_selection(const mux::PaneLayoutRect& pane, float x, float y);
    const mux::PaneLayoutRect* pane_rect_by_id(uint32_t pane_id) const noexcept;

    Tab* active_tab() noexcept;
    mux::Session* focused_session() noexcept;
    const mux::PaneLayoutRect* focused_pane_rect() const noexcept;
    const mux::PaneLayoutRect* pane_rect_at(float x, float y) const noexcept;
    nisaba::Rect terminal_viewport_rect() const noexcept;

    bool pixel_to_cell(const mux::PaneLayoutRect& pane, float x, float y,
                       int& out_view_row, int& out_logical_col) const noexcept;

    AppLaunchOptions opts_;
    config::VaterConfig cfg_;
    config::ConfigWatcher config_watcher_;

    nisaba::backend_os::Platform* platform_{nullptr};
    nisaba::backend_os::Window* window_{nullptr};
    nisaba::gpu::GpuCanvas* canvas_{nullptr};

    float width_{1040.0f};
    float height_{680.0f};
    obsidian::ui::UnitScale scale_{};

    // Obsidian CSD & Live Theme
    obsidian::ui::CsdFrame csd_frame_;
    obsidian::ThemeClient theme_client_;
    obsidian::ui::AppTheme theme_{obsidian::ui::AppTheme::dark()};
    obsidian::ui::AppTheme target_theme_{obsidian::ui::AppTheme::dark()};
    float window_opacity_{0.88f};
    float target_window_opacity_{0.88f};
    std::mutex theme_ev_mutex_;
    obsidian::ThemeEvent pending_theme_ev_{};
    std::atomic<bool> theme_ev_dirty_{false};

    // Typography & Rendering (L4)
    render::FontStack font_stack_;
    render::TerminalPalette palette_;
    render::GpuCellRenderer gpu_renderer_;
    render::CpuTerminalRenderer cpu_renderer_;
    nisaba::Pixmap cpu_backbuffer_;
    render::DamageBridge damage_;
    render::CrtEffect crt_effect_;

    // Multiplexer (L2): Tabs & Split Pane Trees
    std::vector<std::unique_ptr<Tab>> tabs_;
    int active_tab_idx_{0};
    uint32_t next_pane_id_{1};

    // Cached snapshots per pane ID
    std::unordered_map<uint32_t, vt::ViewportSnapshot> snapshots_;

    // L5 UI Overlays
    TabBar tab_bar_;
    SearchOverlay search_overlay_;
    CommandPalette command_palette_;
    ContextMenu context_menu_;
    PreviewPanel preview_panel_;

    // Mouse, Selection, Double/Triple-click, Hover Link & Paste Confirmation state
    float mouse_x_{0.0f};
    float mouse_y_{0.0f};
    int active_mods_{0};
    bool window_focused_{true};
    bool suppress_next_text_input_{false};
    bool selecting_{false};
    uint32_t selecting_pane_id_{0};
    float selection_autoscroll_accum_{0.0f};
    float single_click_clear_timer_{0.0f};
    render::SelectionRange selection_{};
    std::chrono::steady_clock::time_point last_click_time_{};
    int click_streak_{0};
    float last_click_x_{-100.0f};
    float last_click_y_{-100.0f};
    std::string hovered_url_;
    uint32_t hovered_link_id_{0};

    // Interactive Split Divider Dragging
    uint32_t dragging_split_id_{0};
    uint32_t hovered_split_id_{0};
    mux::SplitAxis dragging_split_axis_{mux::SplitAxis::Horizontal};
    nisaba::Rect dragging_split_container_{};

    // Interactive Pane Scrollbar Dragging
    uint32_t dragging_scrollbar_pane_{0};
    uint32_t hovered_scrollbar_pane_{0};

    // Thread-safe queue for OSC 52 clipboard writes originating on PTY I/O threads
    struct PendingClipboardWrite {
        nisaba::backend_os::ClipboardType type{nisaba::backend_os::ClipboardType::Clipboard};
        std::string text;
    };
    std::mutex osc52_mutex_;
    std::vector<PendingClipboardWrite> pending_osc52_writes_;

    // Multiline paste confirmation banner (§5.6) & unsafe hyperlink scheme guard (§8)
    bool paste_confirm_open_{false};
    std::string pending_paste_text_;
    bool link_confirm_open_{false};
    std::string pending_link_uri_;

    // Cursor blink & resize debounce (16ms per §5.1)
    float elapsed_time_{0.0f};
    float blink_timer_{0.0f};
    bool cursor_blink_on_{true};
    bool pty_resize_pending_{false};
    std::chrono::steady_clock::time_point last_resize_time_{};

    std::atomic<bool> needs_redraw_{true};
    bool should_quit_{false};
};

} // namespace vater::ui

