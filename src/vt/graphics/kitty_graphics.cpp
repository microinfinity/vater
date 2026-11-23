/// @file kitty_graphics.cpp
/// @brief L3 Terminal Core — Kitty Graphics Protocol (APC G) implementation (o=z, t=d/f/t/s, f=24/32/100).

#include "vt/graphics/kitty_graphics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vater::vt {

namespace {

int parse_int_sv(std::string_view sv) noexcept {
    if (sv.empty()) return 0;
    bool neg = false;
    size_t i = 0;
    if (sv[0] == '-') { neg = true; i = 1; }
    int v = 0;
    for (; i < sv.size(); ++i) {
        if (sv[i] < '0' || sv[i] > '9') break;
        if (v < 100000000) v = v * 10 + (sv[i] - '0');
    }
    return neg ? -v : v;
}

void compute_cursor_advance(const DecodedImage& img,
                            int req_cols, int req_rows,
                            int cell_px_w, int cell_px_h,
                            int& out_rows, int& out_cols) noexcept {
    cell_px_w = std::max(1, cell_px_w);
    cell_px_h = std::max(1, cell_px_h);
    int eff_cols = req_cols;
    int eff_rows = req_rows;

    if (eff_cols > 0 && eff_rows == 0 && img.width > 0 && img.height > 0) {
        const float px_w = static_cast<float>(eff_cols * cell_px_w);
        const float px_h = px_w * (static_cast<float>(img.height) / static_cast<float>(img.width));
        eff_rows = std::max(1, static_cast<int>(std::ceil(px_h / static_cast<float>(cell_px_h))));
    } else if (eff_rows > 0 && eff_cols == 0 && img.width > 0 && img.height > 0) {
        const float px_h = static_cast<float>(eff_rows * cell_px_h);
        const float px_w = px_h * (static_cast<float>(img.width) / static_cast<float>(img.height));
        eff_cols = std::max(1, static_cast<int>(std::ceil(px_w / static_cast<float>(cell_px_w))));
    } else if (eff_cols == 0 && eff_rows == 0) {
        eff_cols = std::max(1, static_cast<int>((img.width + static_cast<uint32_t>(cell_px_w) - 1u) / static_cast<uint32_t>(cell_px_w)));
        eff_rows = std::max(1, static_cast<int>((img.height + static_cast<uint32_t>(cell_px_h) - 1u) / static_cast<uint32_t>(cell_px_h)));
    }

    // Per Kitty Graphics spec: cursor moves down (rows - 1) and right (cols) when C=0
    out_rows = std::max(0, eff_rows - 1);
    out_cols = std::max(0, eff_cols);
}

} // namespace

bool GraphicsStore::decode_base64(std::string_view in, std::vector<uint8_t>& out) {
    static constexpr std::array<int8_t, 256> kDec = []() {
        std::array<int8_t, 256> t{};
        t.fill(-1);
        for (int i = 'A'; i <= 'Z'; ++i) t[static_cast<size_t>(i)] = static_cast<int8_t>(i - 'A');
        for (int i = 'a'; i <= 'z'; ++i) t[static_cast<size_t>(i)] = static_cast<int8_t>(26 + i - 'a');
        for (int i = '0'; i <= '9'; ++i) t[static_cast<size_t>(i)] = static_cast<int8_t>(52 + i - '0');
        t['+'] = 62;
        t['/'] = 63;
        t['-'] = 62;
        t['_'] = 63;
        return t;
    }();

    out.reserve(out.size() + (in.size() * 3) / 4 + 4);
    uint32_t acc = 0;
    int bits = 0;
    for (char ch : in) {
        if (ch == '=' || ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') continue;
        const int8_t val = kDec[static_cast<uint8_t>(ch)];
        if (val < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(val);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFFu));
        }
    }
    return true;
}

