#pragma once
/// @file search_overlay.hpp
/// @brief L5 UI — Internal Thompson NFA Regex Engine (zero `std::regex`!) + Scrollback Search Overlay.

#include "nisaba/gpu/gpu_canvas.hpp"
#include "nisaba/math/rect.hpp"
#include "theme.hpp"
#include "vt/terminal.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vater::ui {

/// Sovereign O(N*M) linear-time Thompson NFA regex matcher (ZERO `std::regex`, ReDoS-proof!).
/// Supports: `.`, `*`, `+`, `?`, `|`, `(...)`, `[a-z0-9]`, `[^...]`, `^`, `$`, `\d`, `\w`, `\s`, escapes.
class ThompsonRegex {
public:
    bool compile(std::string_view pattern, bool case_sensitive = false);
    [[nodiscard]] bool is_valid() const noexcept { return valid_; }

    /// Finds all non-overlapping matches in `text` (returns byte [start, end) ranges).
    void find_all(std::string_view text, std::vector<std::pair<size_t, size_t>>& out_matches) const;

private:
    enum class Op : uint8_t {
        Char,
        Any,
        Class,
        Split,
        AnchorStart,
        AnchorEnd,
        Match,
    };

    struct State {
        Op op{Op::Match};
        uint8_t ch{0};
        bool negated{false};
        uint8_t char_bitmap[32]{}; // 256-bit bitmap for `[...]` classes
        int out1{-1};
        int out2{-1};
    };

    bool match_at(std::string_view text, size_t start, size_t& out_end) const;
    void add_epsilon(int s, std::vector<int>& list, std::vector<uint32_t>& visited,
                     uint32_t gen, size_t pos, size_t text_len) const;

    std::vector<State> states_;
    int start_state_{-1};
    bool case_sensitive_{false};
    bool valid_{false};
};

/// Floating Obsidian-styled Search Overlay searching live screen + scrollback via ThompsonRegex.
class SearchOverlay {
public:
    SearchOverlay() = default;

    void open();
    void close();
    [[nodiscard]] bool is_open() const noexcept { return open_; }

    /// Updates query and executes search across `terminal`'s scrollback + live grid.
    void run_search(const vt::Terminal& terminal);

    /// Navigation between matches (scrolls terminal to active match).
    void next_match(vt::Terminal& terminal);
    void prev_match(vt::Terminal& terminal);

    /// Handles keyboard & text input while search overlay is open. Returns true if consumed.
    bool handle_key(int keysym, int mods, vt::Terminal& terminal);
    bool handle_text(std::string_view utf8, vt::Terminal& terminal);

    void render(nisaba::gpu::GpuCanvas& canvas, float width, float titlebar_h,
                const obsidian::ui::AppTheme& theme, const obsidian::ui::UnitScale& scale);

    [[nodiscard]] const std::vector<vt::SearchMatch>& matches() const noexcept { return matches_; }
    [[nodiscard]] int active_index() const noexcept { return active_idx_; }
    [[nodiscard]] const std::string& query() const noexcept { return query_; }

private:
    void scroll_to_active(vt::Terminal& terminal);

    bool open_{false};
    bool regex_mode_{true};
    bool case_sensitive_{false};
    std::string query_;
    ThompsonRegex regex_;
    std::vector<vt::SearchMatch> matches_;
    int active_idx_{-1};
    size_t last_scrollback_count_{0};
};

} // namespace vater::ui

