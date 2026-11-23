/// @file search_overlay.cpp
/// @brief L5 UI — Thompson NFA Regex Engine & Scrollback Search Overlay implementation.

#include "ui/search_overlay.hpp"

#include "nisaba/canvas/paint.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace vater::ui {

namespace {

uint8_t fold_byte(uint8_t b, bool case_sensitive) noexcept {
    if (!case_sensitive && b >= 'A' && b <= 'Z') return static_cast<uint8_t>(b + ('a' - 'A'));
    return b;
}

void set_bit(uint8_t bitmap[32], uint8_t b) noexcept {
    bitmap[b >> 3] |= static_cast<uint8_t>(1u << (b & 7u));
}

bool test_bit(const uint8_t bitmap[32], uint8_t b) noexcept {
    return (bitmap[b >> 3] & (1u << (b & 7u))) != 0;
}

} // namespace

bool ThompsonRegex::compile(std::string_view pattern, bool case_sensitive) {
    states_.clear();
    start_state_ = -1;
    case_sensitive_ = case_sensitive;
    valid_ = false;
    if (pattern.empty()) return false;

    // Recursive-descent parser emitting Thompson NFA fragments (start, dangling out-patches)
    struct Frag {
        int start{-1};
        std::vector<int*> outs; // Stored as indices into states_ + slot (0=out1, 1=out2)
        std::vector<std::pair<int, int>> out_slots;
    };

    auto add_state = [&](State s) -> int {
        const int id = static_cast<int>(states_.size());
        states_.push_back(s);
        return id;
    };

    auto patch = [&](const std::vector<std::pair<int, int>>& slots, int target) {
        for (auto [sid, which] : slots) {
            if (which == 0) states_[static_cast<size_t>(sid)].out1 = target;
            else states_[static_cast<size_t>(sid)].out2 = target;
        }
    };

    size_t pos = 0;

    std::function<Frag()> parse_alt;
    std::function<Frag()> parse_concat;
    std::function<Frag()> parse_quant;
    std::function<Frag()> parse_atom;

    auto make_escape_class = [&](char esc) -> State {
        State st{};
        st.op = Op::Class;
        if (esc == 'd' || esc == 'D') {
            for (int c = '0'; c <= '9'; ++c) set_bit(st.char_bitmap, static_cast<uint8_t>(c));
            st.negated = (esc == 'D');
        } else if (esc == 'w' || esc == 'W') {
            for (int c = '0'; c <= '9'; ++c) set_bit(st.char_bitmap, static_cast<uint8_t>(c));
            for (int c = 'a'; c <= 'z'; ++c) set_bit(st.char_bitmap, static_cast<uint8_t>(c));
            for (int c = 'A'; c <= 'Z'; ++c) set_bit(st.char_bitmap, static_cast<uint8_t>(c));
            set_bit(st.char_bitmap, '_');
            st.negated = (esc == 'W');
        } else if (esc == 's' || esc == 'S') {
            for (char c : std::string_view(" \t\r\n\f\v")) set_bit(st.char_bitmap, static_cast<uint8_t>(c));
            st.negated = (esc == 'S');
        } else {
            st.op = Op::Char;
            st.ch = fold_byte(static_cast<uint8_t>(esc), case_sensitive_);
        }
        return st;
    };

    parse_atom = [&]() -> Frag {
        if (pos >= pattern.size()) return Frag{};
        const char ch = pattern[pos];

        if (ch == '(') {
            ++pos;
            Frag inner = parse_alt();
            if (pos < pattern.size() && pattern[pos] == ')') ++pos;
            return inner;
        }

        if (ch == '[') {
            ++pos;
            State st{};
            st.op = Op::Class;
            if (pos < pattern.size() && pattern[pos] == '^') {
                st.negated = true;
                ++pos;
            }
            bool first = true;
            while (pos < pattern.size() && (first || pattern[pos] != ']')) {
                first = false;
                uint8_t lo = static_cast<uint8_t>(pattern[pos++]);
                if (lo == '\\' && pos < pattern.size()) lo = static_cast<uint8_t>(pattern[pos++]);
                if (pos + 1 < pattern.size() && pattern[pos] == '-' && pattern[pos + 1] != ']') {
                    ++pos;
                    uint8_t hi = static_cast<uint8_t>(pattern[pos++]);
                    if (hi == '\\' && pos < pattern.size()) hi = static_cast<uint8_t>(pattern[pos++]);
                    if (lo > hi) std::swap(lo, hi);
                    for (uint32_t c = lo; c <= hi; ++c) {
                        set_bit(st.char_bitmap, fold_byte(static_cast<uint8_t>(c), case_sensitive_));
                    }
                } else {
                    set_bit(st.char_bitmap, fold_byte(lo, case_sensitive_));
                }
            }
            if (pos < pattern.size() && pattern[pos] == ']') ++pos;
            int id = add_state(st);
            return Frag{id, {}, {{id, 0}}};
        }

        ++pos;
        if (ch == '.') {
            State st{}; st.op = Op::Any;
            int id = add_state(st);
            return Frag{id, {}, {{id, 0}}};
        }
        if (ch == '^') {
            State st{}; st.op = Op::AnchorStart;
            int id = add_state(st);
            return Frag{id, {}, {{id, 0}}};
        }
        if (ch == '$') {
            State st{}; st.op = Op::AnchorEnd;
            int id = add_state(st);
            return Frag{id, {}, {{id, 0}}};
        }
        if (ch == '\\' && pos < pattern.size()) {
            State st = make_escape_class(pattern[pos++]);
            int id = add_state(st);
            return Frag{id, {}, {{id, 0}}};
        }

        State st{};
        st.op = Op::Char;
        st.ch = fold_byte(static_cast<uint8_t>(ch), case_sensitive_);
        int id = add_state(st);
        return Frag{id, {}, {{id, 0}}};
    };

    parse_quant = [&]() -> Frag {
        Frag f = parse_atom();
        if (f.start < 0) return f;
        while (pos < pattern.size()) {
            const char q = pattern[pos];
            if (q == '*') {
                ++pos;
                State sp{}; sp.op = Op::Split; sp.out1 = f.start;
                int sid = add_state(sp);
                patch(f.out_slots, sid);
                f.start = sid;
                f.out_slots = {{sid, 1}};
            } else if (q == '+') {
                ++pos;
                State sp{}; sp.op = Op::Split; sp.out1 = f.start;
                int sid = add_state(sp);
                patch(f.out_slots, sid);
                f.out_slots = {{sid, 1}};
            } else if (q == '?') {
                ++pos;
                State sp{}; sp.op = Op::Split; sp.out1 = f.start;
                int sid = add_state(sp);
                f.start = sid;
                f.out_slots.push_back({sid, 1});
            } else {
                break;
            }
        }
        return f;
    };

    parse_concat = [&]() -> Frag {
        Frag res{};
        while (pos < pattern.size() && pattern[pos] != '|' && pattern[pos] != ')') {
            Frag next = parse_quant();
            if (next.start < 0) break;
            if (res.start < 0) {
                res = std::move(next);
            } else {
                patch(res.out_slots, next.start);
                res.out_slots = std::move(next.out_slots);
            }
        }
        return res;
    };

    parse_alt = [&]() -> Frag {
        Frag left = parse_concat();
        while (pos < pattern.size() && pattern[pos] == '|') {
            ++pos;
            Frag right = parse_concat();
            if (left.start < 0) {
                left = std::move(right);
            } else if (right.start >= 0) {
                State sp{}; sp.op = Op::Split; sp.out1 = left.start; sp.out2 = right.start;
                int sid = add_state(sp);
                left.start = sid;
                left.out_slots.insert(left.out_slots.end(), right.out_slots.begin(), right.out_slots.end());
            }
        }
        return left;
    };

    Frag root = parse_alt();
    if (root.start < 0) return false;

    State match_st{};
    match_st.op = Op::Match;
    int match_id = add_state(match_st);
    patch(root.out_slots, match_id);

    start_state_ = root.start;
    valid_ = true;
    return true;
}

