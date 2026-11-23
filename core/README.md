# Obsidian Core UI Kit (Drop-In for Native Nisaba Apps)

Self-contained, zero-boilerplate core files ready to copy directly into any native **Nisaba** C++20 application — or embed directly inside `nisaba/backend_os`.

## Files (100% Standalone)

| File | Namespace | Purpose |
| :--- | :--- | :--- |
| `theme.hpp` | `obsidian::ui` | `AppTheme::light()`, `AppTheme::dark()`, `AppTheme::live(r, g, b)` (Obsidian HSL Monet), `AppTheme::lerp()`, `UnitScale`, `rect_contains` |
| `theme_client.hpp` | `obsidian` | Header-only Unix Socket IPC client (`$XDG_RUNTIME_DIR/obsidian-shell.sock`) for live `mode` (`dark`/`light`/`live`), `live_seed`, and `apps_opacity` with disk fallback |
| `csd_frame.hpp` / `csd_frame.cpp` | `obsidian::ui` | Frameless Obsidian CSD window surface (with live `window_opacity`), titlebar, Close/Minimize/Maximize plates with built-in SVG glyphs, drag & resize handles |

## Drop-In Usage (~12 lines)

```cpp
#include "theme.hpp"
#include "theme_client.hpp"
#include "csd_frame.hpp"

obsidian::ThemeClient theme_client;
obsidian::ui::AppTheme theme = obsidian::ui::AppTheme::dark();
obsidian::ui::AppTheme target_theme = theme;
float opacity = 0.85f, target_opacity = 0.85f;

// On startup:
theme_client.start([&](const obsidian::ThemeEvent& ev) {
    // Store event + set atomic dirty flag, then apply on main GUI thread:
    target_opacity = ev.apps_opacity;
    if (ev.is_light())      target_theme = obsidian::ui::AppTheme::light();
    else if (ev.is_live())  { uint8_t r=80,g=227,b=194; ev.parse_hex_rgb(ev.live_seed, r, g, b);
                              target_theme = obsidian::ui::AppTheme::live(r, g, b); }
    else                    target_theme = obsidian::ui::AppTheme::dark();
});

// In render():
csd_frame.render_frame_background(canvas, width, height, theme, scale, opacity);
csd_frame.render_titlebar(canvas, width, height, theme, scale, "My App", 240.0f);
```

