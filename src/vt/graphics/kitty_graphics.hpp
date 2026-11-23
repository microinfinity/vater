#pragma once
/// @file kitty_graphics.hpp
/// @brief L3 Terminal Core — Kitty Graphics Protocol (APC G) state & image store.
/// Handles base64 decoding, chunked transfers (m=1/m=0), raw RGB/RGBA, zlib (o=z),
/// PNG (f=100), direct/file/temp/POSIX-shm (t=d/f/t/s), placements, deletion (a=d),
/// query (a=q), and strict memory caps (§8). Strictly Nisaba-agnostic (term_core).

#include "vt/graphics/sixel.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vater::vt {

/// Optional external PNG decoder callback (injected by L4 via nisaba::image::load_image_from_memory).
using PngDecodeFn = std::function<bool(const uint8_t* data, size_t len, uint32_t& out_w, uint32_t& out_h, std::vector<uint8_t>& out_rgba)>;

/// Optional external RFC-1950 zlib decompressor callback (injected by L4 via nisaba::image::zlib_decompress).
using ZlibDecompressFn = std::function<bool(const uint8_t* data, size_t len, size_t expected_size, std::vector<uint8_t>& out_raw)>;

/// Stores all active inline images & placements (both Kitty Graphics and Sixel).
class GraphicsStore {
public:
    static constexpr size_t kMaxTotalImageBytes = 128u * 1024u * 1024u; // 128 MB security cap
    static constexpr uint32_t kMaxDimension = 8192;

    void set_png_decoder(PngDecodeFn fn) { png_decoder_ = std::move(fn); }
    void set_zlib_decompressor(ZlibDecompressFn fn) { zlib_decompressor_ = std::move(fn); }

    /// Processes a Kitty APC G payload (everything after `ESC _ G` up to `ST`).
    /// Returns optional response string to write back to the PTY (e.g. `\x1b_Gi=...;OK\x1b\\`).
    std::string handle_kitty_apc(std::string_view payload,
                                 int cursor_row,
                                 int cursor_col,
                                 int cell_px_w,
                                 int cell_px_h,
                                 bool alt_screen,
                                 int& out_cursor_advance_rows,
                                 int& out_cursor_advance_cols);

    /// Inserts a decoded Sixel image at the current cursor position.
    uint32_t add_sixel_image(DecodedImage img, int cursor_row, int cursor_col, bool alt_screen);

    /// Scrolls placements up when the screen scrolls.
    void scroll_placements_up(int rows, bool alt_screen);

    /// Clears placements on screen clear or reset.
    void clear_screen_placements(bool alt_screen);
    void clear_all();

    [[nodiscard]] const std::unordered_map<uint32_t, DecodedImage>& images() const noexcept { return images_; }
    [[nodiscard]] const std::vector<ImagePlacement>& placements() const noexcept { return placements_; }
    [[nodiscard]] uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] size_t total_bytes() const noexcept { return total_bytes_; }

    /// Base64 utilities (also used by OSC 52 clipboard).
    static bool decode_base64(std::string_view in, std::vector<uint8_t>& out);
    static std::string encode_base64(const uint8_t* data, size_t len);

private:
    struct ChunkState {
        bool active{false};
        char action{'t'};
        uint32_t format{32}; // 24=RGB, 32=RGBA, 100=PNG
        char medium{'d'};    // 'd'=direct, 'f'=file, 't'=temp file, 's'=POSIX shm
        char compression{0}; // 0=none, 'z'=zlib (RFC 1950)
        uint32_t id{0};
        uint32_t placement_id{0};
        uint32_t src_w{0};
        uint32_t src_h{0};
        size_t data_size{0};
        size_t data_offset{0};
        int cols{0};
        int rows{0};
        int z_index{0};
        int quiet{0};
        bool suppress_cursor_move{false};
        std::vector<uint8_t> data;
    };

    void evict_if_needed(size_t incoming_bytes);
    void remove_image(uint32_t id);

    std::unordered_map<uint32_t, DecodedImage> images_;
    std::vector<ImagePlacement> placements_;
    ChunkState chunk_{};
    PngDecodeFn png_decoder_;
    ZlibDecompressFn zlib_decompressor_;
    uint32_t next_id_{1};
    uint64_t generation_{1};
    size_t total_bytes_{0};
};

} // namespace vater::vt