void ThompsonRegex::add_epsilon(int s, std::vector<int>& list, std::vector<uint32_t>& visited,
                                uint32_t gen, size_t pos, size_t text_len) const {
    if (s < 0 || visited[static_cast<size_t>(s)] == gen) return;
    visited[static_cast<size_t>(s)] = gen;
    const State& st = states_[static_cast<size_t>(s)];
    if (st.op == Op::Split) {
        add_epsilon(st.out1, list, visited, gen, pos, text_len);
        add_epsilon(st.out2, list, visited, gen, pos, text_len);
    } else if (st.op == Op::AnchorStart) {
        if (pos == 0) add_epsilon(st.out1, list, visited, gen, pos, text_len);
    } else if (st.op == Op::AnchorEnd) {
        if (pos == text_len) add_epsilon(st.out1, list, visited, gen, pos, text_len);
    } else {
        list.push_back(s);
    }
}

bool ThompsonRegex::match_at(std::string_view text, size_t start, size_t& out_end) const {
    if (!valid_ || start_state_ < 0) return false;

    std::vector<int> cur, next;
    std::vector<uint32_t> visited(states_.size(), 0);
    uint32_t gen = 1;

    add_epsilon(start_state_, cur, visited, gen++, start, text.size());

    bool found = false;
    size_t best_end = start;

    for (size_t i = start; i <= text.size(); ++i) {
        for (int s : cur) {
            if (states_[static_cast<size_t>(s)].op == Op::Match) {
                found = true;
                best_end = i;
            }
        }
        if (i == text.size() || cur.empty()) break;

        next.clear();
        const uint8_t b = fold_byte(static_cast<uint8_t>(text[i]), case_sensitive_);
        const uint32_t step_gen = gen++;

        for (int s : cur) {
            const State& st = states_[static_cast<size_t>(s)];
            bool ok = false;
            if (st.op == Op::Char) ok = (b == st.ch);
            else if (st.op == Op::Any) ok = (b != '\n');
            else if (st.op == Op::Class) {
                bool in = test_bit(st.char_bitmap, b);
                ok = st.negated ? !in : in;
            }
            if (ok) {
                add_epsilon(st.out1, next, visited, step_gen, i + 1, text.size());
            }
        }
        cur.swap(next);
    }

    if (found) {
        out_end = best_end;
        return true;
    }
    return false;
}

