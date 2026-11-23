/// @file sixel.cpp
/// @brief L3 Terminal Core — DEC Sixel (DCS q) Graphics Decoder implementation.

#include "vt/graphics/sixel.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace vater::vt {

namespace {

uint32_t pack_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) noexcept {
    return static_cast<uint32_t>(r) |
           (static_cast<uint32_t>(g) << 8) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(a) << 24);
}

uint32_t hls_to_rgba(int h, int l, int s) noexcept {
    // DEC Sixel HLS: H in 0..360 (0=Blue, 120=Red, 240=Green), L in 0..100, S in 0..100
    const float lf = std::clamp(l, 0, 100) / 100.0f;
    const float sf = std::clamp(s, 0, 100) / 100.0f;
    if (sf <= 0.0f) {
        const auto v = static_cast<uint8_t>(std::round(lf * 255.0f));
        return pack_rgba(v, v, v, 255);
    }
    const float maxv = (lf <= 0.5f) ? (lf * (1.0f + sf)) : (lf + sf - lf * sf);
    const float minv = 2.0f * lf - maxv;
    auto hue_to_c = [&](float deg) -> uint8_t {
        while (deg < 0.0f) deg += 360.0f;
        while (deg >= 360.0f) deg -= 360.0f;
        float c = minv;
        if (deg < 60.0f) c = minv + (maxv - minv) * deg / 60.0f;
        else if (deg < 180.0f) c = maxv;
        else if (deg < 240.0f) c = minv + (maxv - minv) * (240.0f - deg) / 60.0f;
        return static_cast<uint8_t>(std::clamp(std::round(c * 255.0f), 0.0f, 255.0f));
    };
    const float hf = static_cast<float>(h % 360);
    // Sixel hue offset: 0 = Blue, 120 = Red, 240 = Green
    const uint8_t r = hue_to_c(hf);
    const uint8_t g = hue_to_c(hf - 120.0f);
    const uint8_t b = hue_to_c(hf + 120.0f);
    return pack_rgba(r, g, b, 255);
}

int parse_uint(std::string_view s, size_t& pos) noexcept {
    int val = 0;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
        if (val < 100000) val = val * 10 + (s[pos] - '0');
        ++pos;
    }
    return val;
}

} // namespace

