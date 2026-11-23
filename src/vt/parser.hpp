#pragma once
/// @file parser.hpp
/// @brief L3 Terminal Core — Full Paul Williams DEC ANSI VT500 State Machine.
/// Zero heap allocations in the hot path; SIMD ASCII fast-path; strict OSC/DCS/APC security caps.
/// Strictly Nisaba-agnostic (term_core).

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "vt/utf8_decoder.hpp"

namespace vater::vt {

/// Maximum CSI/DCS parameters & subparameters (zero-alloc fixed arrays).
inline constexpr size_t kMaxParams = 32;
inline constexpr size_t kMaxSubParams = 8;
inline constexpr size_t kMaxIntermediates = 4;

/// Security bounds (§8): prevent unbounded memory growth from malformed/hostile sequences.
inline constexpr size_t kMaxOscBytes = 256 * 1024;        // 256 KB (covers OSC 52 clipboard)
inline constexpr size_t kMaxDcsBytes = 16 * 1024 * 1024;  // 16 MB  (covers Sixel images)
inline constexpr size_t kMaxApcBytes = 32 * 1024 * 1024;  // 32 MB  (covers Kitty graphics chunks)

/// Parsed CSI/DCS parameter list with subparameter (colon `:`) support (e.g. `4:3`, `38:2::R:G:B`).
struct CsiParams {
    struct Param {
        int32_t value{-1}; // -1 = omitted / default
        std::array<int32_t, kMaxSubParams> subs{};
        uint8_t sub_count{0};
    };

    std::array<Param, kMaxParams> items{};
    uint8_t count{0};
    uint8_t private_marker{0}; // '?', '>', '<', '=' if present as first byte
    std::array<uint8_t, kMaxIntermediates> intermediates{};
    uint8_t intermediate_count{0};
    bool overflow{false};

    void clear() noexcept {
        count = 0;
        private_marker = 0;
        intermediate_count = 0;
        overflow = false;
        items[0] = Param{};
    }

    [[nodiscard]] int32_t get(size_t idx, int32_t default_val = 0) const noexcept {
        if (idx >= count || items[idx].value < 0) return default_val;
        return items[idx].value;
    }

    [[nodiscard]] int32_t get_or_1(size_t idx) const noexcept {
        const int32_t v = get(idx, 1);
        return v <= 0 ? 1 : v;
    }

    [[nodiscard]] bool has_subparams(size_t idx) const noexcept {
        return idx < count && items[idx].sub_count > 0;
    }

    [[nodiscard]] int32_t sub(size_t idx, size_t sub_idx, int32_t default_val = 0) const noexcept {
        if (idx >= count || sub_idx >= items[idx].sub_count) return default_val;
        const int32_t v = items[idx].subs[sub_idx];
        return v < 0 ? default_val : v;
    }
};

/// Abstract sink receiving zero-alloc parsed VT events directly from Parser.
class VtHandler {
public:
    virtual ~VtHandler() = default;

    virtual void on_print(char32_t cp) = 0;
    virtual void on_print_ascii(const uint8_t* data, size_t len) = 0;
    virtual void on_execute(uint8_t c0) = 0;
    virtual void on_esc(uint8_t final_byte, const uint8_t* intermediates, uint8_t intermediate_count) = 0;
    virtual void on_csi(uint8_t final_byte, const CsiParams& params) = 0;
    virtual void on_osc(std::string_view payload) = 0;
    virtual void on_dcs(uint8_t final_byte, const CsiParams& params, std::string_view payload) = 0;
    virtual void on_apc(std::string_view payload) = 0;
};

/// Complete Paul Williams DEC ANSI VT500 State Machine.
class Parser {
public:
    enum class State : uint8_t {
        Ground = 0,
        Escape,
        EscapeIntermediate,
        CsiEntry,
        CsiParam,
        CsiIntermediate,
        CsiIgnore,
        DcsEntry,
        DcsParam,
        DcsIntermediate,
        DcsPassthrough,
        DcsIgnore,
        OscString,
        SosPmApcString,
    };

    Parser();

    void reset() noexcept;
    [[nodiscard]] State state() const noexcept { return state_; }

    /// Feeds raw PTY bytes directly into the state machine and dispatches to `handler`.
    void feed(const uint8_t* data, size_t len, VtHandler& handler);
    void feed(std::string_view bytes, VtHandler& handler) {
        feed(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), handler);
    }

private:
    void enter_state(State s) noexcept;
    void clear_params() noexcept;
    void collect_intermediate(uint8_t b) noexcept;
    void collect_param_byte(uint8_t b) noexcept;
    void finish_param_list() noexcept;

    State state_{State::Ground};
    Utf8Decoder utf8_{};

    CsiParams params_{};
    bool in_subparam_{false};
    bool param_started_{false};

    // OSC / DCS / APC string accumulators (reused capacity, capped by security bounds)
    std::string string_buf_;
    bool string_overflow_{false};
    bool string_esc_seen_{false}; // Detecting ST = ESC '\'
    uint8_t dcs_final_{0};
    bool is_apc_{false}; // True if SosPmApcString was entered via ESC '_' (APC)
};

} // namespace vater::vt

