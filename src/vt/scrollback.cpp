/// @file scrollback.cpp
/// @brief L3 Terminal Core — High-throughput Paged Ring-Buffer Scrollback implementation.
/// Features 16:1 lossless compact ASCII lane, Uniform RLE, Raw Cell slabs, page recycling, and mmap spill.

#include "vt/scrollback.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

namespace vater::vt {

namespace {
inline constexpr uint8_t kKindEmpty        = 0;
inline constexpr uint8_t kKindRawCells     = 1;
inline constexpr uint8_t kKindUniformRle   = 2;
inline constexpr uint8_t kKindCompactAscii = 3;
} // namespace

Scrollback::Page::~Page() {
    unspill_release();
}

Scrollback::Page::Page(Page&& other) noexcept
    : lines(std::move(other.lines)),
      cell_slab(std::move(other.cell_slab)),
      cell_cap(other.cell_cap),
      cell_used(other.cell_used),
      ascii_slab(std::move(other.ascii_slab)),
      ascii_cap(other.ascii_cap),
      ascii_used(other.ascii_used),
      head_line(other.head_line),
      spill_fd(other.spill_fd),
      spill_map(other.spill_map),
      spill_bytes(other.spill_bytes),
      spilled(other.spilled) {
    other.cell_cap = 0;
    other.cell_used = 0;
    other.ascii_cap = 0;
    other.ascii_used = 0;
    other.spill_fd = -1;
    other.spill_map = nullptr;
    other.spill_bytes = 0;
    other.spilled = false;
    other.head_line = 0;
}

Scrollback::Page& Scrollback::Page::operator=(Page&& other) noexcept {
    if (this != &other) {
        unspill_release();
        lines = std::move(other.lines);
        cell_slab = std::move(other.cell_slab);
        cell_cap = other.cell_cap;
        cell_used = other.cell_used;
        ascii_slab = std::move(other.ascii_slab);
        ascii_cap = other.ascii_cap;
        ascii_used = other.ascii_used;
        head_line = other.head_line;
        spill_fd = other.spill_fd;
        spill_map = other.spill_map;
        spill_bytes = other.spill_bytes;
        spilled = other.spilled;
        other.cell_cap = 0;
        other.cell_used = 0;
        other.ascii_cap = 0;
        other.ascii_used = 0;
        other.spill_fd = -1;
        other.spill_map = nullptr;
        other.spill_bytes = 0;
        other.spilled = false;
        other.head_line = 0;
    }
    return *this;
}

void Scrollback::Page::ensure_cell_capacity(size_t need) {
    if (need <= cell_cap) return;
    size_t new_cap = std::max<size_t>(cell_cap * 2, kPageLines * 32);
    while (new_cap < need) new_cap *= 2;
    auto next = std::make_unique_for_overwrite<Cell[]>(new_cap);
    if (cell_slab && cell_used > 0) {
        std::memcpy(next.get(), cell_slab.get(), cell_used * sizeof(Cell));
    }
    cell_slab = std::move(next);
    cell_cap = new_cap;
}

void Scrollback::Page::ensure_ascii_capacity(size_t need) {
    if (need <= ascii_cap) return;
    size_t new_cap = std::max<size_t>(ascii_cap * 2, kPageLines * 128);
    while (new_cap < need) new_cap *= 2;
    auto next = std::make_unique_for_overwrite<uint8_t[]>(new_cap);
    if (ascii_slab && ascii_used > 0) {
        std::memcpy(next.get(), ascii_slab.get(), ascii_used);
    }
    ascii_slab = std::move(next);
    ascii_cap = new_cap;
}

void Scrollback::Page::reset_for_reuse() noexcept {
    unspill_release();
    lines.clear();
    cell_used = 0;
    ascii_used = 0;
    head_line = 0;
}

void Scrollback::Page::unspill_release() noexcept {
    if (spill_map && spill_map != MAP_FAILED) {
        ::munmap(spill_map, spill_bytes);
    }
    if (spill_fd >= 0) {
        ::close(spill_fd);
    }
    spill_map = nullptr;
    spill_fd = -1;
    spill_bytes = 0;
    spilled = false;
}

bool Scrollback::Page::spill_to_mmap() {
    if (spilled || cell_used == 0 || !cell_slab) return false;
    const size_t bytes = cell_used * sizeof(Cell);

    const char* tmpdir = std::getenv("XDG_RUNTIME_DIR");
    if (!tmpdir || tmpdir[0] == '\0') tmpdir = "/tmp";
    std::string tmpl = std::string(tmpdir) + "/vater-sb-XXXXXX";
    std::vector<char> path_buf(tmpl.begin(), tmpl.end());
    path_buf.push_back('\0');

    int fd = ::mkostemp(path_buf.data(), O_CLOEXEC);
    if (fd < 0) return false;
    ::unlink(path_buf.data());

    if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        ::close(fd);
        return false;
    }

