#pragma once
/// @file terminal.hpp
/// @brief L3 Terminal Core — Thread-Safe Public Terminal Facade (feed / snapshot / resize).
/// Implements DEC 2026 Synchronized Output frame freezing and short-mutex dirty-row snapshots.
/// Strictly Nisaba-agnostic (term_core).

#include "vt/dispatch.hpp"
#include "vt/graphics/kitty_graphics.hpp"
#include "vt/grid.hpp"
#include "vt/parser.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace vater::vt {

/// Search match location in absolute scrollback + live grid coordinates.
struct SearchMatch {
    int abs_row{0}; // 0..scrollback_size + rows - 1
    int col_start{0};
    int col_end{0}; // exclusive
};

/// Main thread-safe Terminal Core instance (L3).
/// I/O thread calls `feed()`, UI thread calls `snapshot()` under a short mutex.
class Terminal {
public:
    Terminal(int cols = 80, int rows = 24, size_t max_scrollback = Scrollback::kDefaultMaxLines);

    /// Feeds raw bytes received from PTY into VT Parser -> Dispatcher -> Grid.
    void feed(const uint8_t* data, size_t len);
    void feed(std::string_view bytes) {
        feed(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    }

    /// Resizes grid with full logical reflow preserving cursor position.
    void resize(int cols, int rows);

    /// Captures visible viewport rows (respecting DEC 2026 synchronized output freeze).
    /// Returns true if a fresh snapshot was copied (false if frozen in sync output).
    bool snapshot(ViewportSnapshot& out, bool consume_dirty = true);

    /// Scroll viewport up/down in scrollback (positive delta scrolls up into history).
    void scroll_view(int delta_lines);
    void scroll_to_bottom();
    void scroll_to_offset(int offset);

    /// Configure callbacks, clipboard policy, PNG decoder, cell pixel geometry, and scrollback limit.
    void set_callbacks(DispatcherCallbacks cb);
    void set_clipboard_policy(ClipboardPolicy pol);
    void set_png_decoder(PngDecodeFn fn);
    void set_zlib_decompressor(ZlibDecompressFn fn);
    void set_cell_pixel_size(int cell_w, int cell_h);
    void set_max_scrollback(size_t max_lines);
    void set_bidi_default(bool enabled);

    /// Thread-safe queries for UI & Input Encoder.
    [[nodiscard]] TerminalModes modes() const;
    [[nodiscard]] int cols() const;
    [[nodiscard]] int rows() const;
    [[nodiscard]] int scroll_offset() const;
    [[nodiscard]] size_t scrollback_size() const;
    [[nodiscard]] std::string title() const;
    [[nodiscard]] std::string cwd() const;
    [[nodiscard]] int last_exit_code() const;
    [[nodiscard]] DynamicColorState dynamic_colors() const;
    [[nodiscard]] bool has_dirty() const;

    /// Selection text extraction (absolute coordinates = scrollback_size - scroll_offset + view_row).
    [[nodiscard]] std::string extract_text(int start_abs_row, int start_col,
                                           int end_abs_row, int end_col,
                                           bool rectangular = false) const;

    /// Finds word boundaries around (view_row, col) using configurable delimiter set.
    bool select_word_at(int view_row, int col, std::string_view delimiters,
                        int& out_abs_row, int& out_col_start, int& out_col_end) const;

    /// Semantic selection (OSC 133): selects full command output around view_row.
    bool select_command_output_at(int view_row, int& out_start_abs_row, int& out_end_abs_row) const;

    /// Jump to previous or next OSC 133 prompt row (adjusts scroll_offset).
    bool jump_to_prompt(bool previous);

    /// Resolves hyperlink URI at a given viewport cell (empty if none).
    [[nodiscard]] std::string hyperlink_at(int view_row, int col) const;

    /// Resolves extended cell data & grapheme cluster UTF-8 (for rendering).
    [[nodiscard]] ExtCellData get_ext_data(uint16_t ext_id) const;
    [[nodiscard]] std::u32string get_grapheme(char32_t cp) const;

    /// Returns a snapshot copy of active inline image placements and image lookup.
    void copy_graphics(std::vector<ImagePlacement>& out_placements,
                       std::unordered_map<uint32_t, DecodedImage>& out_images,
                       uint64_t& in_out_generation) const;

    /// Extracts all logical/physical lines (scrollback + live) as UTF-8 strings for search overlay.
    void dump_all_lines_utf8(std::vector<std::string>& out_lines, size_t& out_scrollback_count) const;

    /// Direct unlocked access for single-threaded unit/conformance/fuzz tests.
    [[nodiscard]] Grid& unsafe_grid() noexcept { return grid_; }
    [[nodiscard]] const Grid& unsafe_grid() const noexcept { return grid_; }
    [[nodiscard]] const GraphicsStore& unsafe_graphics() const noexcept { return graphics_; }

private:
    mutable std::mutex mutex_;
    Grid grid_;
    GraphicsStore graphics_;
    Parser parser_;
    Dispatcher dispatcher_;

    // DEC 2026 synchronized output safety timeout (max 200ms freeze to prevent stuck apps)
    std::chrono::steady_clock::time_point sync_started_{};
    bool was_sync_active_{false};
};

} // namespace vater::vt

