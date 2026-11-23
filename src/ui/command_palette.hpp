#pragma once
/// @file command_palette.hpp
/// @brief L5 UI — Obsidian Glass Command Palette (`Ctrl+Shift+P`) with fuzzy subsequence matching.

#include "config/schema.hpp"
#include "nisaba/gpu/gpu_canvas.hpp"
#include "theme.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace vater::ui {

struct PaletteCommand {
    std::string title;
    std::string shortcut;
    config::Action action{config::Action::None};
    std::function<void()> custom_fn;
};

class CommandPalette {
public:
    CommandPalette();

    void register_commands(std::vector<PaletteCommand> cmds);
    void open();
    void close();
    void toggle() { if (open_) close(); else open(); }
    [[nodiscard]] bool is_open() const noexcept { return open_; }

    /// Handles keyboard input; returns triggered Action (or Action::None) if executed.
    bool handle_key(int keysym, int mods, config::Action& out_action);
    bool handle_text(std::string_view utf8);

    void render(nisaba::gpu::GpuCanvas& canvas, float width, float height,
                const obsidian::ui::AppTheme& theme, const obsidian::ui::UnitScale& scale);

private:
    void refilter();

    bool open_{false};
    std::string query_;
    std::vector<PaletteCommand> commands_;
    std::vector<size_t> filtered_;
    int selected_{0};
};

} // namespace vater::ui