    void* map = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        ::close(fd);
        return false;
    }

    std::memcpy(map, cell_slab.get(), bytes);
    ::madvise(map, bytes, MADV_DONTNEED);

    cell_slab.reset();
    cell_cap = 0;

    spill_fd = fd;
    spill_map = map;
    spill_bytes = bytes;
    spilled = true;
    return true;
}

Scrollback::Scrollback(size_t max_lines, bool enable_mmap_spill)
    : max_lines_(max_lines), enable_mmap_spill_(enable_mmap_spill) {}

Scrollback::~Scrollback() = default;
Scrollback::Scrollback(Scrollback&&) noexcept = default;
Scrollback& Scrollback::operator=(Scrollback&&) noexcept = default;

void Scrollback::set_max_lines(size_t max_lines) {
    max_lines_ = max_lines;
    while (total_lines_ > max_lines_) {
        evict_oldest();
    }
}

void Scrollback::push_ascii(std::span<const uint8_t> ascii, const RowMeta& meta) {
    if (max_lines_ == 0) return;

    while (total_lines_ >= max_lines_) {
        evict_oldest();
    }

    if (pages_.empty() || pages_.back().lines.size() >= kPageLines || pages_.back().spilled) {
        if (!free_pages_.empty()) {
            Page recycled = std::move(free_pages_.back());
            free_pages_.pop_back();
            recycled.reset_for_reuse();
            pages_.push_back(std::move(recycled));
        } else {
            Page p;
            p.lines.reserve(kPageLines);
            p.ensure_ascii_capacity(kPageLines * 128);
            pages_.push_back(std::move(p));
        }
    }

    Page& page = pages_.back();
    size_t trimmed_len = ascii.size();
    while (trimmed_len > 0 && ascii[trimmed_len - 1] == ' ') --trimmed_len;

    StoredLine sl{};
    sl.meta = meta;
    sl.offset = static_cast<uint32_t>(page.ascii_used);
    if (trimmed_len == 0) {
        sl.kind = kKindEmpty;
        sl.count = 0;
    } else {
        page.ensure_ascii_capacity(page.ascii_used + trimmed_len);
        std::memcpy(page.ascii_slab.get() + page.ascii_used, ascii.data(), trimmed_len);
        page.ascii_used += trimmed_len;
        sl.kind = kKindCompactAscii;
        sl.count = static_cast<uint16_t>(trimmed_len);
    }

    page.lines.push_back(sl);
    ++total_lines_;
}

void Scrollback::push(std::span<const Cell> cells, const RowMeta& meta) {
    if (max_lines_ == 0) return;

    while (total_lines_ >= max_lines_) {
        evict_oldest();
    }

    if (pages_.empty() || pages_.back().lines.size() >= kPageLines || pages_.back().spilled) {
        if (enable_mmap_spill_ && pages_.size() >= kHotPagesInRam) {
            size_t spill_idx = pages_.size() - kHotPagesInRam;
            if (!pages_[spill_idx].spilled) {
                pages_[spill_idx].spill_to_mmap();
            }
        }
        if (!free_pages_.empty()) {
            Page recycled = std::move(free_pages_.back());
            free_pages_.pop_back();
            recycled.reset_for_reuse();
            pages_.push_back(std::move(recycled));
        } else {
            Page p;
            p.lines.reserve(kPageLines);
            pages_.push_back(std::move(p));
        }
    }

    Page& page = pages_.back();

    size_t trimmed_len = cells.size();
    while (trimmed_len > 0 && cells[trimmed_len - 1].is_blank()) {
        --trimmed_len;
    }

    RowMeta stored_meta = meta;
    if (stored_meta.cols == 0) stored_meta.cols = static_cast<uint16_t>(cells.size());

    StoredLine sl{};
    sl.meta = stored_meta;

    if (trimmed_len == 0) {
        sl.kind = kKindEmpty;
        sl.count = 0;
    } else {
        // Single-pass optimistic 16:1 compact ASCII pack (zero extra pass!)
        page.ensure_ascii_capacity(page.ascii_used + trimmed_len);
        uint8_t* dst = page.ascii_slab.get() + page.ascii_used;
        bool all_plain_ascii = true;
        for (size_t i = 0; i < trimmed_len; ++i) {
            const Cell& c = cells[i];
            if ((c.cp >= 0x80u) | (c.fg != 0u) | (c.bg != 0u) | (c.attrs != 0u) | (c.ext != 0u)) {
                all_plain_ascii = false;
                break;
            }
            dst[i] = static_cast<uint8_t>(c.cp);
        }

        if (all_plain_ascii) {
            sl.kind = kKindCompactAscii;
            sl.offset = static_cast<uint32_t>(page.ascii_used);
            sl.count = static_cast<uint16_t>(trimmed_len);
            page.ascii_used += trimmed_len;
        } else if (trimmed_len >= 4 && cells[0] == cells[1] && cells[0] == cells[trimmed_len - 1]) {
            bool uniform = true;
            const Cell& first = cells[0];
            for (size_t i = 2; i + 1 < trimmed_len; ++i) {
                if (!(cells[i] == first)) { uniform = false; break; }
            }
            if (uniform) {
                page.ensure_cell_capacity(page.cell_used + 1);
                sl.offset = static_cast<uint32_t>(page.cell_used);
                page.cell_slab[page.cell_used++] = first;
                sl.kind = kKindUniformRle;
                sl.count = 1;
                sl.rle_len = static_cast<uint16_t>(trimmed_len);
            } else {
                page.ensure_cell_capacity(page.cell_used + trimmed_len);
                sl.offset = static_cast<uint32_t>(page.cell_used);
                std::memcpy(page.cell_slab.get() + page.cell_used, cells.data(), trimmed_len * sizeof(Cell));
                page.cell_used += trimmed_len;
                sl.kind = kKindRawCells;
                sl.count = static_cast<uint16_t>(trimmed_len);
            }
        } else {
            page.ensure_cell_capacity(page.cell_used + trimmed_len);
            sl.offset = static_cast<uint32_t>(page.cell_used);
            std::memcpy(page.cell_slab.get() + page.cell_used, cells.data(), trimmed_len * sizeof(Cell));
            page.cell_used += trimmed_len;
            sl.kind = kKindRawCells;
            sl.count = static_cast<uint16_t>(trimmed_len);
        }
    }

    page.lines.push_back(sl);
    ++total_lines_;
}

