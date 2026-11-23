/// @file app.cpp
/// @brief L5 Shell UI — Complete Vater Application Controller (M0..M5).

#include "ui/app.hpp"

#include "nisaba/canvas/paint.hpp"
#include "nisaba/image/deflate.hpp"
#include "nisaba/image/image_io.hpp"
#include "os/ipc.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <unistd.h>

namespace vater::ui {

namespace {

bool is_safe_url_scheme(std::string_view uri) noexcept {
    return uri.starts_with("http://") ||
           uri.starts_with("https://") ||
           uri.starts_with("file://");
}

} // namespace

VaterApp::VaterApp(AppLaunchOptions opts, float width, float height)
    : opts_(std::move(opts)),
      width_(width),
      height_(height) {
    scale_ = obsidian::ui::UnitScale::compute(width, height);

    const std::string cfg_path = opts_.config_path.empty()
                                     ? config::default_config_path()
                                     : opts_.config_path;
    auto loaded = config::load_config_file(cfg_path);
    cfg_ = std::move(loaded.config);
    for (const auto& d : loaded.diagnostics) {
        std::fprintf(stderr, "vater config: %s\n", d.c_str());
    }
    if (opts_.software_renderer) cfg_.software_renderer = true;
    if (opts_.force_dark) cfg_.force_dark = true;
    if (opts_.force_light) cfg_.force_light = true;

    config_watcher_.watch(cfg_path);
    crt_effect_.set_enabled(cfg_.crt_effect);

    // Connect live to Obsidian Theme Manager ($XDG_RUNTIME_DIR/obsidian-shell.sock)
    if (cfg_.follow_system_theme && !cfg_.force_dark && !cfg_.force_light) {
        theme_client_.start([this](const obsidian::ThemeEvent& ev) {
            {
                std::lock_guard<std::mutex> lock(theme_ev_mutex_);
                pending_theme_ev_ = ev;
                theme_ev_dirty_.store(true, std::memory_order_release);
            }
            request_redraw();
        });
        apply_system_theme(theme_client_.snapshot());
        theme_ = target_theme_;
        window_opacity_ = target_window_opacity_;
        theme_ev_dirty_.store(false, std::memory_order_release);
    } else {
        target_theme_ = cfg_.force_light ? obsidian::ui::AppTheme::light() : obsidian::ui::AppTheme::dark();
        theme_ = target_theme_;
        window_opacity_ = cfg_.opacity;
        target_window_opacity_ = cfg_.opacity;
    }

    update_palette();
}

VaterApp::~VaterApp() {
    theme_client_.stop();
    config_watcher_.stop();
    for (auto& tab : tabs_) {
        std::vector<mux::Session*> sessions;
        tab->tree.collect_sessions(sessions);
        for (auto* s : sessions) {
            if (s) s->stop(s->is_remote()); // Detach remote vaterd sessions so they survive window close!
        }
    }
    tabs_.clear();
}

void VaterApp::initialize(nisaba::backend_os::Platform* platform,
                          nisaba::backend_os::Window* window,
                          nisaba::gpu::GpuCanvas* canvas) {
    platform_ = platform;
    window_ = window;
    canvas_ = canvas;
    csd_frame_.set_window_and_platform(window, platform);

    if (canvas_ && canvas_->font_system()) {
        font_stack_.init(*canvas_->font_system(), cfg_.font_family, cfg_.fallback_fonts,
                         cfg_.font_size, cfg_.line_height);
    }

    damage_.resize(static_cast<uint32_t>(width_), static_cast<uint32_t>(height_));

    if (!opts_.preview_file.empty()) {
        preview_panel_.open_file(opts_.preview_file);
    }

    create_new_tab(opts_.working_dir);
    request_redraw();
}

void VaterApp::apply_system_theme(const obsidian::ThemeEvent& ev) {
    target_window_opacity_ = std::clamp(ev.apps_opacity, 0.25f, 1.0f);
    if (ev.is_light()) {
        target_theme_ = obsidian::ui::AppTheme::light();
    } else if (ev.is_live()) {
        uint8_t r = 80, g = 227, b = 194;
        obsidian::ThemeEvent::parse_hex_rgb(ev.live_seed, r, g, b);
        target_theme_ = obsidian::ui::AppTheme::live(r, g, b);
    } else {
        target_theme_ = obsidian::ui::AppTheme::dark();
    }
}

void VaterApp::update_palette() {
    vt::DynamicColorState dyn{};
    if (auto* sess = focused_session()) {
        dyn = sess->terminal().dynamic_colors();
    }
    palette_ = render::TerminalPalette::from_theme(theme_, cfg_.palette_override, dyn);
}

void VaterApp::reload_config() {
    const std::string cfg_path = opts_.config_path.empty()
                                     ? config::default_config_path()
                                     : opts_.config_path;
    auto res = config::load_config_file(cfg_path);
    for (const auto& d : res.diagnostics) {
        std::fprintf(stderr, "vater config reload: %s\n", d.c_str());
    }
    if (!res.ok) return;

    cfg_ = std::move(res.config);
    crt_effect_.set_enabled(cfg_.crt_effect);
    if (canvas_ && canvas_->font_system()) {
        font_stack_.init(*canvas_->font_system(), cfg_.font_family, cfg_.fallback_fonts,
                         cfg_.font_size, cfg_.line_height);
    }
    for (auto& tab : tabs_) {
        std::vector<mux::Session*> sessions;
        tab->tree.collect_sessions(sessions);
        for (auto* s : sessions) {
            s->terminal().set_max_scrollback(cfg_.scrollback_lines);
            s->terminal().set_bidi_default(cfg_.bidi_enabled);
            s->terminal().set_clipboard_policy({cfg_.osc52_write, cfg_.osc52_read});
        }
    }
    update_palette();
    recompute_layout(false);
    damage_.mark_full();
    request_redraw();
}

nisaba::Rect VaterApp::terminal_viewport_rect() const noexcept {
    const float tb_h = opts_.quake_mode ? (34.0f * scale_.u) : csd_frame_.get_titlebar_height(scale_);
    const float pad_x = cfg_.padding_x * scale_.u;
    const float pad_y = cfg_.padding_y * scale_.u;
    const float total_w = std::max(80.0f, width_ - pad_x * 2.0f);
    const float preview_w = preview_panel_.is_open() ? std::max(260.0f, total_w * 0.40f) : 0.0f;
    const float term_w = std::max(80.0f, total_w - preview_w);
    const float term_h = std::max(60.0f, height_ - tb_h - pad_y * 2.0f);
    return nisaba::Rect::from_xywh(pad_x, tb_h + pad_y, term_w, term_h).value_or(nisaba::Rect{});
}

std::unique_ptr<mux::Session> VaterApp::create_session(int cols, int rows, const std::string& cwd) {
    const uint32_t id = next_pane_id_++;
    auto sess = std::make_unique<mux::Session>(id, cols, rows, cfg_.scrollback_lines);

    auto& term = sess->terminal();
    term.set_bidi_default(cfg_.bidi_enabled);
    term.set_clipboard_policy({cfg_.osc52_write, cfg_.osc52_read});
    const auto& m = font_stack_.metrics();
    term.set_cell_pixel_size(static_cast<int>(m.cell_w), static_cast<int>(m.cell_h));

    // Inject Nisaba PNG/Image & Zlib decoders into L3 GraphicsStore (keeps L3 strictly Nisaba-agnostic!)
    term.set_png_decoder([](const uint8_t* data, size_t len, uint32_t& out_w, uint32_t& out_h,
                            std::vector<uint8_t>& out_rgba) -> bool {
        auto img = nisaba::image::load_image_from_memory(std::span<const uint8_t>(data, len));
        if (!img.value.has_value() || img.value->width() == 0 || img.value->height() == 0) return false;
        const auto ref = img.value->as_ref();
        out_w = ref.width();
        out_h = ref.height();
        const size_t bytes = static_cast<size_t>(out_w) * out_h * 4u;
        out_rgba.resize(bytes);
        for (uint32_t y = 0; y < out_h; ++y) {
            std::memcpy(out_rgba.data() + static_cast<size_t>(y) * out_w * 4u,
                        ref.row_raw(y), static_cast<size_t>(out_w) * 4u);
        }
        return true;
    });

    term.set_zlib_decompressor([](const uint8_t* data, size_t len, size_t expected_size,
                                  std::vector<uint8_t>& out) -> bool {
        auto res = nisaba::image::zlib_decompress(std::span<const uint8_t>(data, len), expected_size, false);
        if (!res.value.has_value()) return false;
        out = std::move(*res.value);
        return true;
    });

    // Wire OSC 52 clipboard callbacks (marshaling writes to UI thread for Wayland/X11 safety)
    sess->set_clipboard_handlers(
        [this](std::string_view sel, std::string_view text) {
            const auto ctype = (sel.find('p') != std::string_view::npos)
                                   ? nisaba::backend_os::ClipboardType::Primary
                                   : nisaba::backend_os::ClipboardType::Clipboard;
            {
                std::lock_guard<std::mutex> lock(osc52_mutex_);
                pending_osc52_writes_.push_back({ctype, std::string(text)});
            }
            request_redraw();
        },
        [this](std::string_view sel) -> std::string {
            if (!platform_) return {};
            const auto ctype = (sel.find('p') != std::string_view::npos)
                                   ? nisaba::backend_os::ClipboardType::Primary
                                   : nisaba::backend_os::ClipboardType::Clipboard;
            return platform_->getClipboardText(ctype);
        });

    auto wake_fn = [this]() { request_redraw(); };

    // Asciicast v2 playback mode (--play FILE.cast) for the initial session
    if (!opts_.playback_file.empty()) {
        std::string cast_path = std::move(opts_.playback_file);
        opts_.playback_file.clear();
        if (sess->start_playback(cast_path, wake_fn)) {
            return sess;
        }
        std::fprintf(stderr, "vater: failed to open asciicast file: %s\n", cast_path.c_str());
    }

    if (opts_.attach_daemon) {
        const std::string sock = opts_.daemon_socket.empty()
                                     ? os::default_vaterd_socket_path(false)
                                     : opts_.daemon_socket;
        if (sess->start_remote(sock, opts_.attach_session_id, cwd, cfg_.term, wake_fn)) {
            opts_.attach_session_id = 0; // Subsequent new panes create new sessions
            return sess;
        }
        std::fprintf(stderr, "vater: vaterd attach failed (%s); falling back to local PTY\n", sock.c_str());
    }

    os::SpawnConfig scfg;
    scfg.program = opts_.command.empty() ? cfg_.shell : opts_.command;
    scfg.args = opts_.command_args;
    scfg.working_dir = cwd;
    scfg.term = cfg_.term;
    scfg.login_shell = opts_.command.empty();

    // Only use one-shot CLI `-e` command for the first pane
    opts_.command.clear();
    opts_.command_args.clear();

    sess->start_local(scfg, wake_fn);
    return sess;
}

void VaterApp::create_new_tab(const std::string& cwd) {
    std::string dir = cwd;
    if (dir.empty()) {
        if (auto* cur = focused_session()) {
            dir = cur->terminal().cwd();
        }
    }
    const auto vp = terminal_viewport_rect();
    const auto& m = font_stack_.metrics();
    const int cols = std::max(10, static_cast<int>(vp.width() / std::max(1.0f, m.cell_w)));
    const int rows = std::max(4, static_cast<int>(vp.height() / std::max(1.0f, m.cell_h)));

    auto tab = std::make_unique<Tab>();
    tab->tree.init_root(create_session(cols, rows, dir));
    tabs_.push_back(std::move(tab));
    active_tab_idx_ = static_cast<int>(tabs_.size()) - 1;
    selection_.active = false;
    recompute_layout(false);
    damage_.mark_full();
    request_redraw();
}

void VaterApp::split_active_pane(mux::SplitAxis axis) {
    auto* tab = active_tab();
    if (!tab) return;
    const uint32_t target = tab->tree.focused_pane_id();
    std::string cwd;
    if (auto* cur = tab->tree.focused_session()) cwd = cur->terminal().cwd();

    auto sess = create_session(40, 12, cwd);
    tab->tree.split_pane(target, axis, std::move(sess), 0.5f);
    selection_.active = false;
    recompute_layout(false);
    damage_.mark_full();
    request_redraw();
}

void VaterApp::close_active_pane() {
    auto* tab = active_tab();
    if (!tab) return;
    const uint32_t pid = tab->tree.focused_pane_id();
    if (auto* s = tab->tree.find_session(pid)) {
        s->stop(s->is_remote());
    }
    snapshots_.erase(pid);
    if (!tab->tree.close_pane(pid)) {
        tabs_.erase(tabs_.begin() + active_tab_idx_);
        if (tabs_.empty()) {
            should_quit_ = true;
            return;
        }
        active_tab_idx_ = std::clamp(active_tab_idx_, 0, static_cast<int>(tabs_.size()) - 1);
    }
    selection_.active = false;
    recompute_layout(false);
    damage_.mark_full();
    request_redraw();
}

VaterApp::Tab* VaterApp::active_tab() noexcept {
    if (tabs_.empty() || active_tab_idx_ < 0 || active_tab_idx_ >= static_cast<int>(tabs_.size())) {
        return nullptr;
    }
    return tabs_[static_cast<size_t>(active_tab_idx_)].get();
}

mux::Session* VaterApp::focused_session() noexcept {
    if (auto* t = active_tab()) return t->tree.focused_session();
    return nullptr;
}

const mux::PaneLayoutRect* VaterApp::focused_pane_rect() const noexcept {
    if (tabs_.empty() || active_tab_idx_ < 0 || active_tab_idx_ >= static_cast<int>(tabs_.size())) {
        return nullptr;
    }
    const auto& tab = *tabs_[static_cast<size_t>(active_tab_idx_)];
    for (const auto& p : tab.layout) {
        if (p.focused) return &p;
    }
    return tab.layout.empty() ? nullptr : &tab.layout[0];
}

const mux::PaneLayoutRect* VaterApp::pane_rect_at(float x, float y) const noexcept {
    if (tabs_.empty() || active_tab_idx_ < 0 || active_tab_idx_ >= static_cast<int>(tabs_.size())) {
        return nullptr;
    }
    const auto& tab = *tabs_[static_cast<size_t>(active_tab_idx_)];
    for (const auto& p : tab.layout) {
        if (obsidian::ui::rect_contains(p.bounds, x, y)) return &p;
    }
    return nullptr;
}

const mux::PaneLayoutRect* VaterApp::pane_rect_by_id(uint32_t pane_id) const noexcept {
    if (pane_id == 0 || tabs_.empty() || active_tab_idx_ < 0 || active_tab_idx_ >= static_cast<int>(tabs_.size())) {
        return nullptr;
    }
    const auto& tab = *tabs_[static_cast<size_t>(active_tab_idx_)];
    for (const auto& p : tab.layout) {
        if (p.pane_id == pane_id) return &p;
    }
    return nullptr;
}

void VaterApp::update_drag_selection(const mux::PaneLayoutRect& pane, float x, float y) {
    if (!pane.session) return;
    const float cx = std::clamp(x, pane.bounds.left(), std::max(pane.bounds.left(), pane.bounds.right() - 1.0f));
    const float cy = std::clamp(y, pane.bounds.top(), std::max(pane.bounds.top(), pane.bounds.bottom() - 1.0f));
    int vrow = 0, lcol = 0;
    if (!pixel_to_cell(pane, cx, cy, vrow, lcol)) return;

    const auto& term = pane.session->terminal();
    const bool alt = term.modes().alt_screen;
    const int sb = alt ? 0 : static_cast<int>(term.scrollback_size());
    const int off = alt ? 0 : term.scroll_offset();
    selection_.end_abs_row = sb - off + vrow;
    selection_.end_col = lcol;
    selection_.rectangular = (active_mods_ & 4) != 0; // Alt = rectangular selection (§5.6)
}

bool VaterApp::pixel_to_cell(const mux::PaneLayoutRect& pane, float x, float y,
                             int& out_view_row, int& out_logical_col) const noexcept {
    const auto& m = font_stack_.metrics();
    if (m.cell_w <= 0.0f || m.cell_h <= 0.0f) return false;
    const int vcol = std::max(0, static_cast<int>((x - pane.bounds.left()) / m.cell_w));
    const int vrow = std::max(0, static_cast<int>((y - pane.bounds.top()) / m.cell_h));
    out_view_row = vrow;
    out_logical_col = gpu_renderer_.visual_to_logical_col(vrow, vcol);
    if (pane.session) {
        out_view_row = std::clamp(out_view_row, 0, std::max(0, pane.session->terminal().rows() - 1));
        out_logical_col = std::clamp(out_logical_col, 0, std::max(0, pane.session->terminal().cols() - 1));
    }
    return true;
}

void VaterApp::recompute_layout(bool debounce_pty_resize) {
    const auto vp = terminal_viewport_rect();
    for (auto& tab : tabs_) {
        tab->tree.compute_layout(vp, tab->layout, tab->dividers, 2.0f * scale_.u);
    }
    if (debounce_pty_resize) {
        pty_resize_pending_ = true;
        last_resize_time_ = std::chrono::steady_clock::now();
    } else {
        flush_pending_pty_resizes();
    }
}

void VaterApp::flush_pending_pty_resizes() {
    pty_resize_pending_ = false;
    const auto& m = font_stack_.metrics();
    if (m.cell_w < 1.0f || m.cell_h < 1.0f) return;

    for (auto& tab : tabs_) {
        for (const auto& p : tab->layout) {
            if (!p.session) continue;
            const int cols = std::max(4, static_cast<int>(p.bounds.width() / m.cell_w));
            const int rows = std::max(2, static_cast<int>(p.bounds.height() / m.cell_h));
            if (cols != p.session->terminal().cols() || rows != p.session->terminal().rows()) {
                p.session->resize(cols, rows,
                                  static_cast<int>(p.bounds.width()),
                                  static_cast<int>(p.bounds.height()));
            }
            p.session->terminal().set_cell_pixel_size(static_cast<int>(m.cell_w), static_cast<int>(m.cell_h));
        }
    }
}

void VaterApp::resize(float width, float height) {
    width_ = width;
    height_ = height;
    scale_ = obsidian::ui::UnitScale::compute(width, height);
    damage_.resize(static_cast<uint32_t>(width_), static_cast<uint32_t>(height_));
    recompute_layout(true); // 16ms debounce during window edge drag (§5.1)
    request_redraw();
}

void VaterApp::update(float dt) {
    elapsed_time_ += dt;
    csd_frame_.update(dt);

    // Hot-reload config via inotify
    if (config_watcher_.poll_changed()) {
        reload_config();
    }

    // Drain queued OSC 52 clipboard writes on the UI thread
    if (platform_) {
        std::vector<PendingClipboardWrite> writes;
        {
            std::lock_guard<std::mutex> lock(osc52_mutex_);
            writes.swap(pending_osc52_writes_);
        }
        for (const auto& w : writes) {
            platform_->setClipboardText(w.text, w.type);
        }
    }

    // Apply live Obsidian ThemeClient events smoothly
    if (theme_ev_dirty_.exchange(false, std::memory_order_acq_rel)) {
        obsidian::ThemeEvent ev;
        {
            std::lock_guard<std::mutex> lock(theme_ev_mutex_);
            ev = pending_theme_ev_;
        }
        apply_system_theme(ev);
        damage_.mark_full();
        request_redraw();
    }

    const float lerp_t = std::clamp(dt * 12.0f, 0.0f, 1.0f);
    if (std::abs(window_opacity_ - target_window_opacity_) > 0.002f) {
        window_opacity_ += (target_window_opacity_ - window_opacity_) * lerp_t;
        theme_ = obsidian::ui::AppTheme::lerp(theme_, target_theme_, lerp_t);
        update_palette();
        damage_.mark_full();
        request_redraw();
    } else if (theme_.background.to_color_u8() != target_theme_.background.to_color_u8() ||
               theme_.highlight.to_color_u8() != target_theme_.highlight.to_color_u8()) {
        theme_ = obsidian::ui::AppTheme::lerp(theme_, target_theme_, lerp_t);
        update_palette();
        damage_.mark_full();
        request_redraw();
    }

    // 16ms debounced PTY TIOCSWINSZ after window resize (§5.1)
    if (pty_resize_pending_) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - last_resize_time_).count();
        if (ms >= 16) {
            flush_pending_pty_resizes();
            damage_.mark_full();
            request_redraw();
        }
    }

    // Auto-close exited panes
    for (int ti = static_cast<int>(tabs_.size()) - 1; ti >= 0; --ti) {
        std::vector<mux::Session*> sessions;
        tabs_[static_cast<size_t>(ti)]->tree.collect_sessions(sessions);
        for (auto* s : sessions) {
            if (s && !s->is_alive()) {
                const uint32_t dead_id = s->id();
                snapshots_.erase(dead_id);
                if (!tabs_[static_cast<size_t>(ti)]->tree.close_pane(dead_id)) {
                    tabs_.erase(tabs_.begin() + ti);
                    if (tabs_.empty()) {
                        should_quit_ = true;
                        return;
                    }
                    active_tab_idx_ = std::clamp(active_tab_idx_, 0, static_cast<int>(tabs_.size()) - 1);
                    break;
                }
                recompute_layout(false);
                damage_.mark_full();
                request_redraw();
            }
        }
    }

    // Update window title from focused session
    if (window_) {
        if (auto* sess = focused_session()) {
            const std::string t = sess->terminal().title();
            static std::string last_title;
            if (t != last_title && !t.empty()) {
                last_title = t;
                window_->setTitle(t);
            }
        }
    }

    // Cursor blink timer (single-cell damage per §5.5 & §7!)
    if (auto* sess = focused_session()) {
        const auto m = sess->terminal().modes();
        if (m.cursor_blink && m.cursor_visible && window_focused_) {
            blink_timer_ += dt * 1000.0f;
            if (blink_timer_ >= static_cast<float>(cfg_.cursor_blink_ms)) {
                blink_timer_ = 0.0f;
                cursor_blink_on_ = !cursor_blink_on_;
                if (const auto* pr = focused_pane_rect()) {
                    const auto it = snapshots_.find(pr->pane_id);
                    if (it != snapshots_.end()) {
                        damage_.mark_cell(pr->bounds, it->second.cursor.row, it->second.cursor.col,
                                          font_stack_.metrics().cell_w, font_stack_.metrics().cell_h);
                    }
                }
                request_redraw();
            }
        } else if (!cursor_blink_on_) {
            cursor_blink_on_ = true;
            request_redraw();
        }
    }

    // Smooth auto-scroll while dragging text selection near/past top (CSD) or bottom window edge
    if (selecting_) {
        const auto* pane = pane_rect_by_id(selecting_pane_id_);
        if (!pane) pane = focused_pane_rect();
        if (pane && pane->session && !pane->session->terminal().modes().alt_screen) {
            const float edge_zone = std::min(16.0f * scale_.u,
                                             std::max(6.0f, font_stack_.metrics().cell_h * 0.85f));
            int dir = 0;
            float penetration = 0.0f;
            if (mouse_y_ <= pane->bounds.top() + edge_zone) {
                dir = +1; // Scroll up into scrollback
                penetration = (pane->bounds.top() + edge_zone) - mouse_y_;
            } else if (mouse_y_ >= pane->bounds.bottom() - edge_zone) {
                dir = -1; // Scroll down toward live bottom
                penetration = mouse_y_ - (pane->bounds.bottom() - edge_zone);
            }

            if (dir != 0) {
                auto& term = pane->session->terminal();
                const int sb = static_cast<int>(term.scrollback_size());
                const int off = term.scroll_offset();
                const bool can_scroll = (dir > 0 && off < sb) || (dir < 0 && off > 0);
                if (can_scroll) {
                    const float lines_per_sec = std::clamp(10.0f + penetration * 0.55f, 10.0f, 95.0f);
                    selection_autoscroll_accum_ += lines_per_sec * dt;
                    if (selection_autoscroll_accum_ >= 1.0f) {
                        const int step = static_cast<int>(selection_autoscroll_accum_);
                        selection_autoscroll_accum_ -= static_cast<float>(step);
                        term.scroll_view(dir * step);
                        update_drag_selection(*pane, mouse_x_, mouse_y_);
                        damage_.mark_full();
                        request_redraw();
                    }
                } else {
                    selection_autoscroll_accum_ = 0.0f;
                }
            } else {
                selection_autoscroll_accum_ = 0.0f;
            }
        }
    }

    // Auto-clear transient single-click cell highlight after 0.5s (normal click only)
    if (single_click_clear_timer_ > 0.0f) {
        single_click_clear_timer_ -= dt;
        if (single_click_clear_timer_ <= 0.0f) {
            single_click_clear_timer_ = 0.0f;
            if (!selecting_ && selection_.active &&
                selection_.start_abs_row == selection_.end_abs_row &&
                selection_.start_col == selection_.end_col) {
                selection_.active = false;
                damage_.mark_full();
                request_redraw();
            }
        }
    }

    // Check if any visible session has dirty terminal rows
    if (auto* tab = active_tab()) {
        for (const auto& p : tab->layout) {
            if (p.session && p.session->terminal().has_dirty()) {
                request_redraw();
                break;
            }
        }
    }
}

