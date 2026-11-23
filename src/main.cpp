/// @file main.cpp
/// @brief Vater — Sovereign Native C++20 Terminal Emulator on Nisaba Engine.
/// Platform abstraction strictly via `nisaba::backend_os::Platform` (zero direct Wayland/X11 calls).

#include "mux/daemon/vaterd.hpp"
#include "nisaba/backend_os/platform.hpp"
#include "nisaba/backend_os/window.hpp"
#include "nisaba/color/color.hpp"
#include "nisaba/gpu/gpu_canvas.hpp"
#include "nisaba/gpu/gpu_device.hpp"
#include "nisaba/gpu/gpu_surface.hpp"
#include "os/ipc.hpp"
#include "ui/app.hpp"
#include "ui/quake_mode.hpp"

#ifdef NISABA_GLEW
#   include <GL/glew.h>
#endif

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace nisaba;
using nisaba::gpu::GpuCanvas;
using nisaba::gpu::GpuDevice;
using nisaba::gpu::GpuSurface;

namespace {

void print_usage(const char* argv0) {
    std::printf(
        "Usage: %s [options] [-- command [args...]]\n"
        "Vater — Sovereign Native Terminal Emulator (Nisaba Engine)\n\n"
        "Options:\n"
        "  -h, --help                Show this help message\n"
        "  -v, --version             Show version\n"
        "  -c, --config PATH         Custom config.json path\n"
        "  -d, --dir PATH            Initial working directory\n"
        "  --quake                   Launch in Quake drop-down overlay mode (LayerSurface)\n"
        "  --software                Force CPU software fallback renderer\n"
        "  --attach [SESSION_ID]     Attach to persistent vaterd session (0 = create new)\n"
        "  --list-sessions           List active sessions hosted by vaterd\n"
        "  --socket PATH             Custom vaterd Unix socket path\n"
        "  --preview FILE            Open Markdown/PDF/Image in side preview panel\n"
        "  --play FILE.cast          Play back an Asciicast v2 recording\n"
        "  --dark                    Force Obsidian Dark Mode\n"
        "  --light                   Force Obsidian Light Mode\n"
        "  -e CMD [ARGS...]          Execute command inside terminal\n",
        argv0);
}

bool register_ui_sans_font(GpuCanvas& canvas) {
    static constexpr const char* kSansCandidates[] = {
        "/usr/share/obsidian_shell/fonts/Inter-Regular.ttf",
        "/usr/local/share/obsidian_shell/fonts/Inter-Regular.ttf",
        "/usr/share/fonts/truetype/inter/Inter-Regular.ttf",
        "/usr/share/fonts/TTF/Inter-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    };
    for (const char* path : kSansCandidates) {
        if (canvas.create_font("sans", path) >= 0) return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    ::signal(SIGPIPE, SIG_IGN);

    vater::ui::AppLaunchOptions opts;
    bool list_sessions = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "-v" || arg == "--version") {
            std::printf("vater %s (Nisaba Engine)\n", VATER_VERSION);
            return 0;
        }
        if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
            opts.config_path = argv[++i];
        } else if ((arg == "-d" || arg == "--dir") && i + 1 < argc) {
            opts.working_dir = argv[++i];
        } else if (arg == "--quake") {
            opts.quake_mode = true;
        } else if (arg == "--software") {
            opts.software_renderer = true;
        } else if (arg == "--dark") {
            opts.force_dark = true;
        } else if (arg == "--light") {
            opts.force_light = true;
        } else if (arg == "--list-sessions") {
            list_sessions = true;
        } else if (arg == "--socket" && i + 1 < argc) {
            opts.daemon_socket = argv[++i];
        } else if (arg == "--preview" && i + 1 < argc) {
            opts.preview_file = argv[++i];
        } else if (arg == "--play" && i + 1 < argc) {
            opts.playback_file = argv[++i];
        } else if (arg == "--attach") {
            opts.attach_daemon = true;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                opts.attach_session_id = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            }
        } else if (arg == "-e" || arg == "--") {
            if (i + 1 < argc) {
                opts.command = argv[++i];
                for (++i; i < argc; ++i) opts.command_args.emplace_back(argv[i]);
            }
            break;
        }
    }

    if (list_sessions) {
        const std::string sock = opts.daemon_socket.empty()
                                     ? vater::os::default_vaterd_socket_path(false)
                                     : opts.daemon_socket;
        std::vector<vater::daemon::DaemonSessionInfo> list;
        if (!vater::daemon::query_daemon_sessions(sock, list)) {
            std::fprintf(stderr, "vater: could not connect to vaterd at %s\n", sock.c_str());
            return 1;
        }
        if (list.empty()) {
            std::printf("No active vaterd sessions.\n");
        } else {
            for (const auto& s : list) {
                std::printf("  session #%u  %ux%u  [%s]  %s\n",
                            s.id, s.cols, s.rows, s.alive ? "alive" : "exited", s.cwd.c_str());
            }
        }
        return 0;
    }

    auto plat_res = backend_os::Platform::create();
    if (!plat_res.isOk()) {
        std::cerr << "vater: failed to initialize Nisaba Platform\n";
        return 1;
    }
    auto platform = std::move(plat_res.value());

    uint32_t win_w = 1040;
    uint32_t win_h = 680;

    std::unique_ptr<backend_os::Window> window;
    vater::ui::QuakeSurface quake;

    if (opts.quake_mode && quake.create(*platform, 0.48f)) {
        auto sz = quake.surface()->getDrawableSize();
        if (sz.width > 0 && sz.height > 0) {
            win_w = static_cast<uint32_t>(sz.width);
            win_h = static_cast<uint32_t>(sz.height);
        }
        quake.surface()->makeCurrent();
    } else {
        backend_os::WindowConfig wcfg;
        wcfg.title = "Vater";
        wcfg.app_id = "vater";
        wcfg.vsync = true;
        wcfg.transparent = true;
        wcfg.blur = true;
        wcfg.width = static_cast<int>(win_w);
        wcfg.height = static_cast<int>(win_h);
        wcfg.min_width = 480;
        wcfg.min_height = 320;
        wcfg.borderless = false;
        wcfg.csd = true;

        auto win_res = backend_os::Window::create(*platform, wcfg);
        if (!win_res.isOk()) {
            std::cerr << "vater: failed to create native window\n";
            return 1;
        }
        window = std::move(win_res.value());
        window->makeCurrent();

        auto sz = window->getDrawableSize();
        if (sz.width > 0 && sz.height > 0) {
            win_w = static_cast<uint32_t>(sz.width);
            win_h = static_cast<uint32_t>(sz.height);
        }
    }

