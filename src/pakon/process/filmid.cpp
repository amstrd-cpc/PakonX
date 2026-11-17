#include "pakon/process/filmid.hpp"

#include "pakon/process/positive.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <vector>

namespace pakon::process {

namespace {

// ISO speed by DX Part 1 (row, product 0..127) and Part 2 (column,
// specifier 0..15), 0 = no speed listed: Kodak Ansel's
// common-ProdCodeTable.dpi (PIMA DX releases to 2003), verbatim.
constexpr std::uint16_t kDxIso[128][16] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {100, 100, 100, 100, 200, 200, 200, 100, 400, 400, 1600, 3200, 100, 100, 100, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 200, 0, 0, 100, 100, 160, 100, 0, 200, 0, 0, 100, 200, 100, 100},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 400, 200, 100, 100, 100, 100, 1600, 400, 400, 200, 100, 100, 100, 100},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 400, 200, 100, 0, 160, 100, 0, 800, 400, 200, 100, 100, 160, 400},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {100, 100, 100, 50, 200, 0, 200, 400, 400, 0, 1000, 200, 100, 200, 0, 0},
    {0, 0, 0, 0, 0, 400, 100, 100, 0, 200, 400, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {400, 800, 100, 200, 1600, 400, 800, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 400, 400, 0, 0, 0, 0, 0, 0, 0, 100, 200, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 200, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {800, 400, 400, 200, 100, 200, 100, 100, 800, 400, 400, 200, 100, 200, 0, 400},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1600, 800, 400, 200, 160, 100, 100, 100, 1600, 800, 400, 400, 100, 100, 100, 400},
    {800, 400, 400, 200, 100, 800, 100, 100, 400, 800, 400, 0, 100, 100, 100, 100},
    {0, 0, 400, 0, 0, 0, 0, 0, 0, 0, 400, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {100, 100, 100, 50, 200, 200, 400, 400, 400, 200, 200, 200, 100, 200, 100, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 800, 800, 800, 800, 800, 800, 800, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 100, 0, 0, 200, 100, 200, 0, 0, 400, 400, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 200, 0, 200, 0, 0, 400, 0, 0, 0, 0, 0, 0},
    {0, 100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 160, 100, 200, 200, 0, 400, 400, 400, 0, 200, 100, 200, 0, 0},
    {400, 400, 400, 100, 200, 400, 100, 400, 400, 200, 100, 160, 100, 200, 800, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 200, 100, 0, 0, 0, 400, 200, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 100, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {400, 200, 400, 400, 200, 800, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {400, 100, 200, 400, 100, 200, 400, 800, 400, 100, 100, 200, 200, 400, 200, 800},
    {0, 200, 200, 100, 400, 100, 100, 200, 400, 400, 100, 160, 100, 400, 800, 400},
    {0, 0, 0, 0, 0, 0, 100, 400, 200, 1600, 100, 100, 200, 0, 0, 0},
    {0, 25, 125, 1000, 400, 400, 1600, 400, 25, 0, 100, 100, 0, 400, 0, 0},
    {0, 400, 400, 400, 400, 200, 100, 200, 400, 200, 400, 400, 0, 200, 100, 0},
    {0, 0, 1000, 0, 100, 200, 400, 0, 100, 100, 400, 200, 100, 200, 800, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 100, 200, 0, 400, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 100, 200, 0, 400, 0, 0, 0, 0, 0, 0, 0, 200, 0, 400, 0},
    {0, 100, 200, 0, 400, 400, 0, 0, 800, 800, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 100, 0, 400, 0, 400, 0, 0, 0},
    {0, 100, 200, 400, 0, 100, 200, 400, 200, 200, 200, 400, 100, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 400, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 200, 400, 400, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 400, 400, 400, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 100, 100, 100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 100, 100, 200, 100, 400, 200, 0, 400, 100, 100, 200, 0, 400, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 400, 400, 400, 400, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 400, 160, 0, 400, 400, 400, 100, 0, 400, 100, 0, 0, 0},
    {0, 100, 100, 200, 200, 100, 200, 800, 400, 400, 400, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 100, 800, 200, 200, 200, 200, 200, 400, 400, 400, 200, 100, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 200, 0, 0, 0, 0, 400, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 100, 200, 100, 400, 200, 0, 400, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
};

} // namespace

std::optional<int> dx_iso(int product, int specifier) {
    if (product < 0 || product >= 128 || specifier < 0 || specifier >= 16 || kDxIso[product][specifier] == 0) {
        return std::nullopt;
    }
    return kDxIso[product][specifier];
}

FilmGuess detect_film(PakrawReader& raw, const ScanInfo& info, const FrameSplit& split) {
    FilmGuess g;
    const std::size_t h = info.pixels;
    const std::size_t sh = info.reference_shift;
    const std::size_t spr = raw.samples_per_row();
    if (h == 0 || spr < 3 * h) {
        return g;
    }
    for (std::size_t c = 0; c < 3; ++c) {
        if (info.dark[c].size() < h + sh || info.white[c].size() < h + sh) {
            return g;
        }
    }
    // Flat-fielded transmission, on-times taken out (as process/positive.cpp).
    const auto t = [&](std::size_t c, std::size_t p, std::uint16_t v) {
        const double k = info.film_on_time[c] > 0 && info.calibration_on_time[c] > 0
                             ? info.calibration_on_time[c] / info.film_on_time[c] : 1.0;
        const double d = info.dark[c][p + sh];
        return std::max(1e-4, (v - d) / std::max(1.0, info.white[c][p + sh] - d) * k);
    };
    std::vector<std::uint16_t> row(spr);
    const std::size_t from = h / 10, to = h - h / 10;

    // Base: the inner half of every gap run.
    std::array<double, 3> sum{};
    std::size_t n = 0;
    for (const auto& gap : split.gaps) {
        const std::size_t len = gap.last - gap.first;
        for (std::size_t r = gap.first + len / 4; r + len / 4 < gap.last && r < raw.rows(); r += 2) {
            if (!raw.read_row(r, row)) {
                return g;
            }
            for (std::size_t p = from; p < to; p += 8) {
                for (std::size_t c = 0; c < 3; ++c) {
                    sum[c] += t(c, p, row[3 * p + c]);
                }
                ++n;
            }
        }
    }
    if (n > 0) {
        g.have_base = true;
        for (std::size_t c = 0; c < 3; ++c) {
            g.base[c] = sum[c] / static_cast<double>(n);
        }
    }

    // Colour and IR on the pictures (every 16th line and pixel). The R and B
    // rows sit s lines off the G row (positive.hpp kRowSpacingPerPitch), so
    // each channel is read from its own line. A B&W image may carry a fixed
    // cast that grows with density; colour is what varies beyond it: the
    // residual of (d_R - d_mean) and (d_B - d_mean) after a straight-line
    // fit on d_mean.
    const bool ir = info.ir && info.dark_ir.size() >= h + sh && spr >= 4 * h;
    const auto s = static_cast<std::size_t>(std::lround(kRowSpacingPerPitch * split.pitch));
    std::vector<std::uint16_t> rowR(spr), rowB(spr);
    std::vector<std::array<double, 3>> pts; // d_mean, d_R - d_mean, d_B - d_mean
    double gx = 0, gy = 0, gxx = 0, gyy = 0, gxy = 0;
    for (const auto& f : split.frames) {
        if (f.kind != FrameKind::picture) {
            continue;
        }
        for (std::size_t r = std::max(f.first, s); r < f.last && r + s < raw.rows(); r += 16) {
            if (!raw.read_row(r, row) || !raw.read_row(r - s, rowR) || !raw.read_row(r + s, rowB)) {
                return g;
            }
            for (std::size_t p = from; p < to; p += 16) {
                const std::array<std::uint16_t, 3> v{rowR[3 * p], row[3 * p + 1], rowB[3 * p + 2]};
                std::array<double, 3> d{};
                for (std::size_t c = 0; c < 3; ++c) {
                    d[c] = -std::log10(t(c, p, v[c]) / (g.have_base ? g.base[c] : 1.0));
                }
                const double dm = (d[0] + d[1] + d[2]) / 3;
                pts.push_back({dm, d[0] - dm, d[2] - dm});
                if (ir) {
                    const double x = t(1, p, row[3 * p + 1]);
                    const double di = info.dark_ir[p + sh];
                    const double y = (rowR[3 * h + p] - di) / std::max(1.0, info.white_ir[p + sh] - di);
                    gx += x; gy += y; gxx += x * x; gyy += y * y; gxy += x * y;
                }
            }
        }
    }
    const std::size_t m = pts.size();
    if (m > 2) {
        double res = 0;
        for (std::size_t k = 1; k <= 2; ++k) { // R, then B
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (const auto& q : pts) { sx += q[0]; sy += q[k]; sxx += q[0] * q[0]; sxy += q[0] * q[k]; }
            const double N = static_cast<double>(m);
            const double vx = sxx - sx * sx / N;
            const double b = vx > 1e-12 ? (sxy - sx * sy / N) / vx : 0.0;
            const double a = sy / N - b * sx / N;
            for (const auto& q : pts) { const double e = q[k] - (a + b * q[0]); res += e * e; }
        }
        g.colour = std::sqrt(res / (2.0 * static_cast<double>(m)));
        if (ir) {
            const double N = static_cast<double>(m);
            const double vx = gxx - gx * gx / N, vy = gyy - gy * gy / N;
            if (vx > 0 && vy > 0) {
                g.ir_follow = (gxy - gx * gy / N) / std::sqrt(vx * vy);
            }
        }
    }

    // A clear base scanned at the C-41 on-times (film boost) saturates the
    // A/D: the gap then reads at the channel's ceiling, (65535 - dark) /
    // (white - dark) x on_cal / on_film. 13212 / 7918 (B&W, Base 16 IR):
    // base 0.728 / 0.454 / 0.217 = the R / G / B ceilings (2026-11-07).
    std::array<double, 3> ceiling{};
    for (std::size_t c = 0; c < 3; ++c) {
        const double k = info.film_on_time[c] > 0 && info.calibration_on_time[c] > 0
                             ? info.calibration_on_time[c] / info.film_on_time[c] : 1.0;
        double sc = 0;
        for (std::size_t p = from; p < to; ++p) {
            const double d = info.dark[c][p + sh];
            sc += (65535.0 - d) / std::max(1.0, info.white[c][p + sh] - d) * k;
        }
        ceiling[c] = sc / static_cast<double>(to - from);
    }
    g.clipped = g.have_base && g.base[1] > 0.9 * ceiling[1] && g.base[2] > 0.9 * ceiling[2];

    // Decide. kDense: a slide's opaque frame gap (masked colour negative
    // bases read 0.075..0.16 green here: strip8, 7908, 7919, 33232);
    // kMask: an orange mask's blue/red base ratio (0.17..0.27 measured);
    // kSilverIr: silver blocks infrared (IR follows the picture 0.92..0.95
    // on 13212 / 7918; dyes 0.28..0.61 on 7908 / 7919 / 33232).
    constexpr double kDense = 0.03, kMask = 0.5, kColour = 0.03, kSilverIr = 0.85;
    const double mask = g.have_base && g.base[0] > 0 ? g.base[2] / g.base[0] : 1.0;
    const bool colour = g.colour > kColour;
    const bool silver = g.ir_follow && *g.ir_follow > kSilverIr;
    if (!g.have_base || m == 0) {
        g.kind = "";
    } else if (g.base[1] < kDense) {
        g.kind = "e6";
    } else if (silver) {
        g.kind = "bw-silver";
    } else if (g.clipped) {
        // Clear base, clipped: the colour measure no longer holds.
        g.kind = g.ir_follow ? "c41-unmasked" : "";
    } else if (mask < kMask) {
        g.kind = colour ? "c41" : "bw-c41";
    } else {
        g.kind = colour ? "c41-unmasked" : "bw-silver";
    }
    g.why = std::format("base R {:.3f} G {:.3f} B {:.3f} (B/R {:.2f}{}), colour {:.3f}{}", g.base[0], g.base[1],
                        g.base[2], mask, g.clipped ? ", clear base clipped at the C-41 exposure" : "", g.colour,
                        g.ir_follow ? std::format(", IR follows the picture {:.2f}", *g.ir_follow) : std::string());
    return g;
}

} // namespace pakon::process