void VaterApp::render(nisaba::gpu::GpuCanvas& canvas,
                      std::vector<nisaba::ScreenIntRect>& out_damage_screen,
                      std::vector<nisaba::Rect>& out_damage_float) {
    needs_redraw_.store(false, std::memory_order_release);

    const float tb_h = opts_.quake_mode ? (34.0f * scale_.u) : csd_frame_.get_titlebar_height(scale_);

    // 1. Obsidian Glass CSD Window Frame Background
    csd_frame_.render_frame_background(canvas, width_, height_, theme_, scale_, window_opacity_);

    // 2. Titlebar + Tab Bar
    if (!opts_.quake_mode) {
        csd_frame_.render_titlebar(canvas, width_, height_, theme_, scale_, "", 0.0f);
    }

    std::vector<TabItemView> tab_views;
    tab_views.reserve(tabs_.size());
    for (size_t i = 0; i < tabs_.size(); ++i) {
        TabItemView tv{};
        tv.active = (static_cast<int>(i) == active_tab_idx_);
        if (auto* s = tabs_[i]->tree.focused_session()) {
            tv.title = s->terminal().title();
            tv.last_exit_code = s->terminal().last_exit_code();
            tv.recording = s->is_recording();
            tv.has_bell = s->consume_bell();
        }
        tab_views.push_back(std::move(tv));
    }
    tab_bar_.render(canvas, width_, tb_h, tab_views, theme_, scale_);

    // 3. Render Active Tab's Split Panes (GPU 3-pass or CPU fallback)
    if (auto* tab = active_tab()) {
        for (const auto& p : tab->layout) {
            if (!p.session) continue;
            auto& snap = snapshots_[p.pane_id];
            p.session->terminal().snapshot(snap, true);

            // Feed per-row damage into VSPR TiledSpanTracker
            for (int r = 0; r < static_cast<int>(snap.dirty_rows.size()); ++r) {
                if (snap.dirty_rows[static_cast<size_t>(r)]) {
                    damage_.mark_row(p.bounds, r, font_stack_.metrics().cell_h);
                }
            }

            render::RenderOverlayState ov{};
            ov.pane_focused = p.focused && window_focused_;
            ov.cursor_blink_on = cursor_blink_on_;
            ov.enable_ligatures = cfg_.ligatures;
            ov.background_alpha = 0.0f; // Glass frame background shines through
            if (p.focused) {
                ov.selection = selection_;
                ov.hovered_hyperlink_id = hovered_link_id_;
                if (search_overlay_.is_open()) {
                    ov.search_matches = search_overlay_.matches();
                    ov.active_search_match = search_overlay_.active_index();
                }
            }

            const auto pal = render::TerminalPalette::from_theme(
                theme_, cfg_.palette_override, p.session->terminal().dynamic_colors());

            if (cfg_.software_renderer) {
                const uint32_t pw = std::max(1u, static_cast<uint32_t>(std::ceil(p.bounds.width())));
                const uint32_t ph = std::max(1u, static_cast<uint32_t>(std::ceil(p.bounds.height())));
                if (cpu_backbuffer_.width() != pw || cpu_backbuffer_.height() != ph) {
                    if (auto pm = nisaba::Pixmap::create(pw, ph)) cpu_backbuffer_ = std::move(*pm);
                }
                if (cpu_backbuffer_.width() == pw && cpu_backbuffer_.height() == ph) {
                    nisaba::Canvas sw_canvas(cpu_backbuffer_);
                    ov.background_alpha = window_opacity_;
                    auto local_r = nisaba::Rect::from_xywh(0.0f, 0.0f, static_cast<float>(pw), static_cast<float>(ph)).value();
                    cpu_renderer_.render_pane(sw_canvas, local_r, snap, p.session->terminal(),
                                              font_stack_, pal, ov);
                    canvas.draw_pixmap_rect(cpu_backbuffer_.as_ref(), local_r, p.bounds);
                }
            } else {
                gpu_renderer_.render_pane(canvas, p.bounds, snap, p.session->terminal(),
                                          font_stack_, pal, ov);
            }

            // Subtle inactive-pane dim & active border indicator when split
            if (tab->layout.size() > 1 && !p.focused) {
                canvas.fill_rect(p.bounds, nisaba::Paint(nisaba::Color::from_rgba8(0, 0, 0, 28)));
            }

            // Obsidian Scrollbar Thumb when scrollback exists
            nisaba::Rect sb_track{}, sb_thumb{};
            if (compute_scrollbar_rects(p, snap, sb_track, sb_thumb)) {
                const bool sb_active = (dragging_scrollbar_pane_ == p.pane_id) ||
                                       (hovered_scrollbar_pane_ == p.pane_id) ||
                                       (snap.scroll_offset > 0);
                const float u = scale_.u;
                const float bar_w = sb_active ? (6.0f * u) : (3.5f * u);
                const float bar_x = sb_thumb.right() - bar_w;
                if (sb_active) {
                    if (auto tr = nisaba::Rect::from_xywh(bar_x, sb_track.top(), bar_w, sb_track.height())) {
                        canvas.fill_round_rect(*tr, bar_w * 0.5f, bar_w * 0.5f,
                                               nisaba::Paint(obsidian::ui::detail::with_alpha(theme_.separator, 0.35f)));
                    }
                }
                const nisaba::Color thumb_col = (dragging_scrollbar_pane_ == p.pane_id || hovered_scrollbar_pane_ == p.pane_id)
                                                    ? obsidian::ui::detail::with_alpha(theme_.highlight, 0.85f)
                                                    : obsidian::ui::detail::with_alpha(theme_.disabledText,
                                                                                       sb_active ? 0.65f : 0.30f);
                if (auto th = nisaba::Rect::from_xywh(bar_x, sb_thumb.top(), bar_w, sb_thumb.height())) {
                    canvas.fill_round_rect(*th, bar_w * 0.5f, bar_w * 0.5f, nisaba::Paint(thumb_col));
                }
            }
        }

        // Render split dividers (highlight on hover/drag)
        for (const auto& div : tab->dividers) {
            const bool active_div = (div.split_id == dragging_split_id_) ||
                                    (div.split_id == hovered_split_id_);
            const nisaba::Color dcol = active_div
                                           ? obsidian::ui::detail::with_alpha(theme_.highlight, 0.90f)
                                           : theme_.separator;
            canvas.fill_rect(div.bounds, nisaba::Paint(dcol));
        }
    }

    // 4. Optional CRT / Retro Shader Overlay (RuntimeEffect)
    if (crt_effect_.enabled()) {
        crt_effect_.render_overlay(canvas, terminal_viewport_rect(), elapsed_time_);
    }

    // 5. Optional Side-Panel Preview (Markdown / PDF / Image)
    if (preview_panel_.is_open()) {
        const auto tvp = terminal_viewport_rect();
        const float px = tvp.right() + 6.0f * scale_.u;
        const float pw = std::max(100.0f, width_ - px - cfg_.padding_x * scale_.u);
        if (auto prect = nisaba::Rect::from_xywh(px, tvp.top(), pw, tvp.height())) {
            preview_panel_.render(canvas, *prect, theme_, scale_);
        }
    }

    // 6. Hyperlink Hover URL Tooltip (§8 security: always show real target URL on hover)
    if (!hovered_url_.empty()) {
        const float u = scale_.u;
        const float tw = std::min(width_ - 24.0f * u, static_cast<float>(hovered_url_.size()) * 7.2f * u + 20.0f * u);
        const float th = 24.0f * u;
        if (auto tr = nisaba::Rect::from_xywh(12.0f * u, height_ - th - 10.0f * u, tw, th)) {
            canvas.fill_round_rect(*tr, 6.0f * u, 6.0f * u,
                                   nisaba::Paint(obsidian::ui::detail::with_alpha(theme_.secondBackground, 0.94f)));
            canvas.draw_text(tr->left() + 10.0f * u, tr->top() + 16.0f * u, hovered_url_,
                             nisaba::Paint(theme_.highlight), 11.5f * u, "sans");
        }
    }

    // 7. Multiline Paste Confirmation Guard (§5.6)
    if (paste_confirm_open_) {
        const float u = scale_.u;
        const float bw = std::min(width_ - 32.0f * u, 440.0f * u);
        const float bh = 68.0f * u;
        const float bx = (width_ - bw) * 0.5f;
        const float by = tb_h + 18.0f * u;
        if (auto br = nisaba::Rect::from_xywh(bx, by, bw, bh)) {
            canvas.fill_round_rect(*br, 10.0f * u, 10.0f * u,
                                   nisaba::Paint(obsidian::ui::detail::with_alpha(theme_.secondBackground, 0.97f)));
            nisaba::Stroke st; st.width = 1.2f;
            canvas.stroke_round_rect(*br, 10.0f * u, 10.0f * u, nisaba::Paint(obsidian::ui::accent::orange), st);
            canvas.draw_text(bx + 14.0f * u, by + 26.0f * u,
                             "Multiline paste detected (" + std::to_string(pending_paste_text_.size()) + " bytes)",
                             nisaba::Paint(theme_.text), 13.0f * u, "sans");
            canvas.draw_text(bx + 14.0f * u, by + 48.0f * u,
                             "Press Enter to paste, or Escape to cancel",
                             nisaba::Paint(theme_.disabledText), 12.0f * u, "sans");
        }
    }

    // 7b. Unsafe Hyperlink Scheme Confirmation Guard (§8)
    if (link_confirm_open_) {
        const float u = scale_.u;
        const float bw = std::min(width_ - 32.0f * u, 500.0f * u);
        const float bh = 72.0f * u;
        const float bx = (width_ - bw) * 0.5f;
        const float by = tb_h + 18.0f * u;
        if (auto br = nisaba::Rect::from_xywh(bx, by, bw, bh)) {
            canvas.fill_round_rect(*br, 10.0f * u, 10.0f * u,
                                   nisaba::Paint(obsidian::ui::detail::with_alpha(theme_.secondBackground, 0.97f)));
            nisaba::Stroke st; st.width = 1.2f;
            canvas.stroke_round_rect(*br, 10.0f * u, 10.0f * u, nisaba::Paint(obsidian::ui::accent::orange), st);
            std::string preview_uri = pending_link_uri_;
            if (preview_uri.size() > 54) preview_uri = preview_uri.substr(0, 51) + "...";
            canvas.draw_text(bx + 14.0f * u, by + 26.0f * u,
                             "External URI scheme: " + preview_uri,
                             nisaba::Paint(theme_.text), 13.0f * u, "sans");
            canvas.draw_text(bx + 14.0f * u, by + 50.0f * u,
                             "Press Enter to open via xdg-open, or Escape to cancel",
                             nisaba::Paint(theme_.disabledText), 12.0f * u, "sans");
        }
    }

    // 8. Search Overlay, Command Palette & Right-Click Context Menu (topmost)
    search_overlay_.render(canvas, width_, tb_h, theme_, scale_);
    command_palette_.render(canvas, width_, height_, theme_, scale_);
    context_menu_.render(canvas, width_, height_, theme_, scale_);

    // 9. Consume VSPR damage rectangles
    damage_.consume(out_damage_screen, out_damage_float);
}

