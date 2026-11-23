/// @file grid.cpp
/// @brief L3 Terminal Core — Grid operations: text insertion, margins (DECSTBM/DECSLRM),
/// scrolling, erasing, alt screen, tabs, dirty tracking, and viewport snapshots.

#include "vt/grid.hpp"
#include "vt/utf8_decoder.hpp"
#include "vt/wcwidth.hpp"

#include <algorithm>
#include <cstring>

namespace vater::vt {

Grid::Grid(int cols, int rows, size_t max_scrollback)
    : cols_(std::max(1, cols)),
      rows_(std::max(1, rows)),
      scroll_top_(0),
      scroll_bottom_(std::max(0, rows - 1)),
      scroll_left_(0),
      scroll_right_(std::max(0, cols - 1)),
      scrollback_(max_scrollback) {
    main_rows_.resize(static_cast<size_t>(rows_));
    alt_rows_.resize(static_cast<size_t>(rows_));
    for (int r = 0; r < rows_; ++r) {
        main_rows_[static_cast<size_t>(r)].cells.assign(static_cast<size_t>(cols_), Cell{});
        main_rows_[static_cast<size_t>(r)].meta.cols = static_cast<uint16_t>(cols_);
        alt_rows_[static_cast<size_t>(r)].cells.assign(static_cast<size_t>(cols_), Cell{});
        alt_rows_[static_cast<size_t>(r)].meta.cols = static_cast<uint16_t>(cols_);
    }
    reset_tab_stops();
}

Cell Grid::make_blank_cell() const noexcept {
    Cell c{};
    c.cp = U' ';
    c.fg = color_default();
    c.bg = pen_.bg; // BCE (Background Color Erase)
    c.attrs = static_cast<uint16_t>(pen_.attrs & cell_attr::kProtected);
    c.ext = 0;
    return c;
}

void Grid::refresh_pen_ext() {
    if (pen_.ext.is_empty()) {
        pen_.cached_ext_id = 0;
        pen_.attrs &= static_cast<uint16_t>(~cell_attr::kHasExt);
    } else {
        pen_.cached_ext_id = ext_table_.intern(pen_.ext);
        pen_.attrs |= cell_attr::kHasExt;
    }
}

void Grid::reset_tab_stops() {
    tab_stops_.assign(static_cast<size_t>(cols_), false);
    for (int c = 8; c < cols_; c += 8) {
        tab_stops_[static_cast<size_t>(c)] = true;
    }
}

void Grid::reset_margins() noexcept {
    scroll_top_ = 0;
    scroll_bottom_ = rows_ - 1;
    scroll_left_ = 0;
    scroll_right_ = cols_ - 1;
}

void Grid::set_scroll_region(int top, int bottom) {
    top = std::clamp(top, 0, rows_ - 1);
    bottom = std::clamp(bottom, 0, rows_ - 1);
    if (top < bottom) {
        scroll_top_ = top;
        scroll_bottom_ = bottom;
    } else {
        scroll_top_ = 0;
        scroll_bottom_ = rows_ - 1;
    }
    move_cursor_abs(0, 0);
}

void Grid::set_left_right_margins(int left, int right) {
    if (!modes_.allow_left_right_margin) return;
    left = std::clamp(left, 0, cols_ - 1);
    right = std::clamp(right, 0, cols_ - 1);
    if (left < right) {
        scroll_left_ = left;
        scroll_right_ = right;
    } else {
        scroll_left_ = 0;
        scroll_right_ = cols_ - 1;
    }
    move_cursor_abs(0, 0);
}

void Grid::normalize_main_ring() noexcept {
    if (main_head_ != 0 && !main_rows_.empty()) {
        std::rotate(main_rows_.begin(), main_rows_.begin() + main_head_, main_rows_.end());
        main_head_ = 0;
    }
}

void Grid::mark_row_dirty(int r) noexcept {
    if (r >= 0 && r < rows_) {
        auto& lr = active_row(r);
        lr.dirty = true;
        lr.generation = ++frame_generation_;
    }
}

void Grid::mark_all_dirty() noexcept {
    ++frame_generation_;
    for (auto& r : active_rows()) {
        r.dirty = true;
        r.generation = frame_generation_;
    }
}

bool Grid::has_dirty_rows() const noexcept {
    for (const auto& r : active_rows()) {
        if (r.dirty) return true;
    }
    return false;
}

void Grid::clear_dirty_rows() noexcept {
    for (auto& r : active_rows()) {
        r.dirty = false;
    }
}

void Grid::clear_wide_conflict(GridRow& r, int col) noexcept {
    if (col < 0 || col >= cols_) return;
    Cell& target = r.cells[static_cast<size_t>(col)];
    if (target.is_wide() && col + 1 < cols_) {
        r.cells[static_cast<size_t>(col + 1)] = make_blank_cell();
    } else if (target.is_wide_spacer() && col > 0) {
        r.cells[static_cast<size_t>(col - 1)] = make_blank_cell();
    }
}

void Grid::put_char(char32_t raw_cp) {
    char32_t cp = charsets_.translate(raw_cp);
    const int width = wcwidth(cp);
    if (width < 0) return;

    const int left_m = scroll_left();
    const int right_m = scroll_right();

    // Combining marks / zero-width grapheme extenders attach to previous cell
    if (width == 0) {
        if (cp == 0) return;
        int target_row = cursor_.row;
        int target_col = cursor_.pending_wrap ? cursor_.col : (cursor_.col - 1);
        if (target_col < left_m) return;
        auto& r = row(target_row);
        if (r.cells[static_cast<size_t>(target_col)].is_wide_spacer() && target_col > left_m) {
            --target_col;
        }
        Cell& prev = r.cells[static_cast<size_t>(target_col)];
        if (modes_.grapheme_clustering || is_combining_or_extender(cp)) {
            prev.cp = grapheme_table_.append(prev.cp, cp);
            prev.attrs |= cell_attr::kIsGrapheme;
            mark_row_dirty(target_row);
        }
        return;
    }

    last_printed_cp_ = cp;

    // Handle deferred wrap (xenl)
    if (cursor_.pending_wrap) {
        if (modes_.auto_wrap) {
            row(cursor_.row).meta.wrapped = true;
            cursor_.col = left_m;
            cursor_.pending_wrap = false;
            if (cursor_.row == scroll_bottom_) {
                scroll_up(1, scroll_top_, scroll_bottom_);
            } else if (cursor_.row + 1 < rows_) {
                ++cursor_.row;
            }
        } else {
            cursor_.pending_wrap = false;
            cursor_.col = right_m;
        }
    }

    // Wide char at right margin needs wrap if auto_wrap enabled
    if (width == 2 && cursor_.col == right_m) {
        if (modes_.auto_wrap && (right_m - left_m) >= 1) {
            auto& cur_row = row(cursor_.row);
            clear_wide_conflict(cur_row, cursor_.col);
            cur_row.cells[static_cast<size_t>(cursor_.col)] = make_blank_cell();
            cur_row.meta.wrapped = true;
            mark_row_dirty(cursor_.row);

            cursor_.col = left_m;
            if (cursor_.row == scroll_bottom_) {
                scroll_up(1, scroll_top_, scroll_bottom_);
            } else if (cursor_.row + 1 < rows_) {
                ++cursor_.row;
            }
        } else {
            // Cannot fit 2-col glyph in 1-col region
            return;
        }
    }

    // Insert Mode (IRM): shift existing characters right before writing
    if (modes_.insert_mode) {
        insert_chars(width);
    }

    auto& r = row(cursor_.row);
    clear_wide_conflict(r, cursor_.col);
    if (width == 2 && cursor_.col + 1 <= right_m) {
        clear_wide_conflict(r, cursor_.col + 1);
    }

    Cell cell_val{};
    cell_val.cp = cp;
    cell_val.fg = pen_.fg;
    cell_val.bg = pen_.bg;
    cell_val.attrs = static_cast<uint16_t>(pen_.attrs & ~(cell_attr::kWide | cell_attr::kWideSpacer | cell_attr::kIsGrapheme));
    cell_val.ext = pen_.cached_ext_id;

    if (width == 2) {
        cell_val.attrs |= cell_attr::kWide;
        r.cells[static_cast<size_t>(cursor_.col)] = cell_val;
        r.has_wide = true;

        if (cursor_.col + 1 <= right_m) {
            Cell spacer = cell_val;
            spacer.cp = U' ';
            spacer.attrs = static_cast<uint16_t>((cell_val.attrs & ~cell_attr::kWide) | cell_attr::kWideSpacer);
            r.cells[static_cast<size_t>(cursor_.col + 1)] = spacer;
        }
    } else {
        r.cells[static_cast<size_t>(cursor_.col)] = cell_val;
    }
    r.used_cols = std::max(r.used_cols, static_cast<uint16_t>(std::min(cols_, cursor_.col + width)));

    mark_row_dirty(cursor_.row);

    // Advance cursor
    if (cursor_.col + width > right_m) {
        cursor_.col = right_m;
        if (modes_.auto_wrap) {
            cursor_.pending_wrap = true;
        }
    } else {
        cursor_.col += width;
    }
}

void Grid::put_ascii_run(const uint8_t* data, size_t len) {
    if (len == 0) return;

    // Fallback to per-char if insert mode or non-ASCII charset active
    if (modes_.insert_mode || !charsets_.is_plain_ascii()) {
        for (size_t i = 0; i < len; ++i) {
            put_char(static_cast<char32_t>(data[i]));
        }
        return;
    }

    const int left_m = scroll_left();
    const int right_m = scroll_right();

    Cell tmpl{};
    tmpl.fg = pen_.fg;
    tmpl.bg = pen_.bg;
    tmpl.attrs = static_cast<uint16_t>(pen_.attrs & ~(cell_attr::kWide | cell_attr::kWideSpacer | cell_attr::kIsGrapheme));
    tmpl.ext = pen_.cached_ext_id;

    size_t pos = 0;
    while (pos < len) {
        if (cursor_.pending_wrap) {
            if (modes_.auto_wrap) {
                row(cursor_.row).meta.wrapped = true;
                cursor_.col = left_m;
                cursor_.pending_wrap = false;
                if (cursor_.row == scroll_bottom_) {
                    scroll_up(1, scroll_top_, scroll_bottom_);
                } else if (cursor_.row + 1 < rows_) {
                    ++cursor_.row;
                }
            } else {
                cursor_.pending_wrap = false;
                cursor_.col = right_m;
            }
        }

        auto& r = row(cursor_.row);
        const int avail = right_m - cursor_.col + 1;
        const size_t chunk = std::min(len - pos, static_cast<size_t>(std::max(1, avail)));

        // Resolve wide conflicts only when row actually contains wide chars
        if (r.has_wide) [[unlikely]] {
            clear_wide_conflict(r, cursor_.col);
            if (chunk > 1) {
                clear_wide_conflict(r, cursor_.col + static_cast<int>(chunk) - 1);
            }
        }

        Cell* dst = r.cells.data() + cursor_.col;
#if defined(__SSE2__)
        tmpl.cp = 0;
        const __m128i base_cell = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&tmpl));
        for (size_t k = 0; k < chunk; ++k) {
            __m128i cvec = _mm_or_si128(base_cell, _mm_cvtsi32_si128(static_cast<int>(data[pos + k])));
            _mm_store_si128(reinterpret_cast<__m128i*>(dst + k), cvec);
        }
#else
        for (size_t k = 0; k < chunk; ++k) {
            dst[k] = tmpl;
            dst[k].cp = static_cast<char32_t>(data[pos + k]);
        }
#endif
        r.used_cols = std::max(r.used_cols, static_cast<uint16_t>(cursor_.col + static_cast<int>(chunk)));
        last_printed_cp_ = static_cast<char32_t>(data[pos + chunk - 1]);
        mark_row_dirty(cursor_.row);

