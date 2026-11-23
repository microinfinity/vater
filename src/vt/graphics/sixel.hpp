#pragma once
/// @file sixel.hpp
/// @brief L3 Terminal Core — DEC Sixel (DCS q) Graphics Decoder.
/// Zero external dependencies; Nisaba-agnostic (term_core).

#include <cstdint>
#include <string_view>
#include <vector>

namespace vater::vt {

/// Decoded RGBA8888 inline image produced by Sixel or Kitty graphics protocols.
struct DecodedImage {
    uint32_t id{0};
    uint32_t width{0};
    uint32_t height{0};
    std::vector<uint8_t> rgba; // width * height * 4 (RGBA8888, unpremultiplied)
    uint64_t generation{0};
};

/// Inline image placement on the terminal grid (shared by Sixel and Kitty graphics).
struct ImagePlacement {
    uint32_t image_id{0};
    uint32_t placement_id{0};
    int row{0};             // Grid row (0-indexed)
    int col{0};             // Grid col (0-indexed)
    int cols{0};            // Display width in cells (0 = natural pixel size)
    int rows{0};            // Display height in cells (0 = natural pixel size)
    int src_x{0};
    int src_y{0};
    int src_w{0};
    int src_h{0};
    int z_index{0};
    bool is_alt_screen{false};
};

/// Decodes a Sixel DCS payload (`DCS P1;P2;P3 q <payload> ST`) into an RGBA8888 buffer.
/// Enforces strict dimension/memory limits (§8 security: max 4096x4096).
class SixelDecoder {
public:
    static constexpr uint32_t kMaxDimension = 4096;
    static constexpr size_t kMaxPixels = 4096u * 4096u;

    static bool decode(std::string_view payload,
                       bool transparent_bg,
                       uint32_t default_bg_rgba,
                       DecodedImage& out);
};

} // namespace vater::vt