void VaterApp::handle_mouse_move(float x, float y) {
    mouse_x_ = x;
    mouse_y_ = y;

    if (context_menu_.is_open()) {
        if (context_menu_.handle_mouse_move(x, y, width_, height_, scale_.u)) {
            if (platform_) platform_->setCursor(nisaba::backend_os::SystemCursor::Default);
            damage_.mark_full();
            request_redraw();
        }
        return;
    }

    // Active split divider drag
    if (dragging_split_id_ != 0) {
        if (auto* tab = active_tab()) {
            float ratio = 0.5f;
            if (dragging_split_axis_ == mux::SplitAxis::Horizontal) {
                ratio = (x - dragging_split_container_.left()) / std::max(1.0f, dragging_split_container_.width());
            } else {
                ratio = (y - dragging_split_container_.top()) / std::max(1.0f, dragging_split_container_.height());
            }
            if (tab->tree.set_split_ratio(dragging_split_id_, ratio)) {
                recompute_layout(true);
                damage_.mark_full();
                request_redraw();
            }
        }
        return;
    }

    // Active scrollbar drag
    if (dragging_scrollbar_pane_ != 0) {
        if (auto* tab = active_tab()) {
            for (const auto& p : tab->layout) {
                if (p.pane_id == dragging_scrollbar_pane_) {
                    scrub_scrollbar(p, y);
                    return;
                }
            }
        }
        return;
    }

    // Active drag selection (continues smoothly even when pointer enters CSD or window edge)
    if (selecting_) {
        const auto* pane = pane_rect_by_id(selecting_pane_id_);
        if (!pane) pane = focused_pane_rect();
        if (pane && pane->session) {
            update_drag_selection(*pane, x, y);
            damage_.mark_full();
            request_redraw();
        }
        return;
    }

    tab_bar_.handle_hover(x, y);
    if (!opts_.quake_mode) {
        csd_frame_.handle_mouse_move(x, y, width_, height_, scale_);
    }

    // Divider hover cursor & highlight
    mux::DividerRect hit_div{};
    if (hit_test_divider(x, y, hit_div)) {
        if (hovered_split_id_ != hit_div.split_id) {
            hovered_split_id_ = hit_div.split_id;
            damage_.mark_full();
            request_redraw();
        }
        if (platform_) {
            platform_->setCursor(hit_div.axis == mux::SplitAxis::Horizontal
                                     ? nisaba::backend_os::SystemCursor::ResizeHorizontal
                                     : nisaba::backend_os::SystemCursor::ResizeVertical);
        }
        return;
    } else if (hovered_split_id_ != 0) {
        hovered_split_id_ = 0;
        if (platform_) platform_->setCursor(nisaba::backend_os::SystemCursor::Text);
        damage_.mark_full();
        request_redraw();
    }

    // Scrollbar hover check
    uint32_t new_sb_hover = 0;
    if (auto* tab = active_tab()) {
        for (const auto& p : tab->layout) {
            auto it = snapshots_.find(p.pane_id);
            if (it == snapshots_.end()) continue;
            nisaba::Rect tr{}, th{};
            if (compute_scrollbar_rects(p, it->second, tr, th)) {
                const float pad = 5.0f * scale_.u;
                if (x >= tr.left() - pad && x <= tr.right() + pad &&
                    y >= tr.top() && y <= tr.bottom()) {
                    new_sb_hover = p.pane_id;
                    break;
                }
            }
        }
    }
    if (new_sb_hover != hovered_scrollbar_pane_) {
        hovered_scrollbar_pane_ = new_sb_hover;
        if (platform_) {
            platform_->setCursor(new_sb_hover != 0 ? nisaba::backend_os::SystemCursor::Default
                                                   : nisaba::backend_os::SystemCursor::Text);
        }
        damage_.mark_full();
        request_redraw();
    }
    if (new_sb_hover != 0) return;

    if (const auto* pane = pane_rect_at(x, y); pane && pane->session) {
        int vrow = 0, lcol = 0;
        if (pixel_to_cell(*pane, x, y, vrow, lcol)) {
            // Update hyperlink hover state
            const std::string uri = pane->session->terminal().hyperlink_at(vrow, lcol);
            if (uri != hovered_url_) {
                hovered_url_ = uri;
                hovered_link_id_ = 0;
                if (!uri.empty()) {
                    const auto it = snapshots_.find(pane->pane_id);
                    if (it != snapshots_.end() && vrow < static_cast<int>(it->second.lines.size()) &&
                        lcol < static_cast<int>(it->second.lines[static_cast<size_t>(vrow)].cells.size())) {
                        const uint16_t ext_id = it->second.lines[static_cast<size_t>(vrow)].cells[static_cast<size_t>(lcol)].ext;
                        if (ext_id != 0) hovered_link_id_ = pane->session->terminal().get_ext_data(ext_id).hyperlink_id;
                    }
                }
                if (platform_) {
                    platform_->setCursor(hovered_url_.empty() ? nisaba::backend_os::SystemCursor::Text
                                                              : nisaba::backend_os::SystemCursor::Pointer);
                }
                damage_.mark_full();
                request_redraw();
            }

            // Mouse tracking motion reporting
            const auto modes = pane->session->terminal().modes();
            if (modes.mouse_mode != vt::MouseTrackingMode::None) {
                const std::string seq = InputEncoder::encode_mouse(
                    MouseEventKind::Move, 0, active_mods_, lcol, vrow,
                    static_cast<int>(x - pane->bounds.left()),
                    static_cast<int>(y - pane->bounds.top()), modes);
                if (!seq.empty()) pane->session->write_input(seq);
            }
        }
    }
}