std::string GraphicsStore::encode_base64(const uint8_t* data, size_t len) {
    static constexpr char kEnc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8) |
                           static_cast<uint32_t>(data[i + 2]);
        out.push_back(kEnc[(v >> 18) & 63]);
        out.push_back(kEnc[(v >> 12) & 63]);
        out.push_back(kEnc[(v >> 6) & 63]);
        out.push_back(kEnc[v & 63]);
        i += 3;
    }
    if (i < len) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) v |= static_cast<uint32_t>(data[i + 1]) << 8;
        out.push_back(kEnc[(v >> 18) & 63]);
        out.push_back(kEnc[(v >> 12) & 63]);
        out.push_back((i + 1 < len) ? kEnc[(v >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

void GraphicsStore::evict_if_needed(size_t incoming_bytes) {
    while (total_bytes_ + incoming_bytes > kMaxTotalImageBytes && !images_.empty()) {
        uint32_t oldest = images_.begin()->first;
        for (const auto& kv : images_) {
            if (kv.first < oldest) oldest = kv.first;
        }
        remove_image(oldest);
    }
}

void GraphicsStore::remove_image(uint32_t id) {
    auto it = images_.find(id);
    if (it != images_.end()) {
        if (total_bytes_ >= it->second.rgba.size()) total_bytes_ -= it->second.rgba.size();
        else total_bytes_ = 0;
        images_.erase(it);
    }
    placements_.erase(
        std::remove_if(placements_.begin(), placements_.end(),
                       [id](const ImagePlacement& p) { return p.image_id == id; }),
        placements_.end());
    ++generation_;
}

uint32_t GraphicsStore::add_sixel_image(DecodedImage img, int cursor_row, int cursor_col, bool alt_screen) {
    if (img.width == 0 || img.height == 0 || img.rgba.empty()) return 0;
    evict_if_needed(img.rgba.size());
    const uint32_t id = next_id_++;
    img.id = id;
    img.generation = ++generation_;
    total_bytes_ += img.rgba.size();
    images_[id] = std::move(img);

    ImagePlacement pl{};
    pl.image_id = id;
    pl.placement_id = 1;
    pl.row = cursor_row;
    pl.col = cursor_col;
    pl.is_alt_screen = alt_screen;
    placements_.push_back(pl);
    ++generation_;
    return id;
}

void GraphicsStore::scroll_placements_up(int rows, bool alt_screen) {
    if (rows <= 0 || placements_.empty()) return;
    bool changed = false;
    for (auto& p : placements_) {
        if (p.is_alt_screen == alt_screen) {
            p.row -= rows;
            changed = true;
        }
    }
    placements_.erase(
        std::remove_if(placements_.begin(), placements_.end(),
                       [](const ImagePlacement& p) { return p.row < -512; }),
        placements_.end());
    if (changed) ++generation_;
}

void GraphicsStore::clear_screen_placements(bool alt_screen) {
    placements_.erase(
        std::remove_if(placements_.begin(), placements_.end(),
                       [alt_screen](const ImagePlacement& p) { return p.is_alt_screen == alt_screen; }),
        placements_.end());
    ++generation_;
}

void GraphicsStore::clear_all() {
    images_.clear();
    placements_.clear();
    chunk_ = ChunkState{};
    total_bytes_ = 0;
    ++generation_;
}

std::string GraphicsStore::handle_kitty_apc(std::string_view payload,
                                            int cursor_row,
                                            int cursor_col,
                                            int cell_px_w,
                                            int cell_px_h,
                                            bool alt_screen,
                                            int& out_cursor_advance_rows,
                                            int& out_cursor_advance_cols) {
    out_cursor_advance_rows = 0;
    out_cursor_advance_cols = 0;

    if (payload.empty() || payload[0] != 'G') return {};
    payload.remove_prefix(1);

    const size_t semi = payload.find(';');
    const std::string_view ctrl = (semi == std::string_view::npos) ? payload : payload.substr(0, semi);
    const std::string_view b64 = (semi == std::string_view::npos) ? std::string_view{} : payload.substr(semi + 1);

    char action = chunk_.active ? chunk_.action : 't';
    uint32_t fmt = chunk_.active ? chunk_.format : 32;
    char medium = chunk_.active ? chunk_.medium : 'd';
    char compression = chunk_.active ? chunk_.compression : 0;
    uint32_t id = chunk_.active ? chunk_.id : 0;
    uint32_t pid = chunk_.active ? chunk_.placement_id : 0;
    uint32_t src_w = chunk_.active ? chunk_.src_w : 0;
    uint32_t src_h = chunk_.active ? chunk_.src_h : 0;
    size_t data_size = chunk_.active ? chunk_.data_size : 0;
    size_t data_offset = chunk_.active ? chunk_.data_offset : 0;
    int cols = chunk_.active ? chunk_.cols : 0;
    int rows = chunk_.active ? chunk_.rows : 0;
    int z_index = chunk_.active ? chunk_.z_index : 0;
    int quiet = chunk_.active ? chunk_.quiet : 0;
    int more = 0;
    char del_target = 'a';
    bool suppress_cursor = chunk_.active ? chunk_.suppress_cursor_move : false;

    size_t pos = 0;
    while (pos < ctrl.size()) {
        size_t comma = ctrl.find(',', pos);
        if (comma == std::string_view::npos) comma = ctrl.size();
        std::string_view pair = ctrl.substr(pos, comma - pos);
        pos = comma + 1;

        size_t eq = pair.find('=');
        if (eq == std::string_view::npos || eq == 0) continue;
        const char k = pair[0];
        const std::string_view v = pair.substr(eq + 1);

        switch (k) {
            case 'a': if (!v.empty()) action = v[0]; break;
            case 'f': fmt = static_cast<uint32_t>(std::max(0, parse_int_sv(v))); break;
            case 't': if (!v.empty()) medium = v[0]; break;
            case 'o': if (!v.empty()) compression = v[0]; break;
            case 'i': id = static_cast<uint32_t>(std::max(0, parse_int_sv(v))); break;
            case 'p': pid = static_cast<uint32_t>(std::max(0, parse_int_sv(v))); break;
            case 's': src_w = static_cast<uint32_t>(std::max(0, parse_int_sv(v))); break;
            case 'v': src_h = static_cast<uint32_t>(std::max(0, parse_int_sv(v))); break;
            case 'S': data_size = static_cast<size_t>(std::max(0, parse_int_sv(v))); break;
            case 'O': data_offset = static_cast<size_t>(std::max(0, parse_int_sv(v))); break;
            case 'c': cols = std::max(0, parse_int_sv(v)); break;
            case 'r': rows = std::max(0, parse_int_sv(v)); break;
            case 'z': z_index = parse_int_sv(v); break;
            case 'q': quiet = parse_int_sv(v); break;
            case 'm': more = parse_int_sv(v); break;
            case 'd': if (!v.empty()) del_target = v[0]; break;
            case 'C': suppress_cursor = (parse_int_sv(v) != 0); break;
            default: break;
        }
    }

    // Per Kitty Graphics Protocol spec:
    // Only send responses when the client specified an explicit image id (`i > 0`)
    // or sent an explicit capability query (`a=q`). Tools like `fastfetch` omit `i`
    // and do not read stdin; replying on `i=0` leaks `^[_Gi=...;OK^[\` to the shell.
    auto make_reply = [&](uint32_t requested_id, uint32_t reply_id, int eff_quiet,
                          bool is_query, std::string_view msg) -> std::string {
        const bool is_ok = (msg == "OK");
        if (eff_quiet >= 2 || (eff_quiet == 1 && is_ok)) return {};
        if (!is_query && requested_id == 0) return {};
        std::string r = "\x1b_G";
        if (reply_id > 0) {
            r += "i=" + std::to_string(reply_id);
        }
        r.push_back(';');
        r.append(msg);
        r += "\x1b\\";
        return r;
    };

    // Delete action: a=d
    if (action == 'd') {
        switch (del_target) {
            case 'a': case 'A':
                clear_screen_placements(alt_screen);
                if (del_target == 'A') clear_all();
                break;
            case 'i': case 'I':
                if (id > 0) {
                    if (del_target == 'I') {
                        remove_image(id);
                    } else {
                        placements_.erase(
                            std::remove_if(placements_.begin(), placements_.end(),
                                           [id, pid](const ImagePlacement& p) {
                                               return p.image_id == id && (pid == 0 || p.placement_id == pid);
                                           }),
                            placements_.end());
                        ++generation_;
                    }
                }
                break;
            case 'c': case 'C':
                placements_.erase(
                    std::remove_if(placements_.begin(), placements_.end(),
                                   [cursor_row, cursor_col, alt_screen](const ImagePlacement& p) {
                                       return p.is_alt_screen == alt_screen && p.row == cursor_row && p.col == cursor_col;
                                   }),
                    placements_.end());
                ++generation_;
                break;
            default:
                break;
        }
        return {};
    }

    // Place existing image: a=p
    if (action == 'p') {
        auto it = images_.find(id);
        if (id == 0 || it == images_.end()) {
            return make_reply(id, id, quiet, false, "ENOENT:image not found");
        }
        ImagePlacement pl{};
        pl.image_id = id;
        pl.placement_id = pid ? pid : 1;
        pl.row = cursor_row;
        pl.col = cursor_col;
        pl.cols = cols;
        pl.rows = rows;
        pl.z_index = z_index;
        pl.is_alt_screen = alt_screen;
        placements_.push_back(pl);
        ++generation_;
        if (!suppress_cursor) {
            compute_cursor_advance(it->second, cols, rows, cell_px_w, cell_px_h,
                                   out_cursor_advance_rows, out_cursor_advance_cols);
        }
        return make_reply(id, id, quiet, false, "OK");
    }

    // Query support: a=q
    if (action == 'q') {
        return make_reply(id, id, quiet, true, "OK");
    }

    // Transmit (a=t) or Transmit & Display (a=T)
    if (!chunk_.active) {
        chunk_ = ChunkState{};
        chunk_.active = true;
        chunk_.action = action;
        chunk_.format = fmt;
        chunk_.medium = medium;
        chunk_.compression = compression;
        chunk_.id = id;
        chunk_.placement_id = pid;
        chunk_.src_w = src_w;
        chunk_.src_h = src_h;
        chunk_.data_size = data_size;
        chunk_.data_offset = data_offset;
        chunk_.cols = cols;
        chunk_.rows = rows;
        chunk_.z_index = z_index;
        chunk_.quiet = quiet;
        chunk_.suppress_cursor_move = suppress_cursor;
    }

    if (!b64.empty()) {
        if (chunk_.data.size() + b64.size() > kMaxTotalImageBytes) {
            const uint32_t req_id = chunk_.id;
            const int eff_q = std::max(quiet, chunk_.quiet);
            chunk_ = ChunkState{};
            return make_reply(req_id, req_id, eff_q, false, "ENOMEM:image exceeds size limit");
        }
        if (!decode_base64(b64, chunk_.data)) {
            const uint32_t req_id = chunk_.id;
            const int eff_q = std::max(quiet, chunk_.quiet);
            chunk_ = ChunkState{};
            return make_reply(req_id, req_id, eff_q, false, "EINVAL:invalid base64");
        }
    }

    if (more == 1) {
        return {};
    }

    ChunkState finished = std::move(chunk_);
    chunk_ = ChunkState{};
    const int eff_q = std::max(quiet, finished.quiet);

    // Resolve transmission medium: 'd' (direct), 'f' (file), 't' (temp file), 's' (POSIX shared memory)
    std::vector<uint8_t> raw_bytes;
    if (finished.medium == 'f' || finished.medium == 't') {
        std::string path(finished.data.begin(), finished.data.end());
        if (finished.medium == 't' && path.find("tty-graphics-protocol") == std::string::npos) {
            return make_reply(finished.id, finished.id, eff_q, false, "EACCES:unsafe temp file path");
        }
        std::ifstream ifs(path, std::ios::binary | std::ios::ate);
        if (!ifs) {
            return make_reply(finished.id, finished.id, eff_q, false, "ENOENT:cannot open file");
        }
        const auto total_sz = static_cast<size_t>(ifs.tellg());
        if (total_sz == 0 || finished.data_offset >= total_sz) {
            return make_reply(finished.id, finished.id, eff_q, false, "EINVAL:empty or out-of-range file");
        }
        const size_t avail = total_sz - finished.data_offset;
        const size_t read_sz = (finished.data_size > 0) ? std::min(finished.data_size, avail) : avail;
        if (read_sz > kMaxTotalImageBytes) {
            return make_reply(finished.id, finished.id, eff_q, false, "ENOMEM:file too large");
        }
        raw_bytes.resize(read_sz);
        ifs.seekg(static_cast<std::streamoff>(finished.data_offset));
        ifs.read(reinterpret_cast<char*>(raw_bytes.data()), static_cast<std::streamsize>(read_sz));
        ifs.close();
        if (finished.medium == 't') {
            ::unlink(path.c_str());
        }
    } else if (finished.medium == 's') {
        // POSIX shared memory transfer (`shm_open` + `shm_unlink`)
        std::string shm_name(finished.data.begin(), finished.data.end());
        if (shm_name.empty() || shm_name.find("..") != std::string::npos) {
            return make_reply(finished.id, finished.id, eff_q, false, "EINVAL:invalid shm object name");
        }
        if (shm_name[0] != '/') shm_name.insert(shm_name.begin(), '/');
        int sfd = ::shm_open(shm_name.c_str(), O_RDONLY, 0);
        if (sfd < 0) {
            return make_reply(finished.id, finished.id, eff_q, false, "ENOENT:cannot open shm object");
        }
        struct stat st{};
        if (::fstat(sfd, &st) != 0 || st.st_size <= 0) {
            ::close(sfd);
            ::shm_unlink(shm_name.c_str());
            return make_reply(finished.id, finished.id, eff_q, false, "EINVAL:invalid shm size");
        }
        const size_t total_sz = static_cast<size_t>(st.st_size);
        const size_t avail = (finished.data_offset < total_sz) ? (total_sz - finished.data_offset) : 0;
        const size_t read_sz = (finished.data_size > 0) ? std::min(finished.data_size, avail) : avail;
        if (read_sz == 0 || read_sz > kMaxTotalImageBytes) {
            ::close(sfd);
            ::shm_unlink(shm_name.c_str());
            return make_reply(finished.id, finished.id, eff_q, false, "ENOMEM:shm object too large");
        }
        void* map = ::mmap(nullptr, total_sz, PROT_READ, MAP_SHARED, sfd, 0);
        ::close(sfd);
        ::shm_unlink(shm_name.c_str()); // Unlink immediately per Kitty Graphics spec
        if (map == MAP_FAILED) {
            return make_reply(finished.id, finished.id, eff_q, false, "EIO:mmap shm failed");
        }
        raw_bytes.resize(read_sz);
        std::memcpy(raw_bytes.data(), static_cast<const uint8_t*>(map) + finished.data_offset, read_sz);
        ::munmap(map, total_sz);
    } else {
        raw_bytes = std::move(finished.data);
    }

    // Optional RFC-1950 zlib decompression (`o=z`)
    if (finished.compression == 'z') {
        size_t expected = 0;
        if (finished.format == 32 && finished.src_w > 0 && finished.src_h > 0) {
            expected = static_cast<size_t>(finished.src_w) * finished.src_h * 4u;
        } else if (finished.format == 24 && finished.src_w > 0 && finished.src_h > 0) {
            expected = static_cast<size_t>(finished.src_w) * finished.src_h * 3u;
        }
        if (expected > kMaxTotalImageBytes) {
            return make_reply(finished.id, finished.id, eff_q, false, "ENOMEM:decompressed image exceeds limit");
        }
        std::vector<uint8_t> inflated;
        if (!zlib_decompressor_ || !zlib_decompressor_(raw_bytes.data(), raw_bytes.size(), expected, inflated)) {
            return make_reply(finished.id, finished.id, eff_q, false, "EINVAL:zlib decompression failed");
        }
        if (inflated.size() > kMaxTotalImageBytes) {
            return make_reply(finished.id, finished.id, eff_q, false, "ENOMEM:decompressed image exceeds limit");
        }
        raw_bytes = std::move(inflated);
    }

    // Decode into RGBA8888
    DecodedImage img{};
    if (finished.format == 100) {
        if (!png_decoder_ || !png_decoder_(raw_bytes.data(), raw_bytes.size(), img.width, img.height, img.rgba)) {
            return make_reply(finished.id, finished.id, eff_q, false, "EINVAL:PNG decode failed");
        }
    } else if (finished.format == 32) {
        if (finished.src_w == 0 || finished.src_h == 0 ||
            finished.src_w > kMaxDimension || finished.src_h > kMaxDimension) {
            return make_reply(finished.id, finished.id, eff_q, false, "EINVAL:invalid dimensions");
        }
        const size_t need = static_cast<size_t>(finished.src_w) * finished.src_h * 4u;
        if (raw_bytes.size() < need) {
            return make_reply(finished.id, finished.id, eff_q, false, "EINVAL:truncated RGBA payload");
        }
        img.width = finished.src_w;
        img.height = finished.src_h;
        img.rgba.assign(raw_bytes.begin(), raw_bytes.begin() + static_cast<ptrdiff_t>(need));
    } else if (finished.format == 24) {
        if (finished.src_w == 0 || finished.src_h == 0 ||
            finished.src_w > kMaxDimension || finished.src_h > kMaxDimension) {
            return make_reply(finished.id, finished.id, eff_q, false, "EINVAL:invalid dimensions");
        }
        const size_t pixels = static_cast<size_t>(finished.src_w) * finished.src_h;
        if (raw_bytes.size() < pixels * 3u) {
            return make_reply(finished.id, finished.id, eff_q, false, "EINVAL:truncated RGB payload");
        }
        img.width = finished.src_w;
        img.height = finished.src_h;
        img.rgba.resize(pixels * 4u);
        for (size_t i = 0; i < pixels; ++i) {
            img.rgba[i * 4 + 0] = raw_bytes[i * 3 + 0];
            img.rgba[i * 4 + 1] = raw_bytes[i * 3 + 1];
            img.rgba[i * 4 + 2] = raw_bytes[i * 3 + 2];
            img.rgba[i * 4 + 3] = 255;
        }
    } else {
        return make_reply(finished.id, finished.id, eff_q, false, "EINVAL:unsupported format");
    }

    const uint32_t final_id = (finished.id > 0) ? finished.id : next_id_++;
    if (final_id >= next_id_) next_id_ = final_id + 1;
    remove_image(final_id);
    evict_if_needed(img.rgba.size());

    img.id = final_id;
    img.generation = ++generation_;
    total_bytes_ += img.rgba.size();

    if (finished.action == 'T') {
        ImagePlacement pl{};
        pl.image_id = final_id;
        pl.placement_id = finished.placement_id ? finished.placement_id : 1;
        pl.row = cursor_row;
        pl.col = cursor_col;
        pl.cols = finished.cols;
        pl.rows = finished.rows;
        pl.z_index = finished.z_index;
        pl.is_alt_screen = alt_screen;
        placements_.push_back(pl);
        ++generation_;
        if (!finished.suppress_cursor_move) {
            compute_cursor_advance(img, finished.cols, finished.rows, cell_px_w, cell_px_h,
                                   out_cursor_advance_rows, out_cursor_advance_cols);
        }
    }

    images_[final_id] = std::move(img);
    return make_reply(finished.id, final_id, eff_q, false, "OK");
}

} // namespace vater::vt
