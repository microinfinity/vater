/// @file fuzz_parser.cpp
/// @brief libFuzzer + ASan/UBSan target for `vater::vt::Terminal::feed` (§8 & §9).
/// Build with: meson setup build-fuzz -Dbuild_fuzz=true -Dcpp=clang++ && ninja -C build-fuzz fuzz_parser

#include "vt/terminal.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    vater::vt::Terminal term(80, 24, 256);
    term.feed(data, size);

    // Exercise resize reflow and snapshot under arbitrary corrupted/adversarial input
    if (size >= 2) {
        const int new_cols = 4 + (data[0] % 120);
        const int new_rows = 2 + (data[1] % 48);
        term.resize(new_cols, new_rows);
    }
    vater::vt::ViewportSnapshot snap;
    term.snapshot(snap);
    return 0;
}