void VaterApp::handle_mouse_down(float x, float y, int button) {
    mouse_x_ = x;
    mouse_y_ = y;

    // Context menu intercepts clicks first
    if (context_menu_.is_open()) {
        config::Action act = config::Action::None;
        std::function<void()> custom_fn;
        const bool inside = obsidian::ui::rect_contains(
            context_menu_.compute_bounds(width_, height_, scale_.u), x, y);
        context_menu_.handle_mouse_down(x, y, button, width_, height_, scale_.u, act, custom_fn);
        damage_.mark_full();
        request_redraw();
        if (act != config::Action::None) {
            execute_action(act);
            return;
        }
        if (custom_fn) {
            custom_fn();
            return;
        }
        // Right-clicking outside reopens context menu at the new pointer position
        if (!inside && button == 3) {
            open_context_menu(x, y);
        }
        return;
    }

    // Tab bar hit test before CSD titlebar drag
    if (button == 1 && tab_bar_.is_over_interactive(x, y)) {
        const auto click = tab_bar_.handle_click(x, y);
        if (click.kind == TabBarClick::Kind::NewTab) {
            create_new_tab();
            return;
        }
        if (click.kind == TabBarClick::Kind::SelectTab && click.tab_index >= 0 &&
            click.tab_index < static_cast<int>(tabs_.size())) {
            active_tab_idx_ = click.tab_index;
            selection_.active = false;
            damage_.mark_full();
            request_redraw();
            return;
        }
        if (click.kind == TabBarClick::Kind::CloseTab && click.tab_index >= 0 &&
            click.tab_index < static_cast<int>(tabs_.size())) {
            active_tab_idx_ = click.tab_index;
            close_active_pane();
            return;
        }
    }

    if (!opts_.quake_mode && csd_frame_.handle_mouse_down(x, y, button)) {
        request_redraw();
        return;
    }

    // Interactive split divider drag start
    if (button == 1) {
        mux::DividerRect hit_div{};
        if (hit_test_divider(x, y, hit_div)) {
            dragging_split_id_ = hit_div.split_id;
            dragging_split_axis_ = hit_div.axis;
            dragging_split_container_ = hit_div.container_bounds;
            return;
        }
    }

    // Interactive scrollbar click/drag start
    if (button == 1) {
        if (auto* tab = active_tab()) {
            for (const auto& p : tab->layout) {
                auto it = snapshots_.find(p.pane_id);
                if (it == snapshots_.end()) continue;
                nisaba::Rect tr{}, th{};
                if (compute_scrollbar_rects(p, it->second, tr, th)) {
                    const float pad = 5.0f * scale_.u;
                    if (x >= tr.left() - pad && x <= tr.right() + pad &&
                        y >= tr.top() && y <= tr.bottom()) {
                        dragging_scrollbar_pane_ = p.pane_id;
                        scrub_scrollbar(p, y);
                        return;
                    }
                }
            }
        }
    }

    const auto* pane = pane_rect_at(x, y);
    if (!pane || !pane->session) return;

    if (auto* tab = active_tab()) {
        if (tab->tree.focused_pane_id() != pane->pane_id) {
            tab->tree.set_focused_pane(pane->pane_id);
            recompute_layout(false);
            damage_.mark_full();
            request_redraw();
        }
    }

    int vrow = 0, lcol = 0;
    if (!pixel_to_cell(*pane, x, y, vrow, lcol)) return;

    // Middle-click paste from Primary selection (when mouse tracking inactive or Shift held)
    const auto modes = pane->session->terminal().modes();
    const bool bypass_mouse = (active_mods_ & 1) != 0; // Shift bypasses app mouse tracking
    if (button == 2 && (modes.mouse_mode == vt::MouseTrackingMode::None || bypass_mouse)) {
        paste_from_clipboard(nisaba::backend_os::ClipboardType::Primary, false);
        return;
    }

    // Right-click opens Obsidian Glass Context Menu (when mouse tracking inactive or Shift held)
    if (button == 3 && (modes.mouse_mode == vt::MouseTrackingMode::None || bypass_mouse)) {
        open_context_menu(x, y);
        return;
    }

    // Ctrl+Click opens OSC 8 hyperlink (§8 safe scheme check)
    if (button == 1 && (active_mods_ & 2) != 0 && !hovered_url_.empty()) {
        open_hyperlink(hovered_url_);
        return;
    }

    // Forward mouse press to terminal app if mouse tracking enabled
    if (modes.mouse_mode != vt::MouseTrackingMode::None && !bypass_mouse) {
        const std::string seq = InputEncoder::encode_mouse(
            MouseEventKind::Press, button, active_mods_, lcol, vrow,
            static_cast<int>(x - pane->bounds.left()),
            static_cast<int>(y - pane->bounds.top()), modes);
        if (!seq.empty()) pane->session->write_input(seq);
        return;
    }

    if (button == 1) {
        const auto now = std::chrono::steady_clock::now();
        const auto dt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_click_time_).count();
        if (dt_ms < 400 && std::hypot(x - last_click_x_, y - last_click_y_) < 8.0f) {
            click_streak_ = (click_streak_ % 4) + 1;
        } else {
            click_streak_ = 1;
        }
        last_click_time_ = now;
        last_click_x_ = x;
        last_click_y_ = y;

        const auto it = snapshots_.find(pane->pane_id);
        const int sb = (it != snapshots_.end() && !it->second.alt_screen)
                           ? static_cast<int>(it->second.scrollback_size) : 0;
        const int off = (it != snapshots_.end()) ? it->second.scroll_offset : 0;
        const int abs_row = sb - off + vrow;

        single_click_clear_timer_ = 0.0f;
        if (click_streak_ == 2) {
            // Double-click: word selection
            int ar = abs_row, c0 = lcol, c1 = lcol;
            if (pane->session->terminal().select_word_at(vrow, lcol, cfg_.word_delimiters, ar, c0, c1)) {
                selection_ = {true, false, ar, c0, ar, c1};
            }
            selecting_ = false;
        } else if (click_streak_ == 3) {
            // Triple-click: full line selection
            selection_ = {true, false, abs_row, 0, abs_row, pane->session->terminal().cols() - 1};
            selecting_ = false;
        } else if (click_streak_ == 4) {
            // Quad-click: semantic OSC 133 command output selection (§5.6)
            int r0 = abs_row, r1 = abs_row;
            if (pane->session->terminal().select_command_output_at(vrow, r0, r1)) {
                selection_ = {true, false, r0, 0, r1, pane->session->terminal().cols() - 1};
            }
            selecting_ = false;
        } else {
            selection_ = {true, (active_mods_ & 4) != 0, abs_row, lcol, abs_row, lcol};
            selecting_ = true;
            selecting_pane_id_ = pane->pane_id;
            selection_autoscroll_accum_ = 0.0f;
        }
        damage_.mark_full();
        request_redraw();
    }
}

