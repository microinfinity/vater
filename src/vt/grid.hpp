#pragma once
/// @file grid.hpp
/// @brief L3 Terminal Core — Screen Model: Live Grid, Alternate Screen, Scroll Margins (DECSTBM/DECSLRM),
/// Reflow, Tab Stops, Dirty-Row Tracking, and Thread-Safe Snapshots.
/// Strictly Nisaba-agnostic (term_core).

#include "vt/cell.hpp"
#include "vt/charset.hpp"
#include "vt/modes.hpp"
#include "vt/scrollback.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vater::vt {

struct CursorState {
    int col{0};
    int row{0};
    bool pending_wrap{false};
};

struct SavedCursor {
    CursorState cursor{};
    CellPen pen{};
    CharsetState charsets{};
    bool origin_mode{false};
    bool auto_wrap{true};
    bool valid{false};
};

/// Single row of the active terminal screen.
struct GridRow {
    std::vector<Cell> cells;
    RowMeta meta{};
    uint16_t used_cols{0};
    bool has_wide{false};
    bool dirty{true};
    uint64_t generation{1};
};

/// Immutable snapshot of visible viewport rows for lock-free/short-lock L4 rendering.
struct ViewportSnapshot {
    int cols{80};
    int rows{24};
    int scroll_offset{0};       // 0 = live bottom, >0 = lines scrolled up into scrollback
    size_t scrollback_size{0};
    CursorState cursor{};
    bool cursor_visible{true};
    CursorShape cursor_shape{CursorShape::Block};
    bool cursor_blink{true};
    bool reverse_video{false};
    bool bidi_implicit{true};
    bool alt_screen{false};
    uint64_t frame_generation{0};
    std::vector<GridRow> lines; // Exactly `rows` visible rows
    std::vector<bool> dirty_rows;
};

/// Complete terminal screen model (primary + alt screen, scrollback, margins, tabs, reflow).
class Grid {
public:
    Grid(int cols = 80, int rows = 24, size_t max_scrollback = Scrollback::kDefaultMaxLines);

    [[nodiscard]] int cols() const noexcept { return cols_; }
    [[nodiscard]] int rows() const noexcept { return rows_; }
    [[nodiscard]] const CursorState& cursor() const noexcept { return cursor_; }
    [[nodiscard]] CursorState& cursor_mut() noexcept { return cursor_; }
    [[nodiscard]] const CellPen& pen() const noexcept { return pen_; }
    [[nodiscard]] CellPen& pen_mut() noexcept { return pen_; }
    [[nodiscard]] const TerminalModes& modes() const noexcept { return modes_; }
    [[nodiscard]] TerminalModes& modes_mut() noexcept { return modes_; }
    [[nodiscard]] const CharsetState& charsets() const noexcept { return charsets_; }
    [[nodiscard]] CharsetState& charsets_mut() noexcept { return charsets_; }

    [[nodiscard]] ExtCellTable& ext_table() noexcept { return ext_table_; }
    [[nodiscard]] const ExtCellTable& ext_table() const noexcept { return ext_table_; }
    [[nodiscard]] GraphemeTable& grapheme_table() noexcept { return grapheme_table_; }
    [[nodiscard]] const GraphemeTable& grapheme_table() const noexcept { return grapheme_table_; }
    [[nodiscard]] HyperlinkTable& hyperlink_table() noexcept { return hyperlink_table_; }
    [[nodiscard]] const HyperlinkTable& hyperlink_table() const noexcept { return hyperlink_table_; }
    [[nodiscard]] Scrollback& scrollback() noexcept { return scrollback_; }
    [[nodiscard]] const Scrollback& scrollback() const noexcept { return scrollback_; }

    [[nodiscard]] GridRow& row(int r) noexcept { return active_row(r); }
    [[nodiscard]] const GridRow& row(int r) const noexcept { return active_row(r); }
    [[nodiscard]] Cell& cell(int r, int c) noexcept { return active_row(r).cells[static_cast<size_t>(c)]; }
    [[nodiscard]] const Cell& cell(int r, int c) const noexcept { return active_row(r).cells[static_cast<size_t>(c)]; }

    // Scroll margins (0-indexed inclusive)
    [[nodiscard]] int scroll_top() const noexcept { return scroll_top_; }
    [[nodiscard]] int scroll_bottom() const noexcept { return scroll_bottom_; }
    [[nodiscard]] int scroll_left() const noexcept { return modes_.allow_left_right_margin ? scroll_left_ : 0; }
    [[nodiscard]] int scroll_right() const noexcept { return modes_.allow_left_right_margin ? scroll_right_ : (cols_ - 1); }

    void set_scroll_region(int top, int bottom);
    void set_left_right_margins(int left, int right);
    void reset_margins() noexcept;

    // Text writing
    void put_char(char32_t cp);
    void put_ascii_run(const uint8_t* data, size_t len);
    void refresh_pen_ext();

