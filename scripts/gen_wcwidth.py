#!/usr/bin/env python3
"""
Vater — Unicode 16 wcwidth table generator (scripts/gen_wcwidth.py).
Generates src/vt/wcwidth.hpp with zero external dependencies at runtime:
  - Fast-path for ASCII (0x20..0x7E -> 1, C0/DEL -> -1 or 0)
  - Binary search tables for zero-width (combining/format/variation selectors/ZWJ)
    and double-width (East Asian Wide/Fullwidth + Emoji Presentation).
Uses Python's built-in unicodedata module (with Unicode 16 Emoji/Wide ranges).
"""
from __future__ import annotations

import pathlib
import sys
import unicodedata


def merge_ranges(codepoints: list[int]) -> list[tuple[int, int]]:
    if not codepoints:
        return []
    codepoints = sorted(set(codepoints))
    out: list[tuple[int, int]] = []
    start = prev = codepoints[0]
    for cp in codepoints[1:]:
        if cp == prev + 1:
            prev = cp
        else:
            out.append((start, prev))
            start = prev = cp
    out.append((start, prev))
    return out


# Unicode 16 Emoji_Presentation ranges that may be East_Asian_Width=N in older Python
# unicodedata builds, ensuring consistent double-width rendering in terminals.
EXTRA_WIDE_RANGES: list[tuple[int, int]] = [
    (0x1F004, 0x1F004),
    (0x1F0CF, 0x1F0CF),
    (0x1F18E, 0x1F18E),
    (0x1F191, 0x1F19A),
    (0x1F1E6, 0x1F1FF),  # Regional Indicator Symbols
    (0x1F201, 0x1F202),
    (0x1F21A, 0x1F21A),
    (0x1F22F, 0x1F22F),
    (0x1F232, 0x1F23A),
    (0x1F250, 0x1F251),
    (0x1F300, 0x1F320),
    (0x1F32D, 0x1F335),
    (0x1F337, 0x1F37C),
    (0x1F37E, 0x1F393),
    (0x1F3A0, 0x1F3CA),
    (0x1F3CF, 0x1F3D3),
    (0x1F3E0, 0x1F3F0),
    (0x1F3F4, 0x1F3F4),
    (0x1F3F8, 0x1F43E),
    (0x1F440, 0x1F440),
    (0x1F442, 0x1F4FC),
    (0x1F4FF, 0x1F53D),
    (0x1F54B, 0x1F54E),
    (0x1F550, 0x1F567),
    (0x1F57A, 0x1F57A),
    (0x1F595, 0x1F596),
    (0x1F5A4, 0x1F5A4),
    (0x1F5FB, 0x1F64F),
    (0x1F680, 0x1F6C5),
    (0x1F6CC, 0x1F6CC),
    (0x1F6D0, 0x1F6D2),
    (0x1F6D5, 0x1F6D7),
    (0x1F6DC, 0x1F6DF),
    (0x1F6EB, 0x1F6EC),
    (0x1F6F4, 0x1F6FC),
    (0x1F7E0, 0x1F7EB),
    (0x1F7F0, 0x1F7F0),
    (0x1F90C, 0x1F93A),
    (0x1F93C, 0x1F945),
    (0x1F947, 0x1F9FF),
    (0x1FA70, 0x1FA7C),
    (0x1FA80, 0x1FA89),
    (0x1FA8F, 0x1FAC6),
    (0x1FACE, 0x1FADC),
    (0x1FADF, 0x1FAE9),
    (0x1FAF0, 0x1FAF8),
    # CJK Extension I & Unicode 16 additions
    (0x2EBF0, 0x2EE5D),
    (0x30000, 0x3FFFD),
]

# Format characters that should remain zero-width (Soft Hyphen 0x00AD is width 1 in POSIX wcwidth)
EXPLICIT_ZERO_WIDTH: set[int] = {
    0x200B,  # ZERO WIDTH SPACE
    0x200C,  # ZERO WIDTH NON-JOINER
    0x200D,  # ZERO WIDTH JOINER
    0x200E,  # LEFT-TO-RIGHT MARK
    0x200F,  # RIGHT-TO-LEFT MARK
    0x202A, 0x202B, 0x202C, 0x202D, 0x202E,  # BiDi embeddings/overrides
    0x2060, 0x2061, 0x2062, 0x2063, 0x2064,
    0x2066, 0x2067, 0x2068, 0x2069,  # BiDi isolates
    0xFEFF,  # BOM / ZWNBSP
}