        pos += chunk;
        if (cursor_.col + static_cast<int>(chunk) > right_m) {
            cursor_.col = right_m;
            if (modes_.auto_wrap) {
                cursor_.pending_wrap = true;
            }
        } else {
            cursor_.col += static_cast<int>(chunk);
        }
    }
}

void Grid::carriage_return() noexcept {
    cursor_.pending_wrap = false;
    const int left_m = scroll_left();
    cursor_.col = (cursor_.col >= left_m) ? left_m : 0;
}

void Grid::line_feed() {
    cursor_.pending_wrap = false;
    if (cursor_.row == scroll_bottom_) {
        scroll_up(1, scroll_top_, scroll_bottom_);
    } else if (cursor_.row + 1 < rows_) {
        ++cursor_.row;
    }
    if (modes_.line_feed_new_line) {
        carriage_return();
    }
}

void Grid::reverse_index() {
    cursor_.pending_wrap = false;
    if (cursor_.row == scroll_top_) {
        scroll_down(1, scroll_top_, scroll_bottom_);
    } else if (cursor_.row > 0) {
        --cursor_.row;
    }
}

void Grid::backspace() noexcept {
    if (cursor_.pending_wrap) {
        cursor_.pending_wrap = false;
        return;
    }
    const int left_m = scroll_left();
    if (cursor_.col > left_m) {
        --cursor_.col;
    } else if (modes_.reverse_wraparound && cursor_.col == left_m && cursor_.row > scroll_top_) {
        --cursor_.row;
        cursor_.col = scroll_right();
    } else if (cursor_.col > 0 && cursor_.col < left_m) {
        --cursor_.col;
    }
}