void VaterApp::handle_mouse_up(float x, float y, int button) {
    if (button == 1 && dragging_split_id_ != 0) {
        dragging_split_id_ = 0;
        flush_pending_pty_resizes();
        damage_.mark_full();
        request_redraw();
        return;
    }
    if (button == 1 && dragging_scrollbar_pane_ != 0) {
        dragging_scrollbar_pane_ = 0;
        damage_.mark_full();
        request_redraw();
        return;
    }

    if (selecting_ && button == 1) {
        selecting_ = false;
        selecting_pane_id_ = 0;
        selection_autoscroll_accum_ = 0.0f;
        const bool is_single_cell = selection_.active &&
                                    selection_.start_abs_row == selection_.end_abs_row &&
                                    selection_.start_col == selection_.end_col;
        if (is_single_cell) {
            // Normal single click without drag: keep brief highlight then auto-clear after 0.5s
            single_click_clear_timer_ = 0.1f;
        } else if (selection_.active && platform_) {
            // Copy non-empty drag selection to Primary selection (X11/Wayland standard)
            if (auto* sess = focused_session()) {
                const std::string txt = sess->terminal().extract_text(
                    selection_.start_abs_row, selection_.start_col,
                    selection_.end_abs_row, selection_.end_col,
                    selection_.rectangular);
                if (!txt.empty()) {
                    platform_->setClipboardText(txt, nisaba::backend_os::ClipboardType::Primary);
                }
            }
        }
        return;
    }

    if (!opts_.quake_mode && csd_frame_.handle_mouse_up(x, y, button)) {
        request_redraw();
        return;
    }

    if (const auto* pane = pane_rect_at(x, y); pane && pane->session) {
        const auto modes = pane->session->terminal().modes();
        if (modes.mouse_mode != vt::MouseTrackingMode::None) {
            int vrow = 0, lcol = 0;
            if (pixel_to_cell(*pane, x, y, vrow, lcol)) {
                const std::string seq = InputEncoder::encode_mouse(
                    MouseEventKind::Release, button, active_mods_, lcol, vrow,
                    static_cast<int>(x - pane->bounds.left()),
                    static_cast<int>(y - pane->bounds.top()), modes);
                if (!seq.empty()) pane->session->write_input(seq);
            }
        }
    }
}