void ThompsonRegex::find_all(std::string_view text, std::vector<std::pair<size_t, size_t>>& out_matches) const {
    out_matches.clear();
    if (!valid_) return;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = pos;
        if (match_at(text, pos, end) && end > pos) {
            out_matches.emplace_back(pos, end);
            pos = end;
        } else {
            ++pos;
        }
    }
}

// ── SearchOverlay UI ──

void SearchOverlay::open() {
    open_ = true;
}

void SearchOverlay::close() {
    open_ = false;
    matches_.clear();
    active_idx_ = -1;
}

void SearchOverlay::run_search(const vt::Terminal& terminal) {
    matches_.clear();
    active_idx_ = -1;
    if (query_.empty()) return;

    std::vector<std::string> lines;
    terminal.dump_all_lines_utf8(lines, last_scrollback_count_);

    bool use_nfa = regex_mode_ && regex_.compile(query_, case_sensitive_);
    std::vector<std::pair<size_t, size_t>> byte_spans;

    // Lowercased literal needle fallback
    std::string needle = query_;
    if (!case_sensitive_) {
        for (char& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    for (size_t r = 0; r < lines.size(); ++r) {
        const std::string& line = lines[r];
        if (use_nfa) {
            regex_.find_all(line, byte_spans);
            for (auto [b0, b1] : byte_spans) {
                // Convert UTF-8 byte offsets to approximate column indices
                int c0 = 0, c1 = 0;
                for (size_t i = 0; i < b0 && i < line.size(); ++i) {
                    if ((static_cast<uint8_t>(line[i]) & 0xC0u) != 0x80u) ++c0;
                }
                c1 = c0;
                for (size_t i = b0; i < b1 && i < line.size(); ++i) {
                    if ((static_cast<uint8_t>(line[i]) & 0xC0u) != 0x80u) ++c1;
                }
                matches_.push_back(vt::SearchMatch{static_cast<int>(r), c0, std::max(c0 + 1, c1)});
            }
        } else {
            std::string hay = line;
            if (!case_sensitive_) {
                for (char& c : hay) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            size_t p = 0;
            while ((p = hay.find(needle, p)) != std::string::npos) {
                matches_.push_back(vt::SearchMatch{
                    static_cast<int>(r),
                    static_cast<int>(p),
                    static_cast<int>(p + needle.size())
                });
                p += std::max<size_t>(1, needle.size());
            }
        }
        if (matches_.size() >= 10000) break;
    }

    if (!matches_.empty()) {
        active_idx_ = static_cast<int>(matches_.size()) - 1; // Jump to most recent match
    }
}

void SearchOverlay::scroll_to_active(vt::Terminal& terminal) {
    if (active_idx_ < 0 || active_idx_ >= static_cast<int>(matches_.size())) return;
    const int abs_row = matches_[static_cast<size_t>(active_idx_)].abs_row;
    const int sb = static_cast<int>(last_scrollback_count_);
    const int view_rows = terminal.rows();
    if (abs_row < sb) {
        const int target_offset = std::max(0, sb - abs_row + view_rows / 3);
        terminal.scroll_to_offset(target_offset);
    } else {
        terminal.scroll_to_bottom();
    }
}

void SearchOverlay::next_match(vt::Terminal& terminal) {
    if (matches_.empty()) return;
    active_idx_ = (active_idx_ + 1) % static_cast<int>(matches_.size());
    scroll_to_active(terminal);
}

void SearchOverlay::prev_match(vt::Terminal& terminal) {
    if (matches_.empty()) return;
    active_idx_ = (active_idx_ - 1 + static_cast<int>(matches_.size())) % static_cast<int>(matches_.size());
    scroll_to_active(terminal);
}

bool SearchOverlay::handle_key(int keysym, int mods, vt::Terminal& terminal) {
    if (!open_) return false;
    if (keysym == 0xFF1B) { // Escape
        close();
        return true;
    }
    if (keysym == 0xFF0D) { // Enter
        if (mods & 1) prev_match(terminal);
        else next_match(terminal);
        return true;
    }
    if (keysym == 0xFF08) { // BackSpace
        if (!query_.empty()) {
            // Pop full UTF-8 codepoint
            while (!query_.empty() && (static_cast<uint8_t>(query_.back()) & 0xC0u) == 0x80u) {
                query_.pop_back();
            }
            if (!query_.empty()) query_.pop_back();
            run_search(terminal);
            scroll_to_active(terminal);
        }
        return true;
    }
    if ((mods & 4) && (keysym == 'r' || keysym == 'R')) { // Alt+R toggle regex
        regex_mode_ = !regex_mode_;
        run_search(terminal);
        return true;
    }
    if ((mods & 4) && (keysym == 'c' || keysym == 'C')) { // Alt+C toggle case sensitivity
        case_sensitive_ = !case_sensitive_;
        run_search(terminal);
        return true;
    }
    return true;
}

bool SearchOverlay::handle_text(std::string_view utf8, vt::Terminal& terminal) {
    if (!open_) return false;
    if (utf8.empty() || (utf8.size() == 1 && static_cast<uint8_t>(utf8[0]) < 0x20u)) return true;
    query_.append(utf8);
    run_search(terminal);
    scroll_to_active(terminal);
    return true;
}

void SearchOverlay::render(nisaba::gpu::GpuCanvas& canvas, float width, float titlebar_h,
                           const obsidian::ui::AppTheme& theme, const obsidian::ui::UnitScale& scale) {
    if (!open_) return;

    const float u = scale.u;
    const float bar_w = std::min(width - 32.0f * u, 360.0f * u);
    const float bar_h = 38.0f * u;
    const float bx = width - bar_w - 14.0f * u;
    const float by = titlebar_h + 10.0f * u;

    auto rect = nisaba::Rect::from_xywh(bx, by, bar_w, bar_h);
    if (!rect) return;

    const nisaba::Color bg = obsidian::ui::detail::with_alpha(theme.secondBackground, 0.94f);
    canvas.fill_round_rect(*rect, 10.0f * u, 10.0f * u, nisaba::Paint(bg));
    nisaba::Stroke st;
    st.width = 1.0f;
    canvas.stroke_round_rect(*rect, 10.0f * u, 10.0f * u, nisaba::Paint(theme.highlight), st);

    const std::string display = query_.empty() ? "Search (regex)..." : query_;
    const nisaba::Color text_col = query_.empty() ? theme.textFieldPlaceholder : theme.text;
    canvas.draw_text(bx + 12.0f * u, by + 24.0f * u, display, nisaba::Paint(text_col), 13.0f * u, "sans");

    std::string status = (regex_mode_ ? ".*" : "Aa") + std::string("  ");
    if (matches_.empty()) status += "0/0";
    else status += std::to_string(active_idx_ + 1) + "/" + std::to_string(matches_.size());
    canvas.draw_text(bx + bar_w - 92.0f * u, by + 24.0f * u, status,
                     nisaba::Paint(theme.disabledText), 11.5f * u, "sans");
}

} // namespace vater::ui