void Grid::horizontal_tab(int count) noexcept {
    cursor_.pending_wrap = false;
    const int right_m = scroll_right();
    for (int n = 0; n < count; ++n) {
        int next = cursor_.col + 1;
        while (next < right_m && !tab_stops_[static_cast<size_t>(next)]) {
            ++next;
        }
        cursor_.col = std::min(next, right_m);
    }
}

void Grid::backward_tab(int count) noexcept {
    cursor_.pending_wrap = false;
    const int left_m = scroll_left();
    for (int n = 0; n < count; ++n) {
        int prev = cursor_.col - 1;
        while (prev > left_m && !tab_stops_[static_cast<size_t>(prev)]) {
            --prev;
        }
        cursor_.col = std::max(prev, left_m);
    }
}

void Grid::set_tab_stop() noexcept {
    if (cursor_.col >= 0 && cursor_.col < cols_) {
        tab_stops_[static_cast<size_t>(cursor_.col)] = true;
    }
}

void Grid::clear_tab_stop(int mode) noexcept {
    if (mode == 0) {
        if (cursor_.col >= 0 && cursor_.col < cols_) {
            tab_stops_[static_cast<size_t>(cursor_.col)] = false;
        }
    } else if (mode == 3) {
        std::fill(tab_stops_.begin(), tab_stops_.end(), false);
    }
}

void Grid::move_cursor_abs(int r, int c) noexcept {
    cursor_.pending_wrap = false;
    if (modes_.origin_mode) {
        const int top_m = scroll_top_;
        const int bot_m = scroll_bottom_;
        const int left_m = scroll_left();
        const int right_m = scroll_right();
        cursor_.row = std::clamp(top_m + r, top_m, bot_m);
        cursor_.col = std::clamp(left_m + c, left_m, right_m);
    } else {
        cursor_.row = std::clamp(r, 0, rows_ - 1);
        cursor_.col = std::clamp(c, 0, cols_ - 1);
    }
}