def build_tables() -> tuple[list[tuple[int, int]], list[tuple[int, int]]]:
    zero_cps: list[int] = []
    wide_cps: list[int] = []

    for cp in range(0x0080, 0x110000):
        if 0xD800 <= cp <= 0xDFFF:
            continue
        ch = chr(cp)
        cat = unicodedata.category(ch)

        # Hangul Jamo medial vowels and final consonants are zero-width combining
        if (0x1160 <= cp <= 0x11FF) or (0xD7B0 <= cp <= 0xD7FF):
            zero_cps.append(cp)
            continue

        if cp in EXPLICIT_ZERO_WIDTH:
            zero_cps.append(cp)
            continue

        # Combining marks (Mn, Me) and most format chars (Cf, excluding soft hyphen U+00AD and Arabic number signs)
        if cat in ("Mn", "Me"):
            zero_cps.append(cp)
            continue
        if cat == "Cf" and cp != 0x00AD and not (0x0600 <= cp <= 0x0605) and cp != 0x06DD and cp != 0x070F:
            zero_cps.append(cp)
            continue
        # Variation selectors (VS1..VS16, VS17..VS256)
        if (0xFE00 <= cp <= 0xFE0F) or (0xE0100 <= cp <= 0xE01EF):
            zero_cps.append(cp)
            continue

        eaw = unicodedata.east_asian_width(ch)
        if eaw in ("W", "F"):
            wide_cps.append(cp)

    # Merge explicit Unicode 16 wide ranges (excluding anything in zero_cps)
    zero_set = set(zero_cps)
    for lo, hi in EXTRA_WIDE_RANGES:
        for cp in range(lo, hi + 1):
            if cp not in zero_set:
                wide_cps.append(cp)

    return merge_ranges(zero_cps), merge_ranges(wide_cps)


def format_ranges(name: str, ranges: list[tuple[int, int]]) -> str:
    lines = [f"inline constexpr Range {name}[] = {{"]
    row: list[str] = []
    for lo, hi in ranges:
        row.append(f"{{0x{lo:04X}, 0x{hi:04X}}}")
        if len(row) == 4:
            lines.append("    " + ", ".join(row) + ",")
            row.clear()
    if row:
        lines.append("    " + ", ".join(row) + ",")
    lines.append("};")
    return "\n".join(lines)


def generate(out_path: pathlib.Path) -> None:
    zero_ranges, wide_ranges = build_tables()
    header = f"""#pragma once
// AUTO-GENERATED by scripts/gen_wcwidth.py (Unicode {unicodedata.unidata_version} + Unicode 16 Emoji/Wide).
// DO NOT EDIT MANUALLY. Zero external dependencies; Nisaba-agnostic (term_core).

#include <cstddef>
#include <cstdint>

namespace vater::vt {{

namespace detail {{

struct Range {{
    char32_t first;
    char32_t last;
}};

{format_ranges("kZeroWidthRanges", zero_ranges)}

{format_ranges("kWideRanges", wide_ranges)}

template <size_t N>
[[nodiscard]] constexpr bool in_ranges(char32_t cp, const Range (&table)[N]) noexcept {{
    size_t lo = 0;
    size_t hi = N;
    while (lo < hi) {{
        const size_t mid = lo + ((hi - lo) >> 1);
        if (cp < table[mid].first) {{
            hi = mid;
        }} else if (cp > table[mid].last) {{
            lo = mid + 1;
        }} else {{
            return true;
        }}
    }}
    return false;
}}

}} // namespace detail

/// Returns terminal display cell width of a Unicode codepoint:
///   -1 for non-printable C0/C1 control characters
///    0 for NUL, combining marks, zero-width joiners, variation selectors
///    1 for standard narrow/neutral characters
///    2 for East Asian Wide/Fullwidth and Emoji Presentation characters
[[nodiscard]] constexpr int wcwidth(char32_t cp) noexcept {{
    if (cp == 0) return 0;
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return -1;
    if (cp < 0x7F) return 1; // Fast-path printable ASCII
    if (cp > 0x10FFFF) return 1;
    if (detail::in_ranges(cp, detail::kZeroWidthRanges)) return 0;
    if (detail::in_ranges(cp, detail::kWideRanges)) return 2;
    return 1;
}}

/// Returns true if codepoint is a combining mark or grapheme cluster extender
/// (should attach to the previous cell as a grapheme cluster rather than advancing).
[[nodiscard]] constexpr bool is_combining_or_extender(char32_t cp) noexcept {{
    if (cp < 0x0300) return false;
    if (cp == 0x200D) return true; // ZWJ
    if ((cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xE0100 && cp <= 0xE01EF)) return true; // VS
    if (cp >= 0x1F3FB && cp <= 0x1F3FF) return true; // Skin tone modifiers
    return detail::in_ranges(cp, detail::kZeroWidthRanges);
}}

}} // namespace vater::vt
"""
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(header, encoding="utf-8")


if __name__ == "__main__":
    dest = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path("src/vt/wcwidth.hpp")
    generate(dest)
    print(f"Generated {dest}")

