#pragma once
/// @file scrollback.hpp
/// @brief L3 Terminal Core — Paged Ring-Buffer Scrollback with hybrid Raw/RLE storage,
/// zero-churn page recycling, and optional disk mmap spill.
/// Strictly Nisaba-agnostic (term_core).

#include "vt/cell.hpp"
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <vector>

namespace vater::vt {

/// Logical row metadata shared between live Grid and Scrollback.
struct RowMeta {
    bool wrapped{false};       // True if row soft-wrapped onto the next row
    bool prompt_start{false};  // OSC 133;A
    bool command_start{false}; // OSC 133;B
    bool output_start{false};  // OSC 133;C
    bool command_end{false};   // OSC 133;D
    int16_t exit_code{-1};     // Exit code from OSC 133;D;<code>
    uint16_t cols{0};
};

/// Unpacked row view returned when reading from scrollback.
struct ScrollbackRow {
    std::vector<Cell> cells;
    RowMeta meta{};
};

/// Fixed-page (256 lines/page) scrollback buffer:
/// - Uniform/blank rows are compressed via RLE (1 cell stored instead of N).
/// - Dense text rows are stored contiguously in a single `memcpy` (16 bytes/cell, zero RLE bloat).
/// - Evicted pages are recycled in O(1) with zero heap allocator churn during high-speed `cat`.
/// - Optional file-backed mmap spill for very long sessions.
class Scrollback {
public:
    static constexpr size_t kPageLines = 256;
    static constexpr size_t kDefaultMaxLines = 100000;

    explicit Scrollback(size_t max_lines = kDefaultMaxLines, bool enable_mmap_spill = false);
    ~Scrollback();

    Scrollback(const Scrollback&) = delete;
    Scrollback& operator=(const Scrollback&) = delete;
    Scrollback(Scrollback&&) noexcept;
    Scrollback& operator=(Scrollback&&) noexcept;

    void set_max_lines(size_t max_lines);
    [[nodiscard]] size_t max_lines() const noexcept { return max_lines_; }

    [[nodiscard]] size_t size() const noexcept { return total_lines_; }
    [[nodiscard]] bool empty() const noexcept { return total_lines_ == 0; }

    void push(std::span<const Cell> cells, const RowMeta& meta);
    /// Fast-path push for plain default-style ASCII rows (16:1 compact 1-byte/cell lossless storage).
    void push_ascii(std::span<const uint8_t> ascii, const RowMeta& meta);
    bool pop_back(ScrollbackRow& out);
    bool get(size_t index, ScrollbackRow& out, size_t min_cols = 0) const;
    void clear();

    [[nodiscard]] size_t memory_bytes() const noexcept;

private:
    struct StoredLine {
        RowMeta meta;
        uint32_t offset{0};      // Offset into Page::cell_slab (or Page::ascii_slab when kind==Ascii)
        uint16_t count{0};       // Stored element count
        uint16_t rle_len{0};     // Expanded length when kind==UniformRle
        uint8_t kind{0};         // 0=Empty, 1=RawCells, 2=UniformRle, 3=CompactAscii
    };

    struct Page {
        std::vector<StoredLine> lines;
        std::unique_ptr<Cell[]> cell_slab;
        size_t cell_cap{0};
        size_t cell_used{0};
        std::unique_ptr<uint8_t[]> ascii_slab;
        size_t ascii_cap{0};
        size_t ascii_used{0};
        size_t head_line{0};

        int spill_fd{-1};
        void* spill_map{nullptr};
        size_t spill_bytes{0};
        bool spilled{false};

        ~Page();
        Page() = default;
        Page(const Page&) = delete;
        Page& operator=(const Page&) = delete;
        Page(Page&& other) noexcept;
        Page& operator=(Page&& other) noexcept;

        [[nodiscard]] size_t live_lines() const noexcept { return lines.size() - head_line; }
        void ensure_cell_capacity(size_t need);
        void ensure_ascii_capacity(size_t need);
        void reset_for_reuse() noexcept;
        bool spill_to_mmap();
        void unspill_release() noexcept;
    };

    void evict_oldest();
    static void decode_line(const Page& page, size_t local_idx, ScrollbackRow& out, size_t min_cols);

    std::deque<Page> pages_;
    std::vector<Page> free_pages_; // Recycled pages to avoid malloc/free churn on high throughput
    size_t max_lines_{kDefaultMaxLines};
    size_t total_lines_{0};
    bool enable_mmap_spill_{false};
    static constexpr size_t kHotPagesInRam = 32;
};

} // namespace vater::vt