void Grid::move_cursor_rel(int dr, int dc) noexcept {
    cursor_.pending_wrap = false;
    if (dr != 0) {
        int min_r = (cursor_.row >= scroll_top_ && cursor_.row <= scroll_bottom_) ? scroll_top_ : 0;
        int max_r = (cursor_.row >= scroll_top_ && cursor_.row <= scroll_bottom_) ? scroll_bottom_ : (rows_ - 1);
        if (dr < 0) {
            cursor_.row = std::max(min_r, cursor_.row + dr);
        } else {
            cursor_.row = std::min(max_r, cursor_.row + dr);
        }
    }
    if (dc != 0) {
        const int left_m = scroll_left();
        const int right_m = scroll_right();
        int min_c = (cursor_.col >= left_m && cursor_.col <= right_m) ? left_m : 0;
        int max_c = (cursor_.col >= left_m && cursor_.col <= right_m) ? right_m : (cols_ - 1);
        if (dc < 0) {
            cursor_.col = std::max(min_c, cursor_.col + dc);
        } else {
            cursor_.col = std::min(max_c, cursor_.col + dc);
        }
    }
}

void Grid::set_cursor_col(int col) noexcept {
    cursor_.pending_wrap = false;
    if (modes_.origin_mode) {
        cursor_.col = std::clamp(scroll_left() + col, scroll_left(), scroll_right());
    } else {
        cursor_.col = std::clamp(col, 0, cols_ - 1);
    }
}

void Grid::set_cursor_row(int r) noexcept {
    cursor_.pending_wrap = false;
    if (modes_.origin_mode) {
        cursor_.row = std::clamp(scroll_top_ + r, scroll_top_, scroll_bottom_);
    } else {
        cursor_.row = std::clamp(r, 0, rows_ - 1);
    }
}

void Grid::save_cursor() noexcept {
    auto& dst = modes_.alt_screen ? saved_alt_ : saved_main_;
    dst.cursor = cursor_;
    dst.pen = pen_;
    dst.charsets = charsets_;
    dst.origin_mode = modes_.origin_mode;
    dst.auto_wrap = modes_.auto_wrap;
    dst.valid = true;
}

void Grid::restore_cursor() noexcept {
    const auto& src = modes_.alt_screen ? saved_alt_ : saved_main_;
    if (!src.valid) {
        cursor_ = CursorState{};
        pen_.reset();
        charsets_.reset();
        modes_.origin_mode = false;
        modes_.auto_wrap = true;
        return;
    }
    cursor_.row = std::clamp(src.cursor.row, 0, rows_ - 1);
    cursor_.col = std::clamp(src.cursor.col, 0, cols_ - 1);
    cursor_.pending_wrap = src.cursor.pending_wrap && (cursor_.col == cols_ - 1);
    pen_ = src.pen;
    charsets_ = src.charsets;
    modes_.origin_mode = src.origin_mode;
    modes_.auto_wrap = src.auto_wrap;
}

void Grid::scroll_up(int count, int top, int bottom) {
    if (count <= 0 || top > bottom || top < 0 || bottom >= rows_) return;
    const int region_h = bottom - top + 1;
    count = std::min(count, region_h);

    auto& rows = active_rows();
    const int left_m = scroll_left();
    const int right_m = scroll_right();
    const bool full_width = (left_m == 0 && right_m == cols_ - 1);
    const Cell blank = make_blank_cell();

    if (full_width) {
        // O(1) ring-buffer fast path for full-screen primary scroll by 1 line (zero row shifts!)
        if (!modes_.alt_screen && count == 1 && top == 0 && bottom == rows_ - 1) [[likely]] {
            GridRow& top_row = main_rows_[static_cast<size_t>(main_head_)];
            const size_t span_len = (top_row.used_cols > 0 && top_row.used_cols <= top_row.cells.size())
                                        ? static_cast<size_t>(top_row.used_cols)
                                        : top_row.cells.size();
            scrollback_.push(std::span<const Cell>(top_row.cells.data(), span_len), top_row.meta);
            if (scroll_offset_ > 0) {
                scroll_offset_ = std::min(scroll_offset_ + 1, static_cast<int>(scrollback_.size()));
            }

            const bool default_blank = blank.is_blank();
            const size_t clear_n = (default_blank && top_row.used_cols <= top_row.cells.size())
                                       ? static_cast<size_t>(top_row.used_cols)
                                       : top_row.cells.size();
#if defined(__AVX2__)
            const __m256i bvec = _mm256_broadcastsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(&blank)));
            size_t ci = 0;
            Cell* cptr = top_row.cells.data();
            for (; ci + 2 <= clear_n; ci += 2) {
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(cptr + ci), bvec);
            }
            if (ci < clear_n) cptr[ci] = blank;
#else
            std::fill_n(top_row.cells.begin(), clear_n, blank);
