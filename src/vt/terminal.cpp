/// @file terminal.cpp
/// @brief L3 Terminal Core — Thread-Safe Public Terminal Facade implementation.

#include "vt/terminal.hpp"

#include <algorithm>

namespace vater::vt {

Terminal::Terminal(int cols, int rows, size_t max_scrollback)
    : grid_(cols, rows, max_scrollback),
      dispatcher_(grid_, graphics_) {}

void Terminal::feed(const uint8_t* data, size_t len) {
    if (!data || len == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    parser_.feed(data, len, dispatcher_);

    const bool sync_now = grid_.modes().synchronized_output;
    if (sync_now && !was_sync_active_) {
        sync_started_ = std::chrono::steady_clock::now();
    }
    was_sync_active_ = sync_now;
}

void Terminal::resize(int cols, int rows) {
    std::lock_guard<std::mutex> lock(mutex_);
    grid_.resize(cols, rows);
}

bool Terminal::snapshot(ViewportSnapshot& out, bool consume_dirty) {
    std::lock_guard<std::mutex> lock(mutex_);

    // DEC 2026 Synchronized Output: freeze rendering until app resets ?2026l (with 200ms safety cap)
    if (grid_.modes().synchronized_output) {
        auto elapsed = std::chrono::steady_clock::now() - sync_started_;
        if (elapsed < std::chrono::milliseconds(200)) {
            return false;
        }
        grid_.modes_mut().synchronized_output = false;
        was_sync_active_ = false;
    }

    grid_.build_snapshot(out, consume_dirty);
    return true;
}

void Terminal::scroll_view(int delta_lines) {
    std::lock_guard<std::mutex> lock(mutex_);
    grid_.set_scroll_offset(grid_.scroll_offset() + delta_lines);
}

void Terminal::scroll_to_bottom() {
    std::lock_guard<std::mutex> lock(mutex_);
    grid_.set_scroll_offset(0);
}

void Terminal::scroll_to_offset(int offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    grid_.set_scroll_offset(offset);
}

void Terminal::set_callbacks(DispatcherCallbacks cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    dispatcher_.set_callbacks(std::move(cb));
}

void Terminal::set_clipboard_policy(ClipboardPolicy pol) {
    std::lock_guard<std::mutex> lock(mutex_);
    dispatcher_.set_clipboard_policy(pol);
}

void Terminal::set_png_decoder(PngDecodeFn fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    graphics_.set_png_decoder(std::move(fn));
}

void Terminal::set_zlib_decompressor(ZlibDecompressFn fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    graphics_.set_zlib_decompressor(std::move(fn));
}

void Terminal::set_cell_pixel_size(int cell_w, int cell_h) {
    std::lock_guard<std::mutex> lock(mutex_);
    dispatcher_.set_cell_pixel_size(cell_w, cell_h);
}

void Terminal::set_max_scrollback(size_t max_lines) {
    std::lock_guard<std::mutex> lock(mutex_);
    grid_.scrollback().set_max_lines(max_lines);
}

void Terminal::set_bidi_default(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    grid_.modes_mut().bidi_implicit = enabled;
    grid_.mark_all_dirty();
}

TerminalModes Terminal::modes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return grid_.modes();
}

int Terminal::cols() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return grid_.cols();
}

int Terminal::rows() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return grid_.rows();
}

int Terminal::scroll_offset() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return grid_.scroll_offset();
}

size_t Terminal::scrollback_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return grid_.scrollback().size();
}

std::string Terminal::title() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dispatcher_.title();
}

std::string Terminal::cwd() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dispatcher_.cwd();
}

int Terminal::last_exit_code() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dispatcher_.last_exit_code();
}

DynamicColorState Terminal::dynamic_colors() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dispatcher_.dynamic_colors();
}

bool Terminal::has_dirty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return grid_.has_dirty_rows();
}

std::string Terminal::extract_text(int start_abs_row, int start_col,
                                   int end_abs_row, int end_col,
                                   bool rectangular) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return grid_.extract_text(start_abs_row, start_col, end_abs_row, end_col, rectangular);
}

bool Terminal::select_word_at(int view_row, int col, std::string_view delimiters,
                              int& out_abs_row, int& out_col_start, int& out_col_end) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (view_row < 0 || view_row >= grid_.rows() || col < 0 || col >= grid_.cols()) return false;

    const int sb_total = grid_.modes().alt_screen ? 0 : static_cast<int>(grid_.scrollback().size());
    const int abs_row = sb_total - grid_.scroll_offset() + view_row;
    if (abs_row < 0 || abs_row >= sb_total + grid_.rows()) return false;

    ScrollbackRow sb_tmp;
    const std::vector<Cell>* cells = nullptr;
    if (!grid_.modes().alt_screen && abs_row < sb_total) {
        if (grid_.scrollback().get(static_cast<size_t>(abs_row), sb_tmp, static_cast<size_t>(grid_.cols()))) {
            cells = &sb_tmp.cells;
        }
    } else {
        cells = &grid_.row(abs_row - sb_total).cells;
    }
    if (!cells || col >= static_cast<int>(cells->size())) return false;

    auto is_delim = [&](char32_t cp) {
        if (cp <= 0x20u) return true;
        if (cp < 0x80u) {
            return delimiters.find(static_cast<char>(cp)) != std::string_view::npos;
        }
        return false;
    };

    const bool clicked_delim = is_delim((*cells)[static_cast<size_t>(col)].cp);
    int c0 = col;
    int c1 = col;
    while (c0 > 0 && is_delim((*cells)[static_cast<size_t>(c0 - 1)].cp) == clicked_delim) --c0;
    while (c1 + 1 < static_cast<int>(cells->size()) &&
           is_delim((*cells)[static_cast<size_t>(c1 + 1)].cp) == clicked_delim) ++c1;

    out_abs_row = abs_row;
    out_col_start = c0;
    out_col_end = c1;
    return true;
}

