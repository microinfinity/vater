#pragma once
/// @file utf8_decoder.hpp
/// @brief L3 Terminal Core — Branchless DFA UTF-8 decoder + SIMD fast-path ASCII scanner.
/// Zero external dependencies; Nisaba-agnostic (term_core).

#include <cstddef>
#include <cstdint>

#if defined(__AVX2__) || defined(__SSE2__)
#include <immintrin.h>
#endif

namespace vater::vt {

/// Branchless DFA UTF-8 decoder (Bjoern Hoehrmann algorithm).
/// Rejects overlong encodings, UTF-16 surrogates (U+D800..U+DFFF), and codepoints > U+10FFFF.
class Utf8Decoder {
public:
    static constexpr uint32_t kAccept = 0;
    static constexpr uint32_t kReject = 12;

    constexpr void reset() noexcept {
        state_ = kAccept;
        codepoint_ = 0;
    }

    [[nodiscard]] constexpr bool in_sequence() const noexcept {
        return state_ != kAccept;
    }

    /// Feeds a single byte.
    /// Returns:
    ///   1 if a complete valid codepoint was produced in `out_cp`
    ///   0 if more bytes are needed
    ///  -1 if an invalid sequence was encountered (decoder resets to ground; caller emits U+FFFD
    ///     and may re-feed the triggering byte if it was a valid starter).
    constexpr int decode(uint8_t byte, char32_t& out_cp) noexcept {
        const uint32_t type = kByteClass[byte];
        codepoint_ = (state_ != kAccept)
                         ? ((byte & 0x3Fu) | (codepoint_ << 6))
                         : ((0xFFu >> type) & byte);
        state_ = kTransition[state_ + type];
        if (state_ == kAccept) {
            out_cp = static_cast<char32_t>(codepoint_);
            codepoint_ = 0;
            return 1;
        }
        if (state_ == kReject) {
            state_ = kAccept;
            codepoint_ = 0;
            out_cp = 0xFFFDu;
            return -1;
        }
        return 0;
    }

private:
    uint32_t state_{kAccept};
    uint32_t codepoint_{0};

    // Maps byte value -> character class (0..11)
    static constexpr uint8_t kByteClass[256] = {
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,  9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
        7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,  7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
        8,8,2,2,2,2,2,2,2,2,2,2,2,2,2,2,  2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
        10,3,3,3,3,3,3,3,3,3,3,3,3,4,3,3, 11,6,6,6,5,8,8,8,8,8,8,8,8,8,8,8,
    };

    // State transition table (state_offset + char_class -> next_state_offset)
    static constexpr uint8_t kTransition[108] = {
         0,12,24,36,60,96,84,12,12,12,48,72,  // state 0  (Accept)
        12,12,12,12,12,12,12,12,12,12,12,12,  // state 12 (Reject)
        12, 0,12,12,12,12,12, 0,12, 0,12,12,  // state 24 (1 cont left)
        12,24,12,12,12,12,12,24,12,24,12,12,  // state 36 (2 cont left)
        12,12,12,12,12,12,12,24,12,12,12,12,  // state 48 (E0 -> 0xA0..0xBF)
        12,24,12,12,12,12,12,12,12,24,12,12,  // state 60 (ED -> 0x80..0x9F, no surrogates)
        12,12,12,12,12,12,12,36,12,36,12,12,  // state 72 (F0 -> 0x90..0xBF)
        12,36,12,12,12,12,12,36,12,36,12,12,  // state 84 (F1..F3 -> 3 cont)
        12,36,12,12,12,12,12,12,12,12,12,12,  // state 96 (F4 -> 0x80..0x8F, <= U+10FFFF)
    };
};

/// Encodes a Unicode codepoint to UTF-8 into `out` (must have >= 4 bytes). Returns byte count (1..4).
[[nodiscard]] constexpr size_t encode_utf8(char32_t cp, char* out) noexcept {
    if (cp < 0x80u) {
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = static_cast<char>(0xC0u | (cp >> 6));
        out[1] = static_cast<char>(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = static_cast<char>(0xE0u | (cp >> 12));
        out[1] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = static_cast<char>(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (cp <= 0x10FFFFu) {
        out[0] = static_cast<char>(0xF0u | (cp >> 18));
        out[1] = static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
        out[2] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out[3] = static_cast<char>(0x80u | (cp & 0x3Fu));
        return 4;
    }
    // Replacement character U+FFFD
    out[0] = static_cast<char>(0xEF);
    out[1] = static_cast<char>(0xBF);
    out[2] = static_cast<char>(0xBD);
    return 3;
}

/// SIMD fast-path scanner: returns the length of the leading contiguous run of
/// printable ASCII bytes (0x20 <= b <= 0x7E) in `data[0..len)`.
/// Stops at any control byte (< 0x20), DEL (0x7F), or high-bit UTF-8 byte (>= 0x80).
[[nodiscard]] inline size_t scan_printable_ascii(const uint8_t* data, size_t len) noexcept {
    size_t i = 0;

#if defined(__AVX2__)
    const __m256i v_low  = _mm256_set1_epi8(0x20); // >= 0x20 (signed: >= 32)
    const __m256i v_del  = _mm256_set1_epi8(0x7F); // == 0x7F
    while (i + 32 <= len) {
        __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
        // Signed comparison: bytes < 0x20 (which also includes >= 0x80 since signed negative!)
        __m256i is_lt_20 = _mm256_cmpgt_epi8(v_low, chunk);
        __m256i is_del   = _mm256_cmpeq_epi8(chunk, v_del);
        __m256i bad      = _mm256_or_si256(is_lt_20, is_del);
        uint32_t mask    = static_cast<uint32_t>(_mm256_movemask_epi8(bad));
        if (mask != 0) {
            return i + static_cast<size_t>(__builtin_ctz(mask));
        }
        i += 32;
    }
#endif

#if defined(__SSE2__)
    const __m128i s_low = _mm_set1_epi8(0x20);
    const __m128i s_del = _mm_set1_epi8(0x7F);
    while (i + 16 <= len) {
        __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i));
        __m128i is_lt_20 = _mm_cmpgt_epi8(s_low, chunk);
        __m128i is_del   = _mm_cmpeq_epi8(chunk, s_del);
        __m128i bad      = _mm_or_si128(is_lt_20, is_del);
        uint32_t mask    = static_cast<uint32_t>(_mm_movemask_epi8(bad));
        if (mask != 0) {
            return i + static_cast<size_t>(__builtin_ctz(mask));
        }
        i += 16;
    }
#endif

    while (i < len) {
        const uint8_t b = data[i];
        if (b < 0x20u || b >= 0x7Fu) break;
        ++i;
    }
    return i;
}

} // namespace vater::vt