#endif
            top_row.used_cols = default_blank ? 0 : static_cast<uint16_t>(cols_);
            top_row.has_wide = false;
            top_row.meta = RowMeta{};
            top_row.meta.cols = static_cast<uint16_t>(cols_);
            top_row.dirty = true;
            top_row.generation = ++frame_generation_;

            if (++main_head_ >= rows_) main_head_ = 0;
            if (on_scroll_up_) on_scroll_up_(1, false);
            return;
        }

        if (!modes_.alt_screen) normalize_main_ring();

        // Push lines scrolling off the top of primary full screen into scrollback
        if (!modes_.alt_screen && top == 0) {
            for (int i = 0; i < count; ++i) {
                const auto& rsrc = rows[static_cast<size_t>(top + i)];
                const size_t span_len = (rsrc.used_cols > 0 && rsrc.used_cols <= rsrc.cells.size())
                                            ? static_cast<size_t>(rsrc.used_cols)
                                            : rsrc.cells.size();
                scrollback_.push(std::span<const Cell>(rsrc.cells.data(), span_len), rsrc.meta);
            }
            if (scroll_offset_ > 0) {
                scroll_offset_ = std::min(scroll_offset_ + count, static_cast<int>(scrollback_.size()));
            }
        }
        if (top == 0 && on_scroll_up_) {
            on_scroll_up_(count, modes_.alt_screen);
        }

        std::rotate(rows.begin() + top, rows.begin() + top + count, rows.begin() + bottom + 1);
        for (int r = bottom - count + 1; r <= bottom; ++r) {
            auto& line = rows[static_cast<size_t>(r)];
            std::fill(line.cells.begin(), line.cells.end(), blank);
            line.used_cols = blank.is_blank() ? 0 : static_cast<uint16_t>(cols_);
            line.has_wide = false;
            line.meta = RowMeta{};
            line.meta.cols = static_cast<uint16_t>(cols_);
        }
    } else {
        if (!modes_.alt_screen) normalize_main_ring();
        // Partial-width scroll within DECSLRM margins
        for (int r = top; r <= bottom - count; ++r) {
            for (int c = left_m; c <= right_m; ++c) {
                rows[static_cast<size_t>(r)].cells[static_cast<size_t>(c)] =
                    rows[static_cast<size_t>(r + count)].cells[static_cast<size_t>(c)];
            }
            rows[static_cast<size_t>(r)].meta.wrapped = false;
        }
        for (int r = bottom - count + 1; r <= bottom; ++r) {
            for (int c = left_m; c <= right_m; ++c) {
                rows[static_cast<size_t>(r)].cells[static_cast<size_t>(c)] = blank;
            }
            rows[static_cast<size_t>(r)].meta.wrapped = false;
        }
    }

    for (int r = top; r <= bottom; ++r) mark_row_dirty(r);
}

void Grid::scroll_down(int count, int top, int bottom) {
    if (count <= 0 || top > bottom || top < 0 || bottom >= rows_) return;
    if (!modes_.alt_screen) normalize_main_ring();
    const int region_h = bottom - top + 1;
    count = std::min(count, region_h);

    auto& rows = active_rows();
    const int left_m = scroll_left();
    const int right_m = scroll_right();
    const bool full_width = (left_m == 0 && right_m == cols_ - 1);
    const Cell blank = make_blank_cell();

    if (full_width) {
        std::rotate(rows.begin() + top, rows.begin() + bottom + 1 - count, rows.begin() + bottom + 1);
        for (int r = top; r < top + count; ++r) {
            auto& line = rows[static_cast<size_t>(r)];
            std::fill(line.cells.begin(), line.cells.end(), blank);
            line.meta = RowMeta{};
            line.meta.cols = static_cast<uint16_t>(cols_);
        }
    } else {
        for (int r = bottom; r >= top + count; --r) {
            for (int c = left_m; c <= right_m; ++c) {
                rows[static_cast<size_t>(r)].cells[static_cast<size_t>(c)] =
                    rows[static_cast<size_t>(r - count)].cells[static_cast<size_t>(c)];
            }
            rows[static_cast<size_t>(r)].meta.wrapped = false;
        }
        for (int r = top; r < top + count; ++r) {
            for (int c = left_m; c <= right_m; ++c) {
                rows[static_cast<size_t>(r)].cells[static_cast<size_t>(c)] = blank;
            }
            rows[static_cast<size_t>(r)].meta.wrapped = false;
        }
    }

    for (int r = top; r <= bottom; ++r) mark_row_dirty(r);
}

void Grid::erase_in_display(int mode, bool selective) {
    cursor_.pending_wrap = false;
    if (!modes_.alt_screen) normalize_main_ring();
    auto& rows = active_rows();
    const Cell blank = make_blank_cell();

    auto erase_cell = [&](Cell& c) {
        if (!selective || (c.attrs & cell_attr::kProtected) == 0) {
            c = blank;
        }
    };

    if (mode == 0) {
        // Cursor to end of display
        erase_in_line(0, selective);
        for (int r = cursor_.row + 1; r < rows_; ++r) {
            for (auto& c : rows[static_cast<size_t>(r)].cells) erase_cell(c);
            rows[static_cast<size_t>(r)].meta.wrapped = false;
            mark_row_dirty(r);
        }
    } else if (mode == 1) {
        // Start of display to cursor
        for (int r = 0; r < cursor_.row; ++r) {
            for (auto& c : rows[static_cast<size_t>(r)].cells) erase_cell(c);
            rows[static_cast<size_t>(r)].meta.wrapped = false;
            mark_row_dirty(r);
        }
        erase_in_line(1, selective);
    } else if (mode == 2) {
        // Entire display
        for (int r = 0; r < rows_; ++r) {
            for (auto& c : rows[static_cast<size_t>(r)].cells) erase_cell(c);
            rows[static_cast<size_t>(r)].meta.wrapped = false;
            mark_row_dirty(r);
        }
    } else if (mode == 3) {
        // Erase scrollback buffer (xterm 3J)
        scrollback_.clear();
        scroll_offset_ = 0;
    }
}