bool SixelDecoder::decode(std::string_view payload,
                          bool transparent_bg,
                          uint32_t default_bg_rgba,
                          DecodedImage& out) {
    // Default VT340 16-color registers + 256 register table
    std::array<uint32_t, 256> palette{};
    palette[0]  = pack_rgba(0, 0, 0);
    palette[1]  = pack_rgba(51, 51, 204);
    palette[2]  = pack_rgba(204, 33, 33);
    palette[3]  = pack_rgba(51, 204, 51);
    palette[4]  = pack_rgba(204, 51, 204);
    palette[5]  = pack_rgba(51, 204, 204);
    palette[6]  = pack_rgba(204, 204, 51);
    palette[7]  = pack_rgba(135, 135, 135);
    palette[8]  = pack_rgba(66, 66, 66);
    palette[9]  = pack_rgba(84, 84, 255);
    palette[10] = pack_rgba(255, 84, 84);
    palette[11] = pack_rgba(84, 255, 84);
    palette[12] = pack_rgba(255, 84, 255);
    palette[13] = pack_rgba(84, 255, 255);
    palette[14] = pack_rgba(255, 255, 84);
    palette[15] = pack_rgba(204, 204, 204);

    // Dynamic scanline storage (grows safely up to kMaxDimension)
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t raster_w = 0;
    uint32_t raster_h = 0;

    std::vector<uint32_t> pixels;
    uint32_t buf_w = 0;
    uint32_t buf_h = 0;
    const uint32_t bg_fill = transparent_bg ? 0u : default_bg_rgba;

    auto ensure_canvas = [&](uint32_t need_w, uint32_t need_h) -> bool {
        if (need_w > kMaxDimension || need_h > kMaxDimension) return false;
        if (need_w <= buf_w && need_h <= buf_h) return true;
        const uint32_t new_w = std::min(kMaxDimension, std::max({buf_w, need_w, 64u}));
        const uint32_t new_h = std::min(kMaxDimension, std::max({buf_h, need_h, 64u}));
        if (static_cast<size_t>(new_w) * static_cast<size_t>(new_h) > kMaxPixels) return false;
        std::vector<uint32_t> next(static_cast<size_t>(new_w) * static_cast<size_t>(new_h), bg_fill);
        for (uint32_t y = 0; y < buf_h; ++y) {
            std::copy_n(pixels.data() + static_cast<size_t>(y) * buf_w,
                        buf_w,
                        next.data() + static_cast<size_t>(y) * new_w);
        }
        pixels = std::move(next);
        buf_w = new_w;
        buf_h = new_h;
        return true;
    };

    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t current_color = palette[15];
    size_t pos = 0;

    while (pos < payload.size()) {
        const char ch = payload[pos++];
        if (ch == '"') {
            // Raster attributes: " Pan ; Pad ; Ph ; Pv
            (void)parse_uint(payload, pos);
            if (pos < payload.size() && payload[pos] == ';') { ++pos; (void)parse_uint(payload, pos); }
            if (pos < payload.size() && payload[pos] == ';') { ++pos; raster_w = static_cast<uint32_t>(parse_uint(payload, pos)); }
            if (pos < payload.size() && payload[pos] == ';') { ++pos; raster_h = static_cast<uint32_t>(parse_uint(payload, pos)); }
            if (raster_w > 0 && raster_h > 0 && raster_w <= kMaxDimension && raster_h <= kMaxDimension) {
                ensure_canvas(raster_w, raster_h);
            }
        } else if (ch == '#') {
            // Color introducer: #Pc or #Pc;Pu;Px;Py;Pz
            const int reg = parse_uint(payload, pos) & 255;
            if (pos < payload.size() && payload[pos] == ';') {
                ++pos;
                const int cs = parse_uint(payload, pos);
                int p1 = 0, p2 = 0, p3 = 0;
                if (pos < payload.size() && payload[pos] == ';') { ++pos; p1 = parse_uint(payload, pos); }
                if (pos < payload.size() && payload[pos] == ';') { ++pos; p2 = parse_uint(payload, pos); }
                if (pos < payload.size() && payload[pos] == ';') { ++pos; p3 = parse_uint(payload, pos); }
                if (cs == 2) {
                    const uint8_t r = static_cast<uint8_t>(std::clamp(p1, 0, 100) * 255 / 100);
                    const uint8_t g = static_cast<uint8_t>(std::clamp(p2, 0, 100) * 255 / 100);
                    const uint8_t b = static_cast<uint8_t>(std::clamp(p3, 0, 100) * 255 / 100);
                    palette[static_cast<size_t>(reg)] = pack_rgba(r, g, b, 255);
                } else if (cs == 1) {
                    palette[static_cast<size_t>(reg)] = hls_to_rgba(p1, p2, p3);
                }
            }
            current_color = palette[static_cast<size_t>(reg)];
        } else if (ch == '!') {
            // Repeat introducer: !Pn<sixel>
            int rep = std::max(1, parse_uint(payload, pos));
            if (pos >= payload.size()) break;
            const char sym = payload[pos++];
            if (sym >= '?' && sym <= '~') {
                const uint8_t bits = static_cast<uint8_t>(sym - '?');
                rep = std::min(rep, static_cast<int>(kMaxDimension));
                if (!ensure_canvas(x + static_cast<uint32_t>(rep), y + 6)) break;
                for (int k = 0; k < rep; ++k) {
                    for (uint32_t bit = 0; bit < 6; ++bit) {
                        if (bits & (1u << bit)) {
                            pixels[static_cast<size_t>(y + bit) * buf_w + (x + static_cast<uint32_t>(k))] = current_color;
                        }
                    }
                }
                x += static_cast<uint32_t>(rep);
                width = std::max(width, x);
                height = std::max(height, y + 6);
            }
        } else if (ch == '$') {
            // Graphics Carriage Return
            x = 0;
        } else if (ch == '-') {
            // Graphics New Line
            x = 0;
            y += 6;
            if (y >= kMaxDimension) break;
        } else if (ch >= '?' && ch <= '~') {
            const uint8_t bits = static_cast<uint8_t>(ch - '?');
            if (!ensure_canvas(x + 1, y + 6)) break;
            for (uint32_t bit = 0; bit < 6; ++bit) {
                if (bits & (1u << bit)) {
                    pixels[static_cast<size_t>(y + bit) * buf_w + x] = current_color;
                }
            }
            ++x;
            width = std::max(width, x);
            height = std::max(height, y + 6);
        }
    }

    const uint32_t final_w = std::min(kMaxDimension, std::max(width, raster_w));
    const uint32_t final_h = std::min(kMaxDimension, std::max(height, raster_h));
    if (final_w == 0 || final_h == 0) return false;
    if (!ensure_canvas(final_w, final_h)) return false;

    out.width = final_w;
    out.height = final_h;
    out.rgba.resize(static_cast<size_t>(final_w) * static_cast<size_t>(final_h) * 4u);
    for (uint32_t py = 0; py < final_h; ++py) {
        uint8_t* dst = out.rgba.data() + static_cast<size_t>(py) * final_w * 4u;
        const uint32_t* src = pixels.data() + static_cast<size_t>(py) * buf_w;
        for (uint32_t px = 0; px < final_w; ++px) {
            const uint32_t c = src[px];
            dst[px * 4 + 0] = static_cast<uint8_t>(c & 0xFFu);
            dst[px * 4 + 1] = static_cast<uint8_t>((c >> 8) & 0xFFu);
            dst[px * 4 + 2] = static_cast<uint8_t>((c >> 16) & 0xFFu);
            dst[px * 4 + 3] = static_cast<uint8_t>((c >> 24) & 0xFFu);
        }
    }
    return true;
}

} // namespace vater::vt