void Scrollback::evict_oldest() {
    if (pages_.empty() || total_lines_ == 0) return;
    Page& front = pages_.front();
    ++front.head_line;
    --total_lines_;
    if (front.head_line >= front.lines.size()) {
        if (free_pages_.size() < 4) {
            free_pages_.push_back(std::move(front));
        }
        pages_.pop_front();
    }
}

void Scrollback::decode_line(const Page& page, size_t local_idx, ScrollbackRow& out, size_t min_cols) {
    const StoredLine& sl = page.lines[local_idx];
    out.meta = sl.meta;
    const size_t content_len = (sl.kind == kKindUniformRle) ? sl.rle_len : sl.count;
    const size_t target_cols = std::max({min_cols, content_len, static_cast<size_t>(sl.meta.cols)});
    out.cells.resize(target_cols);

    if (sl.kind == kKindCompactAscii && page.ascii_slab && sl.offset + sl.count <= page.ascii_used) {
        const uint8_t* src = page.ascii_slab.get() + sl.offset;
        for (size_t i = 0; i < sl.count; ++i) {
            Cell c{};
            c.cp = static_cast<char32_t>(src[i]);
            out.cells[i] = c;
        }
    } else if (sl.kind == kKindRawCells || sl.kind == kKindUniformRle) {
        const Cell* src_ptr = page.spilled
                                  ? static_cast<const Cell*>(page.spill_map)
                                  : page.cell_slab.get();
        const size_t max_cells = page.spilled
                                     ? (page.spill_bytes / sizeof(Cell))
                                     : page.cell_used;
        if (src_ptr && sl.count > 0 && sl.offset + sl.count <= max_cells) {
            if (sl.kind == kKindUniformRle) {
                std::fill_n(out.cells.begin(), sl.rle_len, src_ptr[sl.offset]);
            } else {
                std::memcpy(out.cells.data(), src_ptr + sl.offset, sl.count * sizeof(Cell));
            }
        }
    }

    if (content_len < target_cols) {
        std::fill(out.cells.begin() + static_cast<ptrdiff_t>(content_len), out.cells.end(), Cell{});
    }
}

bool Scrollback::pop_back(ScrollbackRow& out) {
    if (total_lines_ == 0 || pages_.empty()) return false;
    Page& back = pages_.back();
    const size_t local_idx = back.lines.size() - 1;
    decode_line(back, local_idx, out, 0);

    const StoredLine sl = back.lines.back();
    back.lines.pop_back();
    if (sl.kind == kKindCompactAscii && sl.offset <= back.ascii_used) {
        back.ascii_used = sl.offset;
    } else if ((sl.kind == kKindRawCells || sl.kind == kKindUniformRle) && !back.spilled && sl.offset <= back.cell_used) {
        back.cell_used = sl.offset;
    }
    --total_lines_;

    if (back.live_lines() == 0) {
        pages_.pop_back();
    }
    return true;
}

bool Scrollback::get(size_t index, ScrollbackRow& out, size_t min_cols) const {
    if (index >= total_lines_ || pages_.empty()) return false;

    size_t offset = index;
    for (const auto& page : pages_) {
        const size_t live = page.live_lines();
        if (offset < live) {
            decode_line(page, page.head_line + offset, out, min_cols);
            return true;
        }
        offset -= live;
    }
    return false;
}

void Scrollback::clear() {
    pages_.clear();
    free_pages_.clear();
    total_lines_ = 0;
}

size_t Scrollback::memory_bytes() const noexcept {
    size_t bytes = 0;
    for (const auto& p : pages_) {
        bytes += sizeof(Page);
        bytes += p.lines.capacity() * sizeof(StoredLine);
        bytes += p.ascii_cap;
        if (!p.spilled) {
            bytes += p.cell_cap * sizeof(Cell);
        }
    }
    return bytes;
}

} // namespace vater::vt
