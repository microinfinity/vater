#pragma once
/// @file cell.hpp
/// @brief L3 Terminal Core — 16-byte cache-aligned Cell, Color encoding, Extended attributes & Grapheme clusters.
/// Strictly Nisaba-agnostic (term_core).

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vater::vt {

/// Color encoding stored inside Cell::fg and Cell::bg (32 bits):
///   Bits 24..31 = kind:
///     0x00 = Default foreground / background
///     0x01 = Indexed palette (0..255 in bits 0..7)
///     0x02 = Truecolor RGB (R in 16..23, G in 8..15, B in 0..7)
namespace color_tag {
inline constexpr uint32_t kDefault = 0x00000000u;
inline constexpr uint32_t kIndexed = 0x01000000u;
inline constexpr uint32_t kRgb     = 0x02000000u;
inline constexpr uint32_t kMask    = 0xFF000000u;
} // namespace color_tag

[[nodiscard]] constexpr uint32_t color_default() noexcept { return color_tag::kDefault; }
[[nodiscard]] constexpr uint32_t color_indexed(uint8_t idx) noexcept { return color_tag::kIndexed | idx; }
[[nodiscard]] constexpr uint32_t color_rgb(uint8_t r, uint8_t g, uint8_t b) noexcept {
    return color_tag::kRgb | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
}

[[nodiscard]] constexpr bool is_color_default(uint32_t c) noexcept { return (c & color_tag::kMask) == color_tag::kDefault; }
[[nodiscard]] constexpr bool is_color_indexed(uint32_t c) noexcept { return (c & color_tag::kMask) == color_tag::kIndexed; }
[[nodiscard]] constexpr bool is_color_rgb(uint32_t c) noexcept     { return (c & color_tag::kMask) == color_tag::kRgb; }

[[nodiscard]] constexpr uint8_t color_index(uint32_t c) noexcept { return static_cast<uint8_t>(c & 0xFFu); }
[[nodiscard]] constexpr uint8_t color_r(uint32_t c) noexcept { return static_cast<uint8_t>((c >> 16) & 0xFFu); }
[[nodiscard]] constexpr uint8_t color_g(uint32_t c) noexcept { return static_cast<uint8_t>((c >> 8) & 0xFFu); }
[[nodiscard]] constexpr uint8_t color_b(uint32_t c) noexcept { return static_cast<uint8_t>(c & 0xFFu); }

/// Underline styles (SGR 4:0..4:5) packed into bits 2..4 of Cell::attrs (3 bits).
enum class UnderlineStyle : uint8_t {
    None   = 0,
    Single = 1,
    Double = 2,
    Curly  = 3,
    Dotted = 4,
    Dashed = 5,
};

/// Bit layout for Cell::attrs (16 bits).
namespace cell_attr {
inline constexpr uint16_t kBold          = 1u << 0;
inline constexpr uint16_t kItalic        = 1u << 1;
inline constexpr uint16_t kUnderlineMask = 0x7u << 2; // bits 2..4 (UnderlineStyle)
inline constexpr uint16_t kUnderlineShift= 2u;
inline constexpr uint16_t kDim           = 1u << 5;
inline constexpr uint16_t kBlink         = 1u << 6;
inline constexpr uint16_t kInverse       = 1u << 7;
inline constexpr uint16_t kInvisible     = 1u << 8;
inline constexpr uint16_t kStrikethrough = 1u << 9;
inline constexpr uint16_t kWide          = 1u << 10; // Leading cell of 2-column char
inline constexpr uint16_t kWideSpacer    = 1u << 11; // Trailing spacer of 2-column char
inline constexpr uint16_t kOverline      = 1u << 12;
inline constexpr uint16_t kProtected     = 1u << 13; // DECSCA selective erase protection
inline constexpr uint16_t kHasExt        = 1u << 14; // Cell::ext is non-zero index into ExtCellTable
inline constexpr uint16_t kIsGrapheme    = 1u << 15; // Cell::cp is > 0x10FFFF index into GraphemeTable
} // namespace cell_attr

[[nodiscard]] constexpr UnderlineStyle get_underline_style(uint16_t attrs) noexcept {
    return static_cast<UnderlineStyle>((attrs & cell_attr::kUnderlineMask) >> cell_attr::kUnderlineShift);
}

[[nodiscard]] constexpr uint16_t set_underline_style(uint16_t attrs, UnderlineStyle st) noexcept {
    return static_cast<uint16_t>((attrs & ~cell_attr::kUnderlineMask) |
                                 ((static_cast<uint16_t>(st) & 0x7u) << cell_attr::kUnderlineShift));
}

/// Sentinel base offset for grapheme cluster IDs stored in Cell::cp (> U+10FFFF).
inline constexpr char32_t kGraphemeBase = 0x110000u;

/// 16-byte cache-aligned terminal cell (exact ARCHITECTURE.md §5.3 layout).
struct alignas(16) Cell {
    char32_t cp{U' '};
    uint32_t fg{color_default()};
    uint32_t bg{color_default()};
    uint16_t attrs{0};
    uint16_t ext{0};

    [[nodiscard]] constexpr bool is_wide() const noexcept        { return (attrs & cell_attr::kWide) != 0; }
    [[nodiscard]] constexpr bool is_wide_spacer() const noexcept { return (attrs & cell_attr::kWideSpacer) != 0; }
    [[nodiscard]] constexpr bool is_grapheme() const noexcept    { return cp >= kGraphemeBase; }
    [[nodiscard]] constexpr bool is_blank() const noexcept {
        return cp == U' ' && bg == color_default() && (attrs & ~(cell_attr::kProtected)) == 0 && ext == 0;
    }
    [[nodiscard]] constexpr bool operator==(const Cell&) const noexcept = default;
};

static_assert(sizeof(Cell) == 16, "Cell must be strictly 16 bytes per ARCHITECTURE.md §5.3");

/// Extended per-cell metadata (rare: hyperlinks, colored underlines, image placement).
struct ExtCellData {
    uint32_t hyperlink_id{0};      // 0 = none; index into HyperlinkTable
    uint32_t underline_color{color_default()};
    uint32_t image_id{0};          // Kitty/Sixel image placement ID
    uint16_t image_col{0};
    uint16_t image_row{0};

    [[nodiscard]] constexpr bool is_empty() const noexcept {
        return hyperlink_id == 0 && underline_color == color_default() && image_id == 0;
    }
    [[nodiscard]] constexpr bool operator==(const ExtCellData&) const noexcept = default;
};

/// Interned table of ExtCellData (index 0 = empty/none, up to 65535 entries).
class ExtCellTable {
public:
    ExtCellTable() {
        entries_.push_back(ExtCellData{}); // index 0 = empty
    }

    uint16_t intern(const ExtCellData& data) {
        if (data.is_empty()) return 0;
        // Linear probe on small table, hash map if larger
        for (size_t i = 1; i < entries_.size(); ++i) {
            if (entries_[i] == data) return static_cast<uint16_t>(i);
        }
        if (entries_.size() >= 65535) {
            // Recycle oldest non-zero slot gracefully to stay within uint16_t
            return static_cast<uint16_t>(entries_.size() - 1);
        }
        entries_.push_back(data);
        return static_cast<uint16_t>(entries_.size() - 1);
    }

    [[nodiscard]] const ExtCellData& get(uint16_t idx) const noexcept {
        if (idx < entries_.size()) return entries_[idx];
        return entries_[0];
    }

    void clear() {
        entries_.resize(1);
    }

private:
    std::vector<ExtCellData> entries_;
};

/// Interned grapheme cluster table for multi-codepoint cells (ZWJ emoji, Arabic tashkeel, combining marks).
class GraphemeTable {
public:
    static constexpr size_t kMaxCodepointsPerCluster = 16;
    static constexpr size_t kMaxClusters = 65536;

    /// Returns a Cell::cp token >= kGraphemeBase representing `cluster` (UTF-32 codepoints).
    char32_t intern(const std::u32string& cluster) {
        if (cluster.empty()) return U' ';
        if (cluster.size() == 1) return cluster[0];

        std::u32string clamped = cluster;
        if (clamped.size() > kMaxCodepointsPerCluster) {
            clamped.resize(kMaxCodepointsPerCluster);
        }

        auto it = index_.find(clamped);
        if (it != index_.end()) {
            return kGraphemeBase + it->second;
        }
        if (clusters_.size() >= kMaxClusters) {
            // Bounded memory: evict/reset ring slot
            uint32_t slot = next_evict_ % static_cast<uint32_t>(kMaxClusters);
            ++next_evict_;
            index_.erase(clusters_[slot]);
            clusters_[slot] = clamped;
            index_[clamped] = slot;
            return kGraphemeBase + slot;
        }
        uint32_t id = static_cast<uint32_t>(clusters_.size());
        clusters_.push_back(clamped);
        index_[clamped] = id;
        return kGraphemeBase + id;
    }

    /// Appends a combining/extender codepoint to an existing cell's cp (single or existing cluster).
    char32_t append(char32_t existing_cp, char32_t mark) {
        std::u32string cluster;
        if (existing_cp >= kGraphemeBase) {
            uint32_t id = static_cast<uint32_t>(existing_cp - kGraphemeBase);
            if (id < clusters_.size()) {
                cluster = clusters_[id];
            } else {
                cluster.push_back(U' ');
            }
        } else {
            cluster.push_back(existing_cp);
        }
        if (cluster.size() < kMaxCodepointsPerCluster) {
            cluster.push_back(mark);
        }
        return intern(cluster);
    }

    [[nodiscard]] const std::u32string* lookup(char32_t cp) const noexcept {
        if (cp < kGraphemeBase) return nullptr;
        uint32_t id = static_cast<uint32_t>(cp - kGraphemeBase);
        if (id >= clusters_.size()) return nullptr;
        return &clusters_[id];
    }

    void clear() {
        clusters_.clear();
        index_.clear();
        next_evict_ = 0;
    }

private:
    std::vector<std::u32string> clusters_;
    std::unordered_map<std::u32string, uint32_t> index_;
    uint32_t next_evict_{0};
};

/// Interned OSC 8 Hyperlink table.
struct Hyperlink {
    std::string id;
    std::string uri;
};

class HyperlinkTable {
public:
    static constexpr size_t kMaxLinks = 4096;

    HyperlinkTable() {
        links_.push_back(Hyperlink{}); // 0 = no hyperlink
    }

    uint32_t intern(std::string_view id, std::string_view uri) {
        if (uri.empty()) return 0;
        std::string key;
        key.reserve(id.size() + 1 + uri.size());
        key.append(id);
        key.push_back('\x1f');
        key.append(uri);

        auto it = lookup_.find(key);
        if (it != lookup_.end()) return it->second;

        if (links_.size() >= kMaxLinks) {
            // Overwrite oldest non-zero link
            uint32_t slot = 1 + (evict_cursor_++ % (kMaxLinks - 1));
            links_[slot] = Hyperlink{std::string(id), std::string(uri)};
            lookup_[key] = slot;
            return slot;
        }
        uint32_t idx = static_cast<uint32_t>(links_.size());
        links_.push_back(Hyperlink{std::string(id), std::string(uri)});
        lookup_[key] = idx;
        return idx;
    }

    [[nodiscard]] const Hyperlink* get(uint32_t idx) const noexcept {
        if (idx == 0 || idx >= links_.size()) return nullptr;
        return &links_[idx];
    }

    void clear() {
        links_.resize(1);
        lookup_.clear();
        evict_cursor_ = 0;
    }

private:
    std::vector<Hyperlink> links_;
    std::unordered_map<std::string, uint32_t> lookup_;
    uint32_t evict_cursor_{0};
};

/// Current pen (SGR + hyperlink) applied to newly written cells.
struct CellPen {
    uint32_t fg{color_default()};
    uint32_t bg{color_default()};
    uint16_t attrs{0};
    ExtCellData ext{};
    uint16_t cached_ext_id{0};

    void reset() noexcept {
        fg = color_default();
        bg = color_default();
        attrs = 0;
        ext.underline_color = color_default();
        // Note: OSC 8 hyperlink persists across SGR 0 per OSC 8 spec.
    }
};

} // namespace vater::vt