void VaterApp::handle_scroll(float x, float y, float dy) {
    if (preview_panel_.is_open() && x > terminal_viewport_rect().right()) {
        preview_panel_.scroll(dy);
        damage_.mark_full();
        request_redraw();
        return;
    }

    const auto* pane = pane_rect_at(x, y);
    if (!pane || !pane->session) return;

    const auto modes = pane->session->terminal().modes();
    const int lines = (dy > 0.0f) ? 3 : -3;

    if (modes.mouse_mode != vt::MouseTrackingMode::None) {
        int vrow = 0, lcol = 0;
        if (pixel_to_cell(*pane, x, y, vrow, lcol)) {
            const std::string seq = InputEncoder::encode_mouse(
                dy > 0.0f ? MouseEventKind::WheelUp : MouseEventKind::WheelDown,
                0, active_mods_, lcol, vrow,
                static_cast<int>(x - pane->bounds.left()),
                static_cast<int>(y - pane->bounds.top()), modes);
            if (!seq.empty()) pane->session->write_input(seq);
        }
        return;
    }

    // Alternate scroll mode (DEC 1007): convert wheel to Up/Down arrows inside vim/less
    if (modes.alt_screen && modes.alt_scroll) {
        const char* arrow = (dy > 0.0f) ? (modes.cursor_keys_app ? "\x1bOA" : "\x1b[A")
                                        : (modes.cursor_keys_app ? "\x1bOB" : "\x1b[B");
        for (int i = 0; i < 3; ++i) pane->session->write_input(arrow);
        return;
    }

    pane->session->terminal().scroll_view(lines);
    damage_.mark_full();
    request_redraw();
}