#ifdef NISABA_GLEW
    glewExperimental = GL_TRUE;
    glewInit();
    while (glGetError() != GL_NO_ERROR) {}
#endif

    auto device = GpuDevice::create();
    if (!device) {
        std::cerr << "vater: failed to initialize GPU device\n";
        return 1;
    }

    auto surface = GpuSurface::from_screen(device, win_w, win_h, 0);
    auto canvas = std::make_unique<GpuCanvas>(surface);
    register_ui_sans_font(*canvas);

    vater::ui::VaterApp app(std::move(opts), static_cast<float>(win_w), static_cast<float>(win_h));
    app.initialize(platform.get(), window.get(), canvas.get());

    bool running = true;
    platform->onQuit().connect([&]() { running = false; });

    auto apply_resize = [&](uint32_t nw, uint32_t nh) {
        if (nw == 0 || nh == 0 || (nw == win_w && nh == win_h)) return;
        win_w = nw;
        win_h = nh;
        canvas->resize(win_w, win_h);
        app.resize(static_cast<float>(win_w), static_cast<float>(win_h));
    };

    if (window) {
        window->onClose().connect([&]() { running = false; });
        window->onFocus().connect([&](bool focused) { app.handle_focus(focused); });
        window->onResize().connect([&](int w, int h) {
            auto dsz = window->getDrawableSize();
            apply_resize(static_cast<uint32_t>(dsz.width > 0 ? dsz.width : w),
                         static_cast<uint32_t>(dsz.height > 0 ? dsz.height : h));
        });
    } else if (quake.is_active()) {
        quake.surface()->onClose().connect([&]() { running = false; });
        quake.surface()->onResize().connect([&](int w, int h) {
            auto dsz = quake.surface()->getDrawableSize();
            apply_resize(static_cast<uint32_t>(dsz.width > 0 ? dsz.width : w),
                         static_cast<uint32_t>(dsz.height > 0 ? dsz.height : h));
        });
    }

    float cur_mx = 0.0f;
    float cur_my = 0.0f;

    platform->onMouseMove().connect([&](float x, float y) {
        cur_mx = x; cur_my = y;
        app.handle_mouse_move(x, y);
    });
    platform->onMouseDown().connect([&](float x, float y, int button) {
        cur_mx = x; cur_my = y;
        app.handle_mouse_down(x, y, button);
    });
    platform->onMouseUp().connect([&](float x, float y, int button) {
        cur_mx = x; cur_my = y;
        app.handle_mouse_up(x, y, button);
    });
    platform->onScroll().connect([&](float /*dx*/, float dy) {
        app.handle_scroll(cur_mx, cur_my, dy);
    });
    platform->onKeyDown().connect([&](int key, int mods) {
        if (quake.is_active() && key == 0xFFC9 && mods == 0) { // F12 toggles Quake overlay
            quake.toggle_visible();
            return;
        }
        app.handle_key_down(key, mods);
    });
    platform->onKeyUp().connect([&](int key, int mods) {
        app.handle_key_up(key, mods);
    });
    platform->onTextInput().connect([&](std::string_view text) {
        app.handle_text_input(text);
    });
    platform->onDrop().connect([&](backend_os::DropEvent& ev) {
        app.handle_drop(ev);
    });

    auto prev_time = std::chrono::steady_clock::now();
    std::vector<ScreenIntRect> damage_screen;
    std::vector<Rect> damage_float;

    while (running && !app.should_quit()) {
        platform->pollEvents();

        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - prev_time).count();
        prev_time = now;
        if (dt > 0.1f) dt = 0.1f;

        if (window) {
            auto dsz = window->getDrawableSize();
            if (dsz.width > 0 && dsz.height > 0) {
                apply_resize(static_cast<uint32_t>(dsz.width), static_cast<uint32_t>(dsz.height));
            }
        }

        app.update(dt);
        if (app.should_quit()) break;

        if (!app.needs_redraw()) {
            // Near-zero idle CPU when nothing is damaged (§7)
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            continue;
        }

        if (window) window->makeCurrent();
        else if (quake.is_active()) quake.surface()->makeCurrent();

        canvas->clear(Color::from_rgba8(0, 0, 0, 0));
        app.render(*canvas, damage_screen, damage_float);
        canvas->flush();

        if (window) {
            if (!damage_float.empty()) {
                std::vector<backend_os::Rect> os_damage;
                os_damage.reserve(damage_float.size());
                for (const auto& r : damage_float) {
                    os_damage.push_back(backend_os::Rect{r.left(), r.top(), r.width(), r.height()});
                }
                window->setDamage(os_damage);
            }
            window->swapBuffers();
        } else if (quake.is_active()) {
            quake.surface()->swapBuffers();
        }
    }

    return 0;
}