bool Terminal::select_command_output_at(int view_row, int& out_start_abs_row, int& out_end_abs_row) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (grid_.modes().alt_screen) return false;
    const int sb_total = static_cast<int>(grid_.scrollback().size());
    const int total = sb_total + grid_.rows();
    const int abs_row = sb_total - grid_.scroll_offset() + view_row;
    if (abs_row < 0 || abs_row >= total) return false;

    auto get_meta = [&](int r) -> RowMeta {
        if (r < sb_total) {
            ScrollbackRow tmp;
            if (grid_.scrollback().get(static_cast<size_t>(r), tmp, 0)) return tmp.meta;
            return RowMeta{};
        }
        return grid_.row(r - sb_total).meta;
    };

    int start = abs_row;
    while (start > 0 && !get_meta(start).output_start && !get_meta(start).prompt_start) {
        --start;
    }
    int end = abs_row;
    while (end + 1 < total && !get_meta(end + 1).prompt_start) {
        ++end;
    }
    out_start_abs_row = start;
    out_end_abs_row = end;
    return true;
}

bool Terminal::jump_to_prompt(bool previous) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (grid_.modes().alt_screen) return false;
    const int sb_total = static_cast<int>(grid_.scrollback().size());
    const int total = sb_total + grid_.rows();
    const int current_top = sb_total - grid_.scroll_offset();

    auto is_prompt = [&](int r) -> bool {
        if (r < sb_total) {
            ScrollbackRow tmp;
            return grid_.scrollback().get(static_cast<size_t>(r), tmp, 0) && tmp.meta.prompt_start;
        }
        return grid_.row(r - sb_total).meta.prompt_start;
    };

    if (previous) {
        for (int r = current_top - 1; r >= 0; --r) {
            if (is_prompt(r)) {
                grid_.set_scroll_offset(sb_total - r);
                return true;
            }
        }
    } else {
        for (int r = current_top + 1; r < total; ++r) {
            if (is_prompt(r)) {
                grid_.set_scroll_offset(std::max(0, sb_total - r));
                return true;
            }
        }
    }
    return false;
}

std::string Terminal::hyperlink_at(int view_row, int col) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (view_row < 0 || view_row >= grid_.rows() || col < 0 || col >= grid_.cols()) return {};
    const int sb_total = grid_.modes().alt_screen ? 0 : static_cast<int>(grid_.scrollback().size());
    const int abs_row = sb_total - grid_.scroll_offset() + view_row;
    if (abs_row < 0 || abs_row >= sb_total + grid_.rows()) return {};

    Cell c{};
    if (!grid_.modes().alt_screen && abs_row < sb_total) {
        ScrollbackRow tmp;
        if (!grid_.scrollback().get(static_cast<size_t>(abs_row), tmp, static_cast<size_t>(grid_.cols()))) return {};
        if (col >= static_cast<int>(tmp.cells.size())) return {};
        c = tmp.cells[static_cast<size_t>(col)];
    } else {
        c = grid_.cell(abs_row - sb_total, col);
    }

    if (c.ext == 0) return {};
    const auto& ext = grid_.ext_table().get(c.ext);
    if (ext.hyperlink_id == 0) return {};
    if (const auto* link = grid_.hyperlink_table().get(ext.hyperlink_id)) {
        return link->uri;
    }
    return {};
}

ExtCellData Terminal::get_ext_data(uint16_t ext_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return grid_.ext_table().get(ext_id);
}

std::u32string Terminal::get_grapheme(char32_t cp) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto* s = grid_.grapheme_table().lookup(cp)) return *s;
    return {};
}

void Terminal::copy_graphics(std::vector<ImagePlacement>& out_placements,
                             std::unordered_map<uint32_t, DecodedImage>& out_images,
                             uint64_t& in_out_generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (in_out_generation == graphics_.generation()) return;
    in_out_generation = graphics_.generation();
    out_placements = graphics_.placements();
    out_images = graphics_.images();
}

void Terminal::dump_all_lines_utf8(std::vector<std::string>& out_lines, size_t& out_scrollback_count) const {
    std::lock_guard<std::mutex> lock(mutex_);
    out_lines.clear();
    const bool alt = grid_.modes().alt_screen;
    out_scrollback_count = alt ? 0 : grid_.scrollback().size();
    out_lines.reserve(out_scrollback_count + static_cast<size_t>(grid_.rows()));

    ScrollbackRow sb_tmp;
    for (size_t i = 0; i < out_scrollback_count; ++i) {
        std::string s;
        if (grid_.scrollback().get(i, sb_tmp, 0)) {
            s.reserve(sb_tmp.cells.size());
            for (const auto& c : sb_tmp.cells) grid_.append_cell_utf8(c, s);
        }
        out_lines.push_back(std::move(s));
    }

    for (int r = 0; r < grid_.rows(); ++r) {
        const auto& row = grid_.row(r);
        std::string s;
        s.reserve(row.cells.size());
        for (const auto& c : row.cells) grid_.append_cell_utf8(c, s);
        out_lines.push_back(std::move(s));
    }
}

} // namespace vater::vt