    // Control characters & cursor motion
    void carriage_return() noexcept;
    void line_feed();
    void reverse_index();
    void backspace() noexcept;
    void horizontal_tab(int count = 1) noexcept;
    void backward_tab(int count = 1) noexcept;
    void set_tab_stop() noexcept;
    void clear_tab_stop(int mode) noexcept; // 0 = current col, 3 = all
    void reset_tab_stops();

    void move_cursor_abs(int row, int col) noexcept; // Respects DECOM
    void move_cursor_rel(int dr, int dc) noexcept;
    void set_cursor_col(int col) noexcept;
    void set_cursor_row(int row) noexcept;

    void save_cursor() noexcept;
    void restore_cursor() noexcept;

    // Editing & scrolling
    void scroll_up(int count, int top, int bottom);
    void scroll_down(int count, int top, int bottom);
    void erase_in_display(int mode, bool selective = false); // 0=below, 1=above, 2=all, 3=scrollback
    void erase_in_line(int mode, bool selective = false);    // 0=right, 1=left, 2=all
    void erase_chars(int count);                             // ECH
    void insert_chars(int count);                            // ICH
    void delete_chars(int count);                            // DCH
    void insert_lines(int count);                            // IL
    void delete_lines(int count);                            // DL
    void insert_columns(int count);                          // DECIC
    void delete_columns(int count);                          // DECDC
    void fill_screen_e();                                    // DECALN

    // Alternate screen
    void enter_alt_screen(bool save_cursor_state, bool clear_alt);
    void leave_alt_screen(bool restore_cursor_state);

    // Resize with full logical reflow (implemented in reflow.cpp)
    void resize(int new_cols, int new_rows);

    // Full / soft reset
    void hard_reset();
    void soft_reset();

    // Viewport scrolling & snapshots
    void set_scroll_offset(int offset) noexcept;
    [[nodiscard]] int scroll_offset() const noexcept { return scroll_offset_; }
    void mark_all_dirty() noexcept;
    void mark_row_dirty(int r) noexcept;
    [[nodiscard]] bool has_dirty_rows() const noexcept;
    void clear_dirty_rows() noexcept;

    /// Captures visible rows (honoring scroll_offset) into `out` and resets dirty flags if `consume_dirty` is true.
    void build_snapshot(ViewportSnapshot& out, bool consume_dirty = true);

    /// Extracts plain UTF-8 text from a cell range (inclusive or exclusive end) for selection/copy.
    [[nodiscard]] std::string extract_text(int start_abs_row, int start_col,
                                           int end_abs_row, int end_col,
                                           bool rectangular = false) const;

    /// Converts a cell's codepoint (including grapheme cluster) to UTF-8.
    void append_cell_utf8(const Cell& c, std::string& out) const;

    /// Last written printable codepoint (for CSI b REP).
    [[nodiscard]] char32_t last_printed_cp() const noexcept { return last_printed_cp_; }

    /// Optional hook invoked whenever top=0 full-width lines scroll up (keeps inline images aligned).
    void set_on_scroll_up(std::function<void(int rows, bool alt_screen)> fn) {
        on_scroll_up_ = std::move(fn);
    }

private:
    [[nodiscard]] size_t main_phys(int r) const noexcept {
        int idx = main_head_ + r;
        if (idx >= rows_) idx -= rows_;
        return static_cast<size_t>(idx);
    }
    GridRow& active_row(int r) noexcept {
        return modes_.alt_screen ? alt_rows_[static_cast<size_t>(r)] : main_rows_[main_phys(r)];
    }
    const GridRow& active_row(int r) const noexcept {
        return modes_.alt_screen ? alt_rows_[static_cast<size_t>(r)] : main_rows_[main_phys(r)];
    }
    void normalize_main_ring() noexcept;
    std::vector<GridRow>& active_rows() noexcept { return modes_.alt_screen ? alt_rows_ : main_rows_; }
    const std::vector<GridRow>& active_rows() const noexcept { return modes_.alt_screen ? alt_rows_ : main_rows_; }

    Cell make_blank_cell() const noexcept;
    void clear_wide_conflict(GridRow& row, int col) noexcept;
    void reflow_main_screen(int new_cols, int new_rows);

    int cols_{80};
    int rows_{24};
    int main_head_{0};
    int scroll_top_{0};
    int scroll_bottom_{23};
    int scroll_left_{0};
    int scroll_right_{79};

    std::vector<GridRow> main_rows_;
    std::vector<GridRow> alt_rows_;
    Scrollback scrollback_;

    CursorState cursor_{};
    SavedCursor saved_main_{};
    SavedCursor saved_alt_{};
    CursorState saved_main_for_alt_{};

    CellPen pen_{};
    TerminalModes modes_{};
    CharsetState charsets_{};
    std::vector<bool> tab_stops_;

    ExtCellTable ext_table_;
    GraphemeTable grapheme_table_;
    HyperlinkTable hyperlink_table_;

    int scroll_offset_{0};
    uint64_t frame_generation_{1};
    char32_t last_printed_cp_{0};
    std::function<void(int, bool)> on_scroll_up_;
};

} // namespace vater::vt