void Grid::erase_in_line(int mode, bool selective) {
    cursor_.pending_wrap = false;
    auto& r = row(cursor_.row);
    const Cell blank = make_blank_cell();

    auto erase_range = [&](int from, int to) {
        from = std::clamp(from, 0, cols_ - 1);
        to = std::clamp(to, 0, cols_ - 1);
        if (from > to) return;
        clear_wide_conflict(r, from);
        clear_wide_conflict(r, to);
        for (int c = from; c <= to; ++c) {
            Cell& cell_ref = r.cells[static_cast<size_t>(c)];
            if (!selective || (cell_ref.attrs & cell_attr::kProtected) == 0) {
                cell_ref = blank;
            }
        }
    };

    if (mode == 0) {
        erase_range(cursor_.col, cols_ - 1);
        r.meta.wrapped = false;
    } else if (mode == 1) {
        erase_range(0, cursor_.col);
    } else if (mode == 2) {
        erase_range(0, cols_ - 1);
        r.meta.wrapped = false;
    }
    mark_row_dirty(cursor_.row);
}

void Grid::erase_chars(int count) {
    if (count <= 0) return;
    cursor_.pending_wrap = false;
    auto& r = row(cursor_.row);
    const int end = std::min(cols_ - 1, cursor_.col + count - 1);
    const Cell blank = make_blank_cell();
    clear_wide_conflict(r, cursor_.col);
    clear_wide_conflict(r, end);
    for (int c = cursor_.col; c <= end; ++c) {
        r.cells[static_cast<size_t>(c)] = blank;
    }
    mark_row_dirty(cursor_.row);
}

void Grid::insert_chars(int count) {
    if (count <= 0) return;
    cursor_.pending_wrap = false;
    const int left_m = scroll_left();
    const int right_m = scroll_right();
    if (cursor_.col < left_m || cursor_.col > right_m) return;

    auto& r = row(cursor_.row);
    const int avail = right_m - cursor_.col + 1;
    count = std::min(count, avail);
    const Cell blank = make_blank_cell();

    clear_wide_conflict(r, cursor_.col);
    clear_wide_conflict(r, right_m);

    for (int c = right_m; c >= cursor_.col + count; --c) {
        r.cells[static_cast<size_t>(c)] = r.cells[static_cast<size_t>(c - count)];
    }
    for (int c = cursor_.col; c < cursor_.col + count; ++c) {
        r.cells[static_cast<size_t>(c)] = blank;
    }
    // Fix any orphaned wide cell pushed off right margin
    if (r.cells[static_cast<size_t>(right_m)].is_wide()) {
        r.cells[static_cast<size_t>(right_m)] = blank;
    }
    r.meta.wrapped = false;
    r.used_cols = static_cast<uint16_t>(cols_);
    r.has_wide = true;
    mark_row_dirty(cursor_.row);
}

void Grid::delete_chars(int count) {
    if (count <= 0) return;
    cursor_.pending_wrap = false;
    const int left_m = scroll_left();
    const int right_m = scroll_right();
    if (cursor_.col < left_m || cursor_.col > right_m) return;

    auto& r = row(cursor_.row);
    const int avail = right_m - cursor_.col + 1;
    count = std::min(count, avail);
    const Cell blank = make_blank_cell();

    clear_wide_conflict(r, cursor_.col);
    if (cursor_.col + count <= right_m) {
        clear_wide_conflict(r, cursor_.col + count);
    }

    for (int c = cursor_.col; c <= right_m - count; ++c) {
        r.cells[static_cast<size_t>(c)] = r.cells[static_cast<size_t>(c + count)];
    }
    for (int c = right_m - count + 1; c <= right_m; ++c) {
        r.cells[static_cast<size_t>(c)] = blank;
    }
    r.meta.wrapped = false;
    mark_row_dirty(cursor_.row);
}

void Grid::insert_lines(int count) {
    if (count <= 0) return;
    cursor_.pending_wrap = false;
    if (cursor_.row < scroll_top_ || cursor_.row > scroll_bottom_) return;
    cursor_.col = scroll_left();
    scroll_down(count, cursor_.row, scroll_bottom_);
}

void Grid::delete_lines(int count) {
    if (count <= 0) return;
    cursor_.pending_wrap = false;
    if (cursor_.row < scroll_top_ || cursor_.row > scroll_bottom_) return;
    cursor_.col = scroll_left();
    scroll_up(count, cursor_.row, scroll_bottom_);
}

void Grid::insert_columns(int count) {
    if (count <= 0) return;
    cursor_.pending_wrap = false;
    if (!modes_.alt_screen) normalize_main_ring();
    const int left_m = scroll_left();
    const int right_m = scroll_right();
    if (cursor_.col < left_m || cursor_.col > right_m) return;
    if (cursor_.row < scroll_top_ || cursor_.row > scroll_bottom_) return;

    const int avail = right_m - cursor_.col + 1;
    count = std::min(count, avail);
    const Cell blank = make_blank_cell();
    auto& rows = active_rows();

    for (int r_idx = scroll_top_; r_idx <= scroll_bottom_; ++r_idx) {
        auto& r = rows[static_cast<size_t>(r_idx)];
        for (int c = right_m; c >= cursor_.col + count; --c) {
            r.cells[static_cast<size_t>(c)] = r.cells[static_cast<size_t>(c - count)];
        }
        for (int c = cursor_.col; c < cursor_.col + count; ++c) {
            r.cells[static_cast<size_t>(c)] = blank;
        }
        r.meta.wrapped = false;
        mark_row_dirty(r_idx);
    }
}

