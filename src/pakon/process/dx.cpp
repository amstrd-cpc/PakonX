#include "pakon/process/dx.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <string>

namespace pakon::process {

namespace {

bool even_parity(unsigned a, unsigned b, unsigned c) {
    return ((std::popcount(a) + std::popcount(b) + std::popcount(c)) & 1) == 0;
}

void vote(std::vector<std::pair<FilmDx, int>>& candidates, FilmDx dx) {
    for (auto& [c, n] : candidates) {
        if (c.product == dx.product && c.specifier == dx.specifier) {
            ++n;
            return;
        }
    }
    if (candidates.size() < 4) { // TLB keeps four slots
        candidates.push_back({dx, 1});
    }
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

} // namespace

void decode_dx_record(std::span<const std::uint8_t> record, std::size_t film_row, DxDecode& out,
                      std::vector<std::pair<FilmDx, int>>& candidates) {
    ++out.records;
    if (record.size() < 3) {
        return;
    }
    const std::size_t entries = record[2];
    // Positions are line-counter values; the counter runs film row + c.
    const unsigned counter = (static_cast<unsigned>(record[0]) << 8) | record[1];
    const auto row_of = [&](unsigned pos) -> std::optional<std::size_t> {
        const std::size_t behind = (counter - pos) & 0xffffu;
        if (behind > film_row) {
            return std::nullopt;
        }
        return film_row - behind;
    };
    for (std::size_t k = 0; k < entries && 3 + 5 * k + 5 <= record.size(); ++k) {
        const auto* e = record.data() + 3 + 5 * k;
        const unsigned type = e[0] & 0x0fu;
        const unsigned p0 = e[1], p1 = e[2], p2 = e[3], p3 = e[4];
        if (type == 5) {
            out.last_mark_row = row_of((p1 << 8) | p2);
        } else if (type == 3) {
            ++out.codes;
            if ((p2 & 0x20u) != 0 || !even_parity(p1, p2, p3 & 0x1fu)) {
                ++out.rejected;
                continue;
            }
            int frame = static_cast<int>((p1 >> 2) + (p2 & 1u) * 64);
            if (frame > 0x79) {
                frame -= 0x80;
            }
            vote(candidates, {static_cast<int>((p2 >> 6) + (p3 & 0x1fu) * 4),
                              static_cast<int>((p2 >> 1) & 0xfu)});
            out.frames.push_back({film_row, out.last_mark_row, frame, (p0 & 1u) != 0});
        } else if (type == 4) {
            ++out.codes;
            if ((p2 & 0x20u) != 0 || !even_parity(p1, p2, 0)) {
                ++out.rejected;
                continue;
            }
            vote(candidates, {static_cast<int>((p1 >> 6) + (p2 & 0x1fu) * 4),
                              static_cast<int>((p1 >> 1) & 0xfu)});
        }
    }
}

DxDecode decode_dx(const Json& sidecar) {
    DxDecode out;
    std::vector<std::pair<FilmDx, int>> candidates;
    if (const auto* recs = sidecar["dx"]["records"].array()) {
        for (const auto& r : *recs) {
            const auto hex = r["hex"].string().value_or("");
            std::vector<std::uint8_t> bytes;
            for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
                const int hi = hex_digit(hex[i]);
                const int lo = hex_digit(hex[i + 1]);
                if (hi < 0 || lo < 0) {
                    break;
                }
                bytes.push_back(static_cast<std::uint8_t>(hi * 16 + lo));
            }
            decode_dx_record(bytes, static_cast<std::size_t>(r["film_row"].number().value_or(0)),
                             out, candidates);
        }
    }
    for (const auto& [c, n] : candidates) {
        if (n > out.votes) { // ties keep the earlier slot
            out.votes = n;
            out.film = c;
        }
    }
    return out;
}

std::vector<std::optional<int>> dx_frame_numbers(
    const DxDecode& dx, std::span<const std::pair<std::size_t, std::size_t>> pictures,
    double pitch) {
    std::vector<std::optional<int>> out(pictures.size());
    const double half = pitch / 2;
    if (half <= 0) {
        return out;
    }
    // Fixed slope (one code per half pitch): the intercept is the median
    // of row - code x half over the codes with a mark.
    std::vector<double> origins;
    for (const auto& c : dx.frames) {
        if (c.mark_row) {
            origins.push_back(static_cast<double>(*c.mark_row) + kDxGateShift * pitch -
                              c.code * half);
        }
    }
    if (origins.empty()) {
        return out;
    }
    std::sort(origins.begin(), origins.end());
    const double origin = origins[origins.size() / 2];
    for (std::size_t i = 0; i < pictures.size(); ++i) {
        const double centre = 0.5 * static_cast<double>(pictures[i].first + pictures[i].second);
        const double code = (centre - origin) / half; // ~2N + 0.5 at a picture
        out[i] = static_cast<int>(std::floor(code / 2));
    }
    return out;
}

} // namespace pakon::process