void VaterApp::handle_key_down(int keysym, int mods) {
    active_mods_ = mods;
    suppress_next_text_input_ = false;
    cursor_blink_on_ = true;
    blink_timer_ = 0.0f;

    // 0. Context Menu keyboard navigation
    if (context_menu_.is_open()) {
        suppress_next_text_input_ = true;
        config::Action act = config::Action::None;
        std::function<void()> custom_fn;
        if (context_menu_.handle_key(keysym, act, custom_fn)) {
            damage_.mark_full();
            request_redraw();
            if (act != config::Action::None) execute_action(act);
            else if (custom_fn) custom_fn();
            return;
        }
    }

    // 1. Multiline paste confirmation modal
    if (paste_confirm_open_) {
        suppress_next_text_input_ = true;
        if (keysym == 0xFF0D) { // Enter -> confirm paste
            paste_confirm_open_ = false;
            if (auto* sess = focused_session()) {
                sess->write_input(InputEncoder::encode_paste(pending_paste_text_, sess->terminal().modes()));
            }
            pending_paste_text_.clear();
            damage_.mark_full();
            request_redraw();
        } else if (keysym == 0xFF1B) { // Escape -> cancel
            paste_confirm_open_ = false;
            pending_paste_text_.clear();
            damage_.mark_full();
            request_redraw();
        }
        return;
    }

    // 1b. Unsafe hyperlink scheme confirmation modal (§8)
    if (link_confirm_open_) {
        suppress_next_text_input_ = true;
        if (keysym == 0xFF0D) { // Enter -> confirm open
            std::string uri = std::move(pending_link_uri_);
            pending_link_uri_.clear();
            link_confirm_open_ = false;
            open_hyperlink(uri, true);
            damage_.mark_full();
            request_redraw();
        } else if (keysym == 0xFF1B) { // Escape -> cancel
            pending_link_uri_.clear();
            link_confirm_open_ = false;
            damage_.mark_full();
            request_redraw();
        }
        return;
    }

    // 2. Command Palette overlay
    if (command_palette_.is_open()) {
        config::Action act = config::Action::None;
        if (command_palette_.handle_key(keysym, mods, act)) {
            if (keysym == 0xFF1B || keysym == 0xFF0D || keysym == 0xFF08 ||
                keysym == 0xFF52 || keysym == 0xFF54) {
                suppress_next_text_input_ = true;
            }
            if (act != config::Action::None) execute_action(act);
            damage_.mark_full();
            request_redraw();
            return;
        }
    }

    // 3. Search overlay
    if (search_overlay_.is_open()) {
        if (auto* sess = focused_session()) {
            if (search_overlay_.handle_key(keysym, mods, sess->terminal())) {
                if (keysym == 0xFF1B || keysym == 0xFF0D || keysym == 0xFF08 || (mods & 4)) {
                    suppress_next_text_input_ = true;
                }
                damage_.mark_full();
                request_redraw();
                return;
            }
        }
    }

    // 4. Configured Vater Keybindings
    const config::Action act = config::lookup_keybinding(cfg_.keybindings, keysym, mods);
    if (act != config::Action::None) {
        suppress_next_text_input_ = true;
        execute_action(act);
        return;
    }

    // 5. Encode key to focused PTY session
    if (auto* sess = focused_session()) {
        sess->terminal().scroll_to_bottom();
        bool suppress = false;
        const std::string seq = InputEncoder::encode_key(
            keysym, mods, KeyEventKind::Press, sess->terminal().modes(), suppress);
        if (suppress) suppress_next_text_input_ = true;
        if (!seq.empty()) {
            selection_.active = false;
            sess->write_input(seq);
        }
    }
}

void VaterApp::handle_key_up(int keysym, int mods) {
    active_mods_ = mods;
    if (auto* sess = focused_session()) {
        const auto modes = sess->terminal().modes();
        if (modes.kitty_flags() & vt::kitty_kbd::kReportEvents) {
            bool dummy = false;
            const std::string seq = InputEncoder::encode_key(
                keysym, mods, KeyEventKind::Release, modes, dummy);
            if (!seq.empty()) sess->write_input(seq);
        }
    }
}

void VaterApp::handle_text_input(std::string_view utf8) {
    if (suppress_next_text_input_) {
        suppress_next_text_input_ = false;
        return;
    }
    if (paste_confirm_open_ || link_confirm_open_ || context_menu_.is_open()) return;

    if (command_palette_.is_open()) {
        command_palette_.handle_text(utf8);
        damage_.mark_full();
        request_redraw();
        return;
    }
    if (search_overlay_.is_open()) {
        if (auto* sess = focused_session()) {
            search_overlay_.handle_text(utf8, sess->terminal());
            damage_.mark_full();
            request_redraw();
        }
        return;
    }

    if (auto* sess = focused_session()) {
        const std::string seq = InputEncoder::encode_text(utf8, sess->terminal().modes());
        if (!seq.empty()) {
            selection_.active = false;
            sess->terminal().scroll_to_bottom();
            sess->write_input(seq);
        }
    }
}

void VaterApp::handle_focus(bool focused) {
    window_focused_ = focused;
    if (auto* sess = focused_session()) {
        const std::string seq = InputEncoder::encode_focus(focused, sess->terminal().modes());
        if (!seq.empty()) sess->write_input(seq);
    }
    damage_.mark_full();
    request_redraw();
}

void VaterApp::handle_drop(const nisaba::backend_os::DropEvent& ev) {
    if (!ev.data) return;
    if (auto* sess = focused_session()) {
        std::string payload;
        const auto uris = ev.data->readUris();
        if (!uris.empty()) {
            for (size_t i = 0; i < uris.size(); ++i) {
                if (i > 0) payload.push_back(' ');
                std::string path = uris[i];
                if (path.starts_with("file://")) path.erase(0, 7);
                payload += InputEncoder::shell_quote_path(path);
            }
        } else {
            payload = ev.data->readText();
        }
        if (!payload.empty()) {
            sess->write_input(InputEncoder::encode_paste(payload, sess->terminal().modes()));
        }
    }
}

void VaterApp::copy_selection_to_clipboard() {
    if (!platform_ || !selection_.active) return;
    if (auto* sess = focused_session()) {
        const std::string txt = sess->terminal().extract_text(
            selection_.start_abs_row, selection_.start_col,
            selection_.end_abs_row, selection_.end_col,
            selection_.rectangular);
        if (!txt.empty()) {
            platform_->setClipboardText(txt, nisaba::backend_os::ClipboardType::Clipboard);
        }
    }
}

void VaterApp::paste_from_clipboard(nisaba::backend_os::ClipboardType type, bool confirmed) {
    if (!platform_) return;
    auto* sess = focused_session();
    if (!sess) return;

    const std::string txt = platform_->getClipboardText(type);
    if (txt.empty()) return;

    if (!confirmed && cfg_.confirm_multiline_paste &&
        !sess->terminal().modes().bracketed_paste &&
        InputEncoder::is_multiline_or_dangerous_paste(txt)) {
        pending_paste_text_ = txt;
        paste_confirm_open_ = true;
        damage_.mark_full();
        request_redraw();
        return;
    }

    sess->terminal().scroll_to_bottom();
    sess->write_input(InputEncoder::encode_paste(txt, sess->terminal().modes()));
}

