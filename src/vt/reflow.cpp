/// @file reflow.cpp
/// @brief L3 Terminal Core — Logical Line Reflow on Terminal Resize preserving cursor position.

#include "vt/grid.hpp"

#include <algorithm>
#include <vector>

namespace vater::vt {

namespace {

struct LogicalLine {
    std::vector<Cell> cells;
    RowMeta meta{};
    bool has_cursor{false};
    size_t cursor_offset{0};
};

} // namespace

void Grid::resize(int new_cols, int new_rows) {
    new_cols = std::max(2, new_cols);
    new_rows = std::max(1, new_rows);
    if (new_cols == cols_ && new_rows == rows_) return;

    // Resize alt screen without reflow (fullscreen TUI apps like vim/htop redraw on SIGWINCH)
    {
        std::vector<GridRow> new_alt(static_cast<size_t>(new_rows));
        for (int r = 0; r < new_rows; ++r) {
            new_alt[static_cast<size_t>(r)].cells.assign(static_cast<size_t>(new_cols), Cell{});
            new_alt[static_cast<size_t>(r)].meta.cols = static_cast<uint16_t>(new_cols);
            if (r < rows_) {
                const int copy_c = std::min(cols_, new_cols);
                std::copy_n(alt_rows_[static_cast<size_t>(r)].cells.begin(),
                            static_cast<size_t>(copy_c),
                            new_alt[static_cast<size_t>(r)].cells.begin());
                if (new_cols < cols_ && new_alt[static_cast<size_t>(r)].cells[static_cast<size_t>(new_cols - 1)].is_wide()) {
                    new_alt[static_cast<size_t>(r)].cells[static_cast<size_t>(new_cols - 1)] = Cell{};
                }
            }
        }
        alt_rows_ = std::move(new_alt);
    }

    if (modes_.alt_screen) {
        // Still reflow main screen cleanly in the background, then clamp alt cursor
        const CursorState alt_cursor = cursor_;
        cursor_ = saved_main_for_alt_;
        reflow_main_screen(new_cols, new_rows);
        saved_main_for_alt_ = cursor_;

        cols_ = new_cols;
        rows_ = new_rows;
        cursor_.row = std::clamp(alt_cursor.row, 0, rows_ - 1);
        cursor_.col = std::clamp(alt_cursor.col, 0, cols_ - 1);
        cursor_.pending_wrap = false;
    } else {
        reflow_main_screen(new_cols, new_rows);
        cols_ = new_cols;
        rows_ = new_rows;
    }

    reset_margins();
    reset_tab_stops();
    scroll_offset_ = std::clamp(scroll_offset_, 0, static_cast<int>(scrollback_.size()));
    mark_all_dirty();
}

void Grid::reflow_main_screen(int new_cols, int new_rows) {
    normalize_main_ring();
    // Pull up to recent scrollback rows plus main_rows_ into logical paragraphs so
    // narrowing/widening seamlessly unwraps and re-wraps text while preserving cursor.
    constexpr size_t kReflowScrollbackWindow = 2048;

    std::vector<GridRow> combined;
    const size_t sb_pull = std::min(scrollback_.size(), kReflowScrollbackWindow);
    combined.reserve(sb_pull + static_cast<size_t>(rows_));

    if (sb_pull > 0) {
        std::vector<ScrollbackRow> popped(sb_pull);
        for (size_t i = sb_pull; i > 0; --i) {
            scrollback_.pop_back(popped[i - 1]);
        }
        for (auto& sr : popped) {
            GridRow gr;
            gr.cells = std::move(sr.cells);
            gr.meta = sr.meta;
            combined.push_back(std::move(gr));
        }
    }

    const int cursor_combined_row = static_cast<int>(combined.size()) + cursor_.row;
    const int cursor_combined_col = cursor_.col;

    // Trim trailing completely empty rows below cursor from main screen so resizing taller
    // pulls scrollback back into view cleanly.
    int last_content_row = cursor_.row;
    for (int r = rows_ - 1; r > cursor_.row; --r) {
        bool any = false;
        for (const auto& c : main_rows_[static_cast<size_t>(r)].cells) {
            if (!c.is_blank()) {
                any = true;
                break;
            }
        }
        if (any || main_rows_[static_cast<size_t>(r)].meta.wrapped) {
            last_content_row = r;
            break;
        }
    }

    for (int r = 0; r <= last_content_row; ++r) {
        combined.push_back(std::move(main_rows_[static_cast<size_t>(r)]));
    }

    // 1. Join soft-wrapped physical rows into logical lines
    std::vector<LogicalLine> logicals;
    logicals.reserve(combined.size());

    for (size_t i = 0; i < combined.size(); ++i) {
        const bool is_continuation = (i > 0 && combined[i - 1].meta.wrapped);
        if (!is_continuation) {
            logicals.emplace_back();
            logicals.back().meta = combined[i].meta;
            logicals.back().meta.wrapped = false;
        }
        auto& line = logicals.back();
        const size_t base_offset = line.cells.size();

        if (static_cast<int>(i) == cursor_combined_row) {
            line.has_cursor = true;
            line.cursor_offset = base_offset + static_cast<size_t>(std::max(0, cursor_combined_col));
        }

        // Trim trailing default blanks ONLY on the final physical row of a logical line
        size_t row_len = combined[i].cells.size();
        if (!combined[i].meta.wrapped) {
            while (row_len > 0 && combined[i].cells[row_len - 1].is_blank()) {
                --row_len;
            }
        }
        line.cells.insert(line.cells.end(),
                          combined[i].cells.begin(),
                          combined[i].cells.begin() + static_cast<ptrdiff_t>(row_len));
    }

    // 2. Re-wrap each logical line to `new_cols`
    std::vector<GridRow> wrapped_rows;
    wrapped_rows.reserve(combined.size());

    int new_cursor_row = 0;
    int new_cursor_col = 0;
    bool cursor_placed = false;

    for (const auto& ll : logicals) {
        if (ll.cells.empty()) {
            GridRow gr;
            gr.cells.assign(static_cast<size_t>(new_cols), Cell{});
            gr.meta = ll.meta;
            gr.meta.wrapped = false;
            gr.meta.cols = static_cast<uint16_t>(new_cols);
            if (ll.has_cursor) {
                new_cursor_row = static_cast<int>(wrapped_rows.size());
                new_cursor_col = std::min(static_cast<int>(ll.cursor_offset), new_cols - 1);
                cursor_placed = true;
            }
            gr.used_cols = static_cast<uint16_t>(new_cols);
            gr.has_wide = true;
            wrapped_rows.push_back(std::move(gr));
            continue;
        }

        size_t pos = 0;
        bool first_chunk = true;
        while (pos < ll.cells.size()) {
            GridRow gr;
            gr.cells.assign(static_cast<size_t>(new_cols), Cell{});
            gr.meta = first_chunk ? ll.meta : RowMeta{};
            gr.meta.wrapped = false;
            gr.meta.cols = static_cast<uint16_t>(new_cols);
            first_chunk = false;

            int col = 0;
            const size_t chunk_start = pos;
            while (pos < ll.cells.size() && col < new_cols) {
                const Cell& c = ll.cells[pos];
                if (c.is_wide_spacer()) {
                    ++pos;
                    continue;
                }
                const int w = c.is_wide() ? 2 : 1;
                if (w == 2 && col + 1 >= new_cols) {
                    // Wide char does not fit at end of line: push to next wrapped line
                    break;
                }
                if (ll.has_cursor && !cursor_placed && ll.cursor_offset == pos) {
                    new_cursor_row = static_cast<int>(wrapped_rows.size());
                    new_cursor_col = col;
                    cursor_placed = true;
                }
                gr.cells[static_cast<size_t>(col)] = c;
                if (w == 2) {
                    Cell spacer = c;
                    spacer.cp = U' ';
                    spacer.attrs = static_cast<uint16_t>((c.attrs & ~cell_attr::kWide) | cell_attr::kWideSpacer);
                    gr.cells[static_cast<size_t>(col + 1)] = spacer;
                }
                col += w;
                ++pos;
                if (w == 2 && pos < ll.cells.size() && ll.cells[pos].is_wide_spacer()) {
                    ++pos;
                }
            }

            // Guard against zero-progress loop if new_cols == 1 and wide char
            if (pos == chunk_start) ++pos;

            if (pos < ll.cells.size()) {
                gr.meta.wrapped = true;
            } else if (ll.has_cursor && !cursor_placed) {
                // Cursor was at or past end of content on this logical line
                const size_t extra = (ll.cursor_offset > pos) ? (ll.cursor_offset - pos) : 0;
                const int target_c = col + static_cast<int>(extra);
                if (target_c < new_cols) {
                    new_cursor_row = static_cast<int>(wrapped_rows.size());
                    new_cursor_col = target_c;
                    cursor_placed = true;
                } else {
                    new_cursor_row = static_cast<int>(wrapped_rows.size());
                    new_cursor_col = new_cols - 1;
                    cursor_placed = true;
                }
            }

            gr.used_cols = static_cast<uint16_t>(new_cols);
            gr.has_wide = true;
            wrapped_rows.push_back(std::move(gr));
        }
    }

    // 3. Split back into scrollback (top overflow) + main_rows_ (exact `new_rows`)
    const int total = static_cast<int>(wrapped_rows.size());
    const int overflow = std::max(0, total - new_rows);

    for (int i = 0; i < overflow; ++i) {
        scrollback_.push(wrapped_rows[static_cast<size_t>(i)].cells,
                         wrapped_rows[static_cast<size_t>(i)].meta);
    }

    main_rows_.clear();
    main_rows_.reserve(static_cast<size_t>(new_rows));
    for (int i = overflow; i < total; ++i) {
        main_rows_.push_back(std::move(wrapped_rows[static_cast<size_t>(i)]));
    }
    while (static_cast<int>(main_rows_.size()) < new_rows) {
        GridRow empty;
        empty.cells.assign(static_cast<size_t>(new_cols), Cell{});
        empty.meta.cols = static_cast<uint16_t>(new_cols);
        main_rows_.push_back(std::move(empty));
    }

    if (cursor_placed) {
        cursor_.row = std::clamp(new_cursor_row - overflow, 0, new_rows - 1);
        cursor_.col = std::clamp(new_cursor_col, 0, new_cols - 1);
    } else {
        cursor_.row = std::clamp(cursor_.row, 0, new_rows - 1);
        cursor_.col = std::clamp(cursor_.col, 0, new_cols - 1);
    }
    cursor_.pending_wrap = false;
}

} // namespace vater::vt

