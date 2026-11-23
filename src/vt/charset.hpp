#pragma once
/// @file charset.hpp
/// @brief L3 Terminal Core — G0–G3 Character Sets & DEC Special Graphics line-drawing translation.
/// Strictly Nisaba-agnostic (term_core).

#include <array>
#include <cstdint>

namespace vater::vt {

enum class CharsetId : uint8_t {
    Ascii = 0,          // 'B' US ASCII
    DecSpecial = 1,     // '0' DEC Special Graphics (line drawing)
    Uk = 2,             // 'A' UK (£ for #)
    DecSupplemental = 3 // '<' / '%5'
};

struct CharsetState {
    std::array<CharsetId, 4> g{CharsetId::Ascii, CharsetId::Ascii, CharsetId::Ascii, CharsetId::Ascii};
    uint8_t gl{0};            // Active G-set in GL (0..3)
    int8_t single_shift{-1};  // -1 = none, 2 = SS2 (G2), 3 = SS3 (G3)

    void reset() noexcept {
        g = {CharsetId::Ascii, CharsetId::Ascii, CharsetId::Ascii, CharsetId::Ascii};
        gl = 0;
        single_shift = -1;
    }

    void designate(uint8_t slot, CharsetId cs) noexcept {
        if (slot < 4) g[slot] = cs;
    }

    [[nodiscard]] CharsetId active_charset() const noexcept {
        if (single_shift >= 0 && single_shift < 4) return g[static_cast<size_t>(single_shift)];
        return g[gl & 3u];
    }

    /// Translates a printable codepoint through the active GL/single-shift charset.
    /// Consumes single-shift if active.
    [[nodiscard]] char32_t translate(char32_t cp) noexcept {
        const CharsetId cs = active_charset();
        single_shift = -1;
        if (cs == CharsetId::Ascii || cp >= 0x80u) return cp;
        if (cs == CharsetId::Uk) {
            return (cp == U'#') ? U'\u00A3' : cp; // £
        }
        if (cs == CharsetId::DecSpecial && cp >= 0x5Fu && cp <= 0x7Eu) {
            return kDecSpecialTable[cp - 0x5Fu];
        }
        return cp;
    }

    /// True if fast-path ASCII bypass is safe (active charset is ASCII and no single shift).
    [[nodiscard]] bool is_plain_ascii() const noexcept {
        return single_shift < 0 && g[gl & 3u] == CharsetId::Ascii;
    }

private:
    // DEC Special Graphics mapping for 0x5F ('_') .. 0x7E ('~')
    static constexpr char32_t kDecSpecialTable[32] = {
        U'\u00A0', // 0x5F '_' -> NBSP (blank)
        U'\u25C6', // 0x60 '`' -> ◆ diamond
        U'\u2592', // 0x61 'a' -> ▒ checkerboard
        U'\u2409', // 0x62 'b' -> ␉ HT
        U'\u240C', // 0x63 'c' -> ␌ FF
        U'\u240D', // 0x64 'd' -> ␍ CR
        U'\u240A', // 0x65 'e' -> ␊ LF
        U'\u00B0', // 0x66 'f' -> ° degree
        U'\u00B1', // 0x67 'g' -> ± plus-minus
        U'\u2424', // 0x68 'h' -> ␤ NL
        U'\u240B', // 0x69 'i' -> ␋ VT
        U'\u2518', // 0x6A 'j' -> ┘ lower-right corner
        U'\u2510', // 0x6B 'k' -> ┐ upper-right corner
        U'\u250C', // 0x6C 'l' -> ┌ upper-left corner
        U'\u2514', // 0x6D 'm' -> └ lower-left corner
        U'\u253C', // 0x6E 'n' -> ┼ crossing lines
        U'\u23BA', // 0x6F 'o' -> ⎺ scan line 1
        U'\u23BB', // 0x70 'p' -> ⎻ scan line 3
        U'\u2500', // 0x71 'q' -> ─ horizontal line (scan line 5)
        U'\u23BC', // 0x72 'r' -> ⎼ scan line 7
        U'\u23BD', // 0x73 's' -> ⎽ scan line 9
        U'\u251C', // 0x74 't' -> ├ left tee
        U'\u2524', // 0x75 'u' -> ┤ right tee
        U'\u2534', // 0x76 'v' -> ┴ bottom tee
        U'\u252C', // 0x77 'w' -> ┬ top tee
        U'\u2502', // 0x78 'x' -> │ vertical line
        U'\u2264', // 0x79 'y' -> ≤ less-than-or-equal
        U'\u2265', // 0x7A 'z' -> ≥ greater-than-or-equal
        U'\u03C0', // 0x7B '{' -> π pi
        U'\u2260', // 0x7C '|' -> ≠ not-equal
        U'\u00A3', // 0x7D '}' -> £ UK pound
        U'\u00B7', // 0x7E '~' -> · centered dot
    };
};

} // namespace vater::vt