void Grid::delete_columns(int count) {
    if (count <= 0) return;
    cursor_.pending_wrap = false;
    if (!modes_.alt_screen) normalize_main_ring();
    const int left_m = scroll_left();
    const int right_m = scroll_right();
    if (cursor_.col < left_m || cursor_.col > right_m) return;
    if (cursor_.row < scroll_top_ || cursor_.row > scroll_bottom_) return;

    const int avail = right_m - cursor_.col + 1;
    count = std::min(count, avail);
    const Cell blank = make_blank_cell();
    auto& rows = active_rows();

    for (int r_idx = scroll_top_; r_idx <= scroll_bottom_; ++r_idx) {
        auto& r = rows[static_cast<size_t>(r_idx)];
        for (int c = cursor_.col; c <= right_m - count; ++c) {
            r.cells[static_cast<size_t>(c)] = r.cells[static_cast<size_t>(c + count)];
        }
        for (int c = right_m - count + 1; c <= right_m; ++c) {
            r.cells[static_cast<size_t>(c)] = blank;
        }
        r.meta.wrapped = false;
        mark_row_dirty(r_idx);
    }
}

void Grid::fill_screen_e() {
    reset_margins();
    modes_.origin_mode = false;
    cursor_ = CursorState{};
    Cell e{};
    e.cp = U'E';
    for (auto& r : active_rows()) {
        std::fill(r.cells.begin(), r.cells.end(), e);
        r.used_cols = static_cast<uint16_t>(cols_);
        r.meta.wrapped = false;
    }
    mark_all_dirty();
}

void Grid::enter_alt_screen(bool save_cursor_state, bool clear_alt) {
    if (save_cursor_state) {
        save_cursor();
    }
    if (!modes_.alt_screen) {
        saved_main_for_alt_ = cursor_;
        modes_.alt_screen = true;
        scroll_offset_ = 0;
    }
    if (clear_alt) {
        const Cell blank{};
        for (auto& r : alt_rows_) {
            std::fill(r.cells.begin(), r.cells.end(), blank);
            r.meta = RowMeta{};
            r.meta.cols = static_cast<uint16_t>(cols_);
        }
    }
    mark_all_dirty();
}

void Grid::leave_alt_screen(bool restore_cursor_state) {
    if (modes_.alt_screen) {
        modes_.alt_screen = false;
        if (restore_cursor_state) {
            restore_cursor();
        } else {
            cursor_.row = std::clamp(saved_main_for_alt_.row, 0, rows_ - 1);
            cursor_.col = std::clamp(saved_main_for_alt_.col, 0, cols_ - 1);
            cursor_.pending_wrap = false;
        }
    }
    mark_all_dirty();
}

void Grid::hard_reset() {
    main_head_ = 0;
    modes_.reset();
    pen_ = CellPen{};
    charsets_.reset();
    cursor_ = CursorState{};
    saved_main_ = SavedCursor{};
    saved_alt_ = SavedCursor{};
    reset_margins();
    reset_tab_stops();
    scroll_offset_ = 0;
    const Cell blank{};
    for (auto& r : main_rows_) {
        std::fill(r.cells.begin(), r.cells.end(), blank);
        r.meta = RowMeta{};
        r.meta.cols = static_cast<uint16_t>(cols_);
    }
    for (auto& r : alt_rows_) {
        std::fill(r.cells.begin(), r.cells.end(), blank);
        r.meta = RowMeta{};
        r.meta.cols = static_cast<uint16_t>(cols_);
    }
    mark_all_dirty();
}

void Grid::soft_reset() {
    modes_.cursor_visible = true;
    modes_.origin_mode = false;
    modes_.insert_mode = false;
    modes_.cursor_keys_app = false;
    modes_.app_keypad = false;
    modes_.allow_left_right_margin = false;
    pen_.reset();
    charsets_.reset();
    reset_margins();
    saved_main_ = SavedCursor{};
    saved_alt_ = SavedCursor{};
}

void Grid::set_scroll_offset(int offset) noexcept {
    if (modes_.alt_screen) {
        scroll_offset_ = 0;
        return;
    }
    const int max_off = static_cast<int>(scrollback_.size());
    const int clamped = std::clamp(offset, 0, max_off);
    if (clamped != scroll_offset_) {
        scroll_offset_ = clamped;
        mark_all_dirty();
    }
}

