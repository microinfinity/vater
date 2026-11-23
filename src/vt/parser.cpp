/// @file parser.cpp
/// @brief L3 Terminal Core — Full Paul Williams DEC ANSI VT500 State Machine implementation.

#include "vt/parser.hpp"

#include <algorithm>

namespace vater::vt {

namespace {

inline bool is_c0_anywhere(uint8_t b) noexcept {
    // 0x18 (CAN), 0x1A (SUB), 0x1B (ESC) interrupt any state immediately
    return b == 0x18u || b == 0x1Au || b == 0x1Bu;
}

inline bool is_c0_exec(uint8_t b) noexcept {
    return b <= 0x17u || b == 0x19u || (b >= 0x1Cu && b <= 0x1Fu);
}

} // namespace

Parser::Parser() {
    string_buf_.reserve(256);
    reset();
}

void Parser::reset() noexcept {
    state_ = State::Ground;
    utf8_.reset();
    clear_params();
    string_buf_.clear();
    string_overflow_ = false;
    string_esc_seen_ = false;
    dcs_final_ = 0;
    is_apc_ = false;
}

void Parser::clear_params() noexcept {
    params_.clear();
    in_subparam_ = false;
    param_started_ = false;
}

void Parser::enter_state(State s) noexcept {
    state_ = s;
}

void Parser::collect_intermediate(uint8_t b) noexcept {
    if (params_.intermediate_count < kMaxIntermediates) {
        params_.intermediates[params_.intermediate_count++] = b;
    } else {
        params_.overflow = true;
    }
}

void Parser::collect_param_byte(uint8_t b) noexcept {
    if (params_.overflow) return;

    if (b >= '0' && b <= '9') {
        if (!param_started_) {
            if (params_.count == 0) {
                params_.count = 1;
                params_.items[0] = CsiParams::Param{};
            }
            param_started_ = true;
        }
        auto& p = params_.items[params_.count - 1];
        const int32_t digit = static_cast<int32_t>(b - '0');
        if (!in_subparam_) {
            if (p.value < 0) p.value = 0;
            if (p.value < 1000000) {
                p.value = p.value * 10 + digit;
            }
        } else {
            if (p.sub_count > 0) {
                int32_t& sv = p.subs[p.sub_count - 1];
                if (sv < 0) sv = 0;
                if (sv < 1000000) {
                    sv = sv * 10 + digit;
                }
            }
        }
        return;
    }

    if (b == ';') {
        if (params_.count == 0) {
            // Leading ';' means first param was omitted
            params_.count = 1;
            params_.items[0] = CsiParams::Param{};
        }
        if (params_.count < kMaxParams) {
            params_.items[params_.count] = CsiParams::Param{};
            ++params_.count;
            in_subparam_ = false;
            param_started_ = true;
        } else {
            params_.overflow = true;
        }
        return;
    }

    if (b == ':') {
        if (params_.count == 0) {
            params_.count = 1;
            params_.items[0] = CsiParams::Param{};
        }
        auto& p = params_.items[params_.count - 1];
        if (p.sub_count < kMaxSubParams) {
            p.subs[p.sub_count++] = -1;
            in_subparam_ = true;
            param_started_ = true;
        }
    }
}

void Parser::finish_param_list() noexcept {
    // Nothing extra needed; count reflects active parameters.
}

void Parser::feed(const uint8_t* data, size_t len, VtHandler& handler) {
    size_t i = 0;
    while (i < len) {
        // SIMD Fast-Path: when in Ground and not mid-UTF8 sequence, scan contiguous ASCII
        if (state_ == State::Ground && !utf8_.in_sequence()) [[likely]] {
            const size_t ascii_len = scan_printable_ascii(data + i, len - i);
            if (ascii_len > 0) {
                handler.on_print_ascii(data + i, ascii_len);
                i += ascii_len;
                if (i >= len) break;
            }
        }

        const uint8_t b = data[i++];

        // ── String-accumulating states with ESC '\' (ST) detection ──
        if (state_ == State::OscString || state_ == State::DcsPassthrough || state_ == State::SosPmApcString) {
            if (string_esc_seen_) {
                string_esc_seen_ = false;
                if (b == '\\') {
                    // ST (String Terminator = ESC \)
                    if (!string_overflow_) {
                        if (state_ == State::OscString) {
                            handler.on_osc(string_buf_);
                        } else if (state_ == State::DcsPassthrough) {
                            handler.on_dcs(dcs_final_, params_, string_buf_);
                        } else if (is_apc_) {
                            handler.on_apc(string_buf_);
                        }
                    }
                    string_buf_.clear();
                    enter_state(State::Ground);
                    continue;
                }
                // ESC followed by something else aborts string and enters Escape state
                string_buf_.clear();
                clear_params();
                enter_state(State::Escape);
                // Fall through to process `b` in Escape state below
            } else {
                if (b == 0x1Bu) { // ESC
                    string_esc_seen_ = true;
                    continue;
                }
                if (b == 0x18u || b == 0x1Au) { // CAN / SUB abort
                    string_buf_.clear();
                    enter_state(State::Ground);
                    handler.on_execute(b);
                    continue;
                }
                if (state_ == State::OscString && b == 0x07u) { // BEL terminates OSC
                    if (!string_overflow_) {
                        handler.on_osc(string_buf_);
                    }
                    string_buf_.clear();
                    enter_state(State::Ground);
                    continue;
                }
                if (b == 0x9Cu) { // 8-bit ST (only when not mid-UTF8 in OSC; safe terminator)
                    // Note: in UTF-8 streams 0x9C is a valid continuation byte!
                    // Do NOT treat raw 0x9C as C1 ST inside UTF-8 OSC strings.
                }
                const size_t cap = (state_ == State::OscString)
                                       ? kMaxOscBytes
                                       : ((state_ == State::DcsPassthrough) ? kMaxDcsBytes : kMaxApcBytes);
                if (!string_overflow_) {
                    if (string_buf_.size() < cap) {
                        string_buf_.push_back(static_cast<char>(b));
                    } else {
                        string_overflow_ = true;
                        string_buf_.clear();
                    }
                }
                continue;
            }
        }

        // ── Anywhere transitions (Paul Williams table) ──
        if (b == 0x18u || b == 0x1Au) { // CAN / SUB
            utf8_.reset();
            enter_state(State::Ground);
            handler.on_execute(b);
            continue;
        }
        if (b == 0x1Bu) { // ESC
            utf8_.reset();
            clear_params();
            enter_state(State::Escape);
            continue;
        }

        switch (state_) {
            case State::Ground: {
                if (utf8_.in_sequence()) {
                    if (b < 0x80u) {
                        // Interrupted UTF-8 sequence: emit U+FFFD and re-process `b`
                        utf8_.reset();
                        handler.on_print(0xFFFDu);
                        --i;
                        continue;
                    }
                    char32_t cp = 0;
                    const int res = utf8_.decode(b, cp);
                    if (res == 1) {
                        handler.on_print(cp);
                    } else if (res < 0) {
                        handler.on_print(0xFFFDu);
                    }
                    continue;
                }

                if (b < 0x20u) {
                    handler.on_execute(b);
                } else if (b < 0x7Fu) {
                    handler.on_print(static_cast<char32_t>(b));
                } else if (b == 0x7Fu) {
                    // DEL ignored in ground
                } else {
                    char32_t cp = 0;
                    const int res = utf8_.decode(b, cp);
                    if (res == 1) {
                        handler.on_print(cp);
                    } else if (res < 0) {
                        handler.on_print(0xFFFDu);
                    }
                }
                break;
            }

            case State::Escape: {
                if (is_c0_exec(b)) {
                    handler.on_execute(b);
                } else if (b == 0x7Fu) {
                    // ignore DEL
                } else if (b >= 0x20u && b <= 0x2Fu) {
                    collect_intermediate(b);
                    enter_state(State::EscapeIntermediate);
                } else if (b == '[') {
                    clear_params();
                    enter_state(State::CsiEntry);
                } else if (b == ']') {
                    string_buf_.clear();
                    string_overflow_ = false;
                    string_esc_seen_ = false;
                    enter_state(State::OscString);
                } else if (b == 'P') {
                    clear_params();
                    string_buf_.clear();
                    string_overflow_ = false;
                    string_esc_seen_ = false;
                    dcs_final_ = 0;
                    enter_state(State::DcsEntry);
                } else if (b == '_' || b == 'X' || b == '^') {
                    // APC ('_'), SOS ('X'), PM ('^')
                    string_buf_.clear();
                    string_overflow_ = false;
                    string_esc_seen_ = false;
                    is_apc_ = (b == '_');
                    enter_state(State::SosPmApcString);
                } else if (b >= 0x30u && b <= 0x7Eu) {
                    handler.on_esc(b, params_.intermediates.data(), params_.intermediate_count);
                    enter_state(State::Ground);
                } else {
                    enter_state(State::Ground);
                }
                break;
            }

            case State::EscapeIntermediate: {
                if (is_c0_exec(b)) {
                    handler.on_execute(b);
                } else if (b == 0x7Fu) {
                    // ignore
                } else if (b >= 0x20u && b <= 0x2Fu) {
                    collect_intermediate(b);
                } else if (b >= 0x30u && b <= 0x7Eu) {
                    handler.on_esc(b, params_.intermediates.data(), params_.intermediate_count);
                    enter_state(State::Ground);
                } else {
                    enter_state(State::Ground);
                }
                break;
            }

            case State::CsiEntry: {
                if (is_c0_exec(b)) {
                    handler.on_execute(b);
                } else if (b == 0x7Fu) {
                    // ignore
                } else if (b >= 0x20u && b <= 0x2Fu) {
                    collect_intermediate(b);
                    enter_state(State::CsiIntermediate);
                } else if ((b >= '0' && b <= '9') || b == ';' || b == ':') {
                    collect_param_byte(b);
                    enter_state(State::CsiParam);
                } else if (b >= 0x3Cu && b <= 0x3Fu) {
                    // Private markers: '<', '=', '>', '?'
                    params_.private_marker = b;
                    enter_state(State::CsiParam);
                } else if (b >= 0x40u && b <= 0x7Eu) {
                    finish_param_list();
                    handler.on_csi(b, params_);
                    enter_state(State::Ground);
                } else {
                    enter_state(State::CsiIgnore);
                }
                break;
            }

            case State::CsiParam: {
                if (is_c0_exec(b)) {
                    handler.on_execute(b);
                } else if (b == 0x7Fu) {
                    // ignore
                } else if ((b >= '0' && b <= '9') || b == ';' || b == ':') {
                    collect_param_byte(b);
                } else if (b >= 0x20u && b <= 0x2Fu) {
                    collect_intermediate(b);
                    enter_state(State::CsiIntermediate);
                } else if (b >= 0x3Cu && b <= 0x3Fu) {
                    enter_state(State::CsiIgnore);
                } else if (b >= 0x40u && b <= 0x7Eu) {
                    finish_param_list();
                    handler.on_csi(b, params_);
                    enter_state(State::Ground);
                } else {
                    enter_state(State::CsiIgnore);
                }
                break;
            }

            case State::CsiIntermediate: {
                if (is_c0_exec(b)) {
                    handler.on_execute(b);
                } else if (b == 0x7Fu) {
                    // ignore
                } else if (b >= 0x20u && b <= 0x2Fu) {
                    collect_intermediate(b);
                } else if (b >= 0x30u && b <= 0x3Fu) {
                    enter_state(State::CsiIgnore);
                } else if (b >= 0x40u && b <= 0x7Eu) {
                    finish_param_list();
                    handler.on_csi(b, params_);
                    enter_state(State::Ground);
                } else {
                    enter_state(State::CsiIgnore);
                }
                break;
            }

            case State::CsiIgnore: {
                if (is_c0_exec(b)) {
                    handler.on_execute(b);
                } else if (b >= 0x40u && b <= 0x7Eu) {
                    enter_state(State::Ground);
                }
                break;
            }

            case State::DcsEntry: {
                if (is_c0_exec(b) || b == 0x7Fu) {
                    // ignored per Paul Williams table
                } else if (b >= 0x20u && b <= 0x2Fu) {
                    collect_intermediate(b);
                    enter_state(State::DcsIntermediate);
                } else if ((b >= '0' && b <= '9') || b == ';' || b == ':') {
                    collect_param_byte(b);
                    enter_state(State::DcsParam);
                } else if (b >= 0x3Cu && b <= 0x3Fu) {
                    params_.private_marker = b;
                    enter_state(State::DcsParam);
                } else if (b >= 0x40u && b <= 0x7Eu) {
                    dcs_final_ = b;
                    enter_state(State::DcsPassthrough);
                } else {
                    enter_state(State::DcsIgnore);
                }
                break;
            }

            case State::DcsParam: {
                if (is_c0_exec(b) || b == 0x7Fu) {
                    // ignored
                } else if ((b >= '0' && b <= '9') || b == ';' || b == ':') {
                    collect_param_byte(b);
                } else if (b >= 0x20u && b <= 0x2Fu) {
                    collect_intermediate(b);
                    enter_state(State::DcsIntermediate);
                } else if (b >= 0x3Cu && b <= 0x3Fu) {
                    enter_state(State::DcsIgnore);
                } else if (b >= 0x40u && b <= 0x7Eu) {
                    dcs_final_ = b;
                    enter_state(State::DcsPassthrough);
                } else {
                    enter_state(State::DcsIgnore);
                }
                break;
            }

            case State::DcsIntermediate: {
                if (is_c0_exec(b) || b == 0x7Fu) {
                    // ignored
                } else if (b >= 0x20u && b <= 0x2Fu) {
                    collect_intermediate(b);
                } else if (b >= 0x30u && b <= 0x3Fu) {
                    enter_state(State::DcsIgnore);
                } else if (b >= 0x40u && b <= 0x7Eu) {
                    dcs_final_ = b;
                    enter_state(State::DcsPassthrough);
                } else {
                    enter_state(State::DcsIgnore);
                }
                break;
            }

            case State::DcsIgnore: {
                // Consume until ESC \ (handled above on ESC -> Escape state)
                break;
            }

            case State::DcsPassthrough:
            case State::OscString:
            case State::SosPmApcString:
                // Handled at top of loop
                break;
        }
    }
}

} // namespace vater::vt

