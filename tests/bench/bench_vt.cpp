/// @file bench_vt.cpp
/// @brief Performance Budget Verification (§7):
///   - Raw ASCII parser throughput target: >= 1 GB/s
///   - 100 MB `cat` simulation target: < 0.5s
///   - Scrollback memory overhead measurement

#include "vt/parser.hpp"
#include "vt/terminal.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace {

class CountingHandler final : public vater::vt::VtHandler {
public:
    size_t ascii_bytes{0};
    size_t csi_count{0};
    void on_print(char32_t) override {}
    void on_print_ascii(const uint8_t*, size_t len) override { ascii_bytes += len; }
    void on_execute(uint8_t) override {}
    void on_esc(uint8_t, const uint8_t*, uint8_t) override {}
    void on_csi(uint8_t, const vater::vt::CsiParams&) override { ++csi_count; }
    void on_osc(std::string_view) override {}
    void on_dcs(uint8_t, const vater::vt::CsiParams&, std::string_view) override {}
    void on_apc(std::string_view) override {}
};

} // namespace

int main() {
    // 1. Raw ASCII Parser Throughput (100 MB buffer)
    constexpr size_t kTotalBytes = 100u * 1024u * 1024u;
    std::string line = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_-+=[]{}()/*&^%$#@\r\n";
    std::string chunk;
    chunk.reserve(65536 + line.size());
    while (chunk.size() < 65536) chunk += line;

    vater::vt::Parser parser;
    CountingHandler sink;
    const size_t iters = kTotalBytes / chunk.size();

    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iters; ++i) {
        parser.feed(chunk, sink);
    }
    auto t1 = std::chrono::steady_clock::now();
    const double sec_parser = std::chrono::duration<double>(t1 - t0).count();
    const double gbps = (static_cast<double>(iters * chunk.size()) / (1024.0 * 1024.0 * 1024.0)) / sec_parser;

    // 2. Full Terminal (Parser + Dispatcher + Grid + Scrollback) 100 MB `cat` simulation
    vater::vt::Terminal term(120, 40, 10000);
    auto t2 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iters; ++i) {
        term.feed(chunk);
    }
    auto t3 = std::chrono::steady_clock::now();
    const double sec_cat = std::chrono::duration<double>(t3 - t2).count();
    const size_t sb_kb = term.unsafe_grid().scrollback().memory_bytes() / 1024;

    std::printf("Vater §7 Performance Benchmark:\n");
    std::printf("  [1] Raw ASCII Parser throughput : %.2f GB/s (%.3f s for 100 MB)  [target >= 1.00 GB/s]\n",
                gbps, sec_parser);
    std::printf("  [2] Full Terminal cat 100 MB    : %.3f s (%.1f MB/s)             [target < 0.500 s]\n",
                sec_cat, (100.0 / sec_cat));
    std::printf("  [3] Scrollback (10k lines RLE)  : %zu KB\n", sb_kb);
    return 0;
}