void VaterApp::open_hyperlink(const std::string& uri, bool confirmed) {
    if (!confirmed && !is_safe_url_scheme(uri)) {
        pending_link_uri_ = uri;
        link_confirm_open_ = true;
        damage_.mark_full();
        request_redraw();
        return;
    }
    // Check if local Markdown/PDF/Image file:// -> open directly in sovereign side preview panel!
    if (uri.starts_with("file://")) {
        const std::string local_path = uri.substr(7);
        if (preview_panel_.open_file(local_path)) {
            recompute_layout(false);
            damage_.mark_full();
            request_redraw();
            return;
        }
    }
    pid_t pid = ::fork();
    if (pid == 0) {
        ::execlp("xdg-open", "xdg-open", uri.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
}

void VaterApp::open_context_menu(float x, float y) {
    const bool has_selection = selection_.active &&
                               (selection_.start_abs_row != selection_.end_abs_row ||
                                selection_.start_col != selection_.end_col);
    const auto* sess = focused_session();
    const bool is_rec = sess && sess->is_recording();

    std::vector<ContextMenuItem> items;
    if (!hovered_url_.empty()) {
        const std::string uri = hovered_url_;
        items.push_back({"Open Link", "Ctrl+Click", config::Action::None,
                         [this, uri]() { open_hyperlink(uri, false); }, true, false, false, false});
        items.push_back(ContextMenuItem::separator());
    }

    items.push_back({"Copy", "Ctrl+Shift+C", config::Action::Copy, {}, has_selection, false, false, false});
    items.push_back({"Paste", "Ctrl+Shift+V", config::Action::Paste, {}, true, false, false, false});
    items.push_back(ContextMenuItem::separator());

    items.push_back({"Split Right", "Ctrl+Shift+D", config::Action::SplitHorizontal, {}, true, false, false, false});
    items.push_back({"Split Down", "Ctrl+Shift+E", config::Action::SplitVertical, {}, true, false, false, false});
    items.push_back({"New Tab", "Ctrl+Shift+T", config::Action::NewTab, {}, true, false, false, false});
    items.push_back(ContextMenuItem::separator());

    items.push_back({"Search Scrollback", "Ctrl+Shift+F", config::Action::ToggleSearch, {}, true, search_overlay_.is_open(), false, false});
    items.push_back({"Command Palette", "Ctrl+Shift+P", config::Action::ToggleCommandPalette, {}, true, false, false, false});
    items.push_back({"Preview Panel", "Ctrl+Shift+M", config::Action::TogglePreviewPanel, {}, true, preview_panel_.is_open(), false, false});
    items.push_back(ContextMenuItem::separator());

    items.push_back({"Arabic BiDi", "Ctrl+Shift+B", config::Action::ToggleBidi, {}, true, cfg_.bidi_enabled, false, false});
    items.push_back({"Ligatures", "Ctrl+Shift+L", config::Action::ToggleLigatures, {}, true, cfg_.ligatures, false, false});
    items.push_back({"Retro CRT Shader", "Ctrl+Shift+G", config::Action::ToggleCrtEffect, {}, true, crt_effect_.enabled(), false, false});
    items.push_back({is_rec ? "Stop Recording" : "Record Asciicast", "", config::Action::ToggleRecording, {}, true, is_rec, false, false});
    items.push_back(ContextMenuItem::separator());

    items.push_back({"Clear Scrollback", "Ctrl+Shift+K", config::Action::ClearScrollback, {}, true, false, false, false});
    items.push_back({"Close Pane", "Ctrl+Shift+W", config::Action::ClosePane, {}, true, false, false, true});

    context_menu_.open(x, y, std::move(items));
    damage_.mark_full();
    request_redraw();
}

bool VaterApp::hit_test_divider(float x, float y, mux::DividerRect& out) const noexcept {
    if (tabs_.empty() || active_tab_idx_ < 0 || active_tab_idx_ >= static_cast<int>(tabs_.size())) {
        return false;
    }
    const float tol = 4.5f * scale_.u;
    const auto& tab = *tabs_[static_cast<size_t>(active_tab_idx_)];
    for (const auto& div : tab.dividers) {
        const float x0 = div.bounds.left() - (div.axis == mux::SplitAxis::Horizontal ? tol : 0.0f);
        const float x1 = div.bounds.right() + (div.axis == mux::SplitAxis::Horizontal ? tol : 0.0f);
        const float y0 = div.bounds.top() - (div.axis == mux::SplitAxis::Vertical ? tol : 0.0f);
        const float y1 = div.bounds.bottom() + (div.axis == mux::SplitAxis::Vertical ? tol : 0.0f);
        if (x >= x0 && x <= x1 && y >= y0 && y <= y1) {
            out = div;
            return true;
        }
    }
    return false;
}

bool VaterApp::compute_scrollbar_rects(const mux::PaneLayoutRect& pane,
                                       const vt::ViewportSnapshot& snap,
                                       nisaba::Rect& out_track,
                                       nisaba::Rect& out_thumb) const noexcept {
    if (snap.alt_screen || snap.scrollback_size == 0 || snap.rows <= 0) return false;
    const float u = scale_.u;
    const float track_w = 8.0f * u;
    const float margin_y = 4.0f * u;
    const float track_h = pane.bounds.height() - margin_y * 2.0f;
    if (track_h < 32.0f * u) return false;

    const float track_x = pane.bounds.right() - track_w - 2.0f * u;
    const float track_y = pane.bounds.top() + margin_y;

    const float total_lines = static_cast<float>(snap.scrollback_size + static_cast<size_t>(snap.rows));
    const float vis_ratio = std::clamp(static_cast<float>(snap.rows) / total_lines, 0.04f, 1.0f);
    const float thumb_h = std::clamp(track_h * vis_ratio, 24.0f * u, track_h);
    const float max_sb = static_cast<float>(snap.scrollback_size);
    const float frac_from_top = std::clamp(1.0f - (static_cast<float>(snap.scroll_offset) / max_sb), 0.0f, 1.0f);
    const float thumb_y = track_y + frac_from_top * (track_h - thumb_h);

    auto tr = nisaba::Rect::from_xywh(track_x, track_y, track_w, track_h);
    auto th = nisaba::Rect::from_xywh(track_x, thumb_y, track_w, thumb_h);
    if (!tr || !th) return false;
    out_track = *tr;
    out_thumb = *th;
    return true;
}

void VaterApp::scrub_scrollbar(const mux::PaneLayoutRect& pane, float y) {
    if (!pane.session) return;
    auto it = snapshots_.find(pane.pane_id);
    if (it == snapshots_.end() || it->second.scrollback_size == 0) return;
    nisaba::Rect tr{}, th{};
    if (!compute_scrollbar_rects(pane, it->second, tr, th)) return;

    const float usable_h = std::max(1.0f, tr.height() - th.height());
    const float rel = std::clamp((y - tr.top() - th.height() * 0.5f) / usable_h, 0.0f, 1.0f);
    const int offset = static_cast<int>(std::round((1.0f - rel) * static_cast<float>(it->second.scrollback_size)));
    pane.session->terminal().scroll_to_offset(offset);
    damage_.mark_full();
    request_redraw();
}

void VaterApp::execute_action(config::Action act) {
    switch (act) {
        case config::Action::Copy:
            copy_selection_to_clipboard();
            break;
        case config::Action::Paste:
            paste_from_clipboard(nisaba::backend_os::ClipboardType::Clipboard, false);
            break;
        case config::Action::NewTab:
            create_new_tab();
            break;
        case config::Action::ClosePane:
            close_active_pane();
            break;
        case config::Action::NextTab:
            if (tabs_.size() > 1) {
                active_tab_idx_ = (active_tab_idx_ + 1) % static_cast<int>(tabs_.size());
                damage_.mark_full();
                request_redraw();
            }
            break;
        case config::Action::PrevTab:
            if (tabs_.size() > 1) {
                active_tab_idx_ = (active_tab_idx_ - 1 + static_cast<int>(tabs_.size())) % static_cast<int>(tabs_.size());
                damage_.mark_full();
                request_redraw();
            }
            break;
        case config::Action::SplitHorizontal:
            split_active_pane(mux::SplitAxis::Horizontal);
            break;
        case config::Action::SplitVertical:
            split_active_pane(mux::SplitAxis::Vertical);
            break;
        case config::Action::FocusLeft:
        case config::Action::FocusRight:
        case config::Action::FocusUp:
        case config::Action::FocusDown:
            if (auto* tab = active_tab()) {
                mux::FocusDir dir = mux::FocusDir::Left;
                if (act == config::Action::FocusRight) dir = mux::FocusDir::Right;
                else if (act == config::Action::FocusUp) dir = mux::FocusDir::Up;
                else if (act == config::Action::FocusDown) dir = mux::FocusDir::Down;
                const uint32_t nb = tab->tree.find_neighbor(tab->tree.focused_pane_id(), dir, tab->layout);
                tab->tree.set_focused_pane(nb);
                recompute_layout(false);
                damage_.mark_full();
                request_redraw();
            }
            break;
        case config::Action::ToggleZoomPane:
            if (auto* tab = active_tab()) {
                tab->tree.set_zoomed(!tab->tree.is_zoomed());
                recompute_layout(false);
                damage_.mark_full();
                request_redraw();
            }
            break;
        case config::Action::FontIncrease:
            font_stack_.set_size(font_stack_.metrics().font_size + 1.0f, cfg_.line_height);
            recompute_layout(false);
            damage_.mark_full();
            request_redraw();
            break;
        case config::Action::FontDecrease:
            font_stack_.set_size(font_stack_.metrics().font_size - 1.0f, cfg_.line_height);
            recompute_layout(false);
            damage_.mark_full();
            request_redraw();
            break;
        case config::Action::FontReset:
            font_stack_.set_size(cfg_.font_size, cfg_.line_height);
            recompute_layout(false);
            damage_.mark_full();
            request_redraw();
            break;
        case config::Action::ToggleSearch:
            if (search_overlay_.is_open()) search_overlay_.close();
            else search_overlay_.open();
            damage_.mark_full();
            request_redraw();
            break;
        case config::Action::ToggleCommandPalette:
            command_palette_.toggle();
            damage_.mark_full();
            request_redraw();
            break;
        case config::Action::ToggleBidi:
            cfg_.bidi_enabled = !cfg_.bidi_enabled;
            if (auto* s = focused_session()) s->terminal().set_bidi_default(cfg_.bidi_enabled);
            damage_.mark_full();
            request_redraw();
            break;
        case config::Action::ToggleLigatures:
            cfg_.ligatures = !cfg_.ligatures;
            damage_.mark_full();
            request_redraw();
            break;
        case config::Action::ToggleCrtEffect:
            crt_effect_.toggle();
            damage_.mark_full();
            request_redraw();
            break;
        case config::Action::ScrollPageUp:
            if (auto* s = focused_session()) {
                s->terminal().scroll_view(std::max(1, s->terminal().rows() - 2));
                damage_.mark_full();
                request_redraw();
            }
            break;
        case config::Action::ScrollPageDown:
            if (auto* s = focused_session()) {
                s->terminal().scroll_view(-std::max(1, s->terminal().rows() - 2));
                damage_.mark_full();
                request_redraw();
            }
            break;
        case config::Action::ScrollToBottom:
            if (auto* s = focused_session()) {
                s->terminal().scroll_to_bottom();
                damage_.mark_full();
                request_redraw();
            }
            break;
        case config::Action::PrevPrompt:
            if (auto* s = focused_session()) {
                s->terminal().jump_to_prompt(true);
                damage_.mark_full();
                request_redraw();
            }
            break;
        case config::Action::NextPrompt:
            if (auto* s = focused_session()) {
                s->terminal().jump_to_prompt(false);
                damage_.mark_full();
                request_redraw();
            }
            break;
        case config::Action::ReloadConfig:
            reload_config();
            break;
        case config::Action::TogglePreviewPanel:
            preview_panel_.toggle_help();
            recompute_layout(false);
            damage_.mark_full();
            request_redraw();
            break;
        case config::Action::ToggleRecording:
            if (auto* s = focused_session()) {
                if (s->is_recording()) {
                    s->stop_recording();
                } else {
                    const std::string path = "/tmp/vater-" + std::to_string(std::time(nullptr)) + ".cast";
                    s->start_recording(path);
                }
                damage_.mark_full();
                request_redraw();
            }
            break;
        case config::Action::ClearScrollback:
            if (auto* s = focused_session()) {
                s->terminal().feed("\x1b[3J");
                damage_.mark_full();
                request_redraw();
            }
            break;
        default:
            break;
    }
}

} // namespace vater::ui