void Grid::build_snapshot(ViewportSnapshot& out, bool consume_dirty) {
    const bool size_changed = (out.cols != cols_ || out.rows != rows_ ||
                               static_cast<int>(out.lines.size()) != rows_);
    const bool offset_changed = (out.scroll_offset != scroll_offset_);

    out.cols = cols_;
    out.rows = rows_;
    out.scroll_offset = modes_.alt_screen ? 0 : scroll_offset_;
    out.scrollback_size = scrollback_.size();
    out.cursor = cursor_;
    out.cursor_visible = modes_.cursor_visible && (out.scroll_offset == 0);
    out.cursor_shape = modes_.cursor_shape;
    out.cursor_blink = modes_.cursor_blink;
    out.reverse_video = modes_.reverse_video;
    out.bidi_implicit = modes_.bidi_implicit;
    out.alt_screen = modes_.alt_screen;
    out.frame_generation = frame_generation_;

    if (size_changed) {
        out.lines.resize(static_cast<size_t>(rows_));
        out.dirty_rows.assign(static_cast<size_t>(rows_), true);
    } else {
        out.dirty_rows.assign(static_cast<size_t>(rows_), offset_changed);
    }

    const int sb_total = static_cast<int>(scrollback_.size());

    if (out.scroll_offset == 0 || modes_.alt_screen) {
        for (int r = 0; r < rows_; ++r) {
            auto& src = active_row(r);
            if (size_changed || offset_changed || src.dirty ||
                out.lines[static_cast<size_t>(r)].generation != src.generation) {
                out.lines[static_cast<size_t>(r)] = src;
                out.dirty_rows[static_cast<size_t>(r)] = true;
            }
            if (consume_dirty) src.dirty = false;
        }
    } else {
        ScrollbackRow sb_row;
        for (int r = 0; r < rows_; ++r) {
            const int abs_idx = sb_total - out.scroll_offset + r;
            auto& dst = out.lines[static_cast<size_t>(r)];
            if (abs_idx >= 0 && abs_idx < sb_total) {
                if (scrollback_.get(static_cast<size_t>(abs_idx), sb_row, static_cast<size_t>(cols_))) {
                    dst.cells = sb_row.cells;
                    if (static_cast<int>(dst.cells.size()) > cols_) {
                        dst.cells.resize(static_cast<size_t>(cols_));
                    }
                    dst.meta = sb_row.meta;
                    dst.dirty = true;
                    out.dirty_rows[static_cast<size_t>(r)] = true;
                }
            } else if (abs_idx >= sb_total && (abs_idx - sb_total) < rows_) {
                dst = active_row(abs_idx - sb_total);
                out.dirty_rows[static_cast<size_t>(r)] = true;
            }
        }
        if (consume_dirty) clear_dirty_rows();
    }
}

void Grid::append_cell_utf8(const Cell& c, std::string& out) const {
    if (c.is_wide_spacer()) return;
    char buf[4];
    if (c.is_grapheme()) {
        if (const auto* cluster = grapheme_table_.lookup(c.cp)) {
            for (char32_t cp : *cluster) {
                size_t n = encode_utf8(cp, buf);
                out.append(buf, n);
            }
            return;
        }
    }
    size_t n = encode_utf8(c.cp, buf);
    out.append(buf, n);
}

std::string Grid::extract_text(int start_abs_row, int start_col,
                               int end_abs_row, int end_col,
                               bool rectangular) const {
    if (start_abs_row > end_abs_row || (start_abs_row == end_abs_row && start_col > end_col)) {
        std::swap(start_abs_row, end_abs_row);
        std::swap(start_col, end_col);
    }

    const int sb_total = modes_.alt_screen ? 0 : static_cast<int>(scrollback_.size());
    const int total_rows = sb_total + rows_;

    start_abs_row = std::clamp(start_abs_row, 0, total_rows - 1);
    end_abs_row = std::clamp(end_abs_row, 0, total_rows - 1);

    std::string result;
    ScrollbackRow sb_tmp;

    for (int abs_r = start_abs_row; abs_r <= end_abs_row; ++abs_r) {
        const std::vector<Cell>* cells_ptr = nullptr;
        bool is_wrapped = false;
        if (!modes_.alt_screen && abs_r < sb_total) {
            if (scrollback_.get(static_cast<size_t>(abs_r), sb_tmp, static_cast<size_t>(cols_))) {
                cells_ptr = &sb_tmp.cells;
                is_wrapped = sb_tmp.meta.wrapped;
            }
        } else {
            const int live_r = abs_r - sb_total;
            if (live_r >= 0 && live_r < rows_) {
                cells_ptr = &active_row(live_r).cells;
                is_wrapped = active_row(live_r).meta.wrapped;
            }
        }
        if (!cells_ptr) continue;

        int c0 = rectangular ? std::min(start_col, end_col)
                             : (abs_r == start_abs_row ? start_col : 0);
        int c1 = rectangular ? std::max(start_col, end_col)
                             : (abs_r == end_abs_row ? end_col : (static_cast<int>(cells_ptr->size()) - 1));
        c0 = std::clamp(c0, 0, static_cast<int>(cells_ptr->size()) - 1);
        c1 = std::clamp(c1, 0, static_cast<int>(cells_ptr->size()) - 1);

        // Trim trailing blank spaces unless line is soft-wrapped or rectangular selection
        int last_non_blank = c1;
        if (!rectangular && !is_wrapped) {
            while (last_non_blank >= c0 && (*cells_ptr)[static_cast<size_t>(last_non_blank)].cp == U' ' &&
                   !(*cells_ptr)[static_cast<size_t>(last_non_blank)].is_wide_spacer()) {
                --last_non_blank;
            }
        }

        for (int c = c0; c <= last_non_blank; ++c) {
            append_cell_utf8((*cells_ptr)[static_cast<size_t>(c)], result);
        }

        if (abs_r < end_abs_row && (rectangular || !is_wrapped)) {
            result.push_back('\n');
        }
    }
    return result;
}

} // namespace vater::vt

