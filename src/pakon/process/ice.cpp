#include "pakon/process/ice.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace pakon::process {

namespace {

// IR more than `sigmas` noise widths below clean film starts a defect;
// `ring` pixels of dilation around it. Dust dips on 7908 sit mostly at
// 0.90..0.97 over a noise of ~0.005 (2026-11-06), so the thresholds
// follow the measured noise, not fixed levels.
struct Tuning {
    double sigmas;
    std::size_t ring;
};
Tuning tuning(IceStrength s) {
    switch (s) {
    case IceStrength::light: return {9.0, 1};
    case IceStrength::strong: return {4.0, 2};
    default: return {6.0, 2};
    }
}

constexpr std::array<std::pair<long, long>, 4> kSides{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};

} // namespace

std::string_view ice_name(IceStrength s) {
    switch (s) {
    case IceStrength::light: return "light";
    case IceStrength::normal: return "normal";
    case IceStrength::strong: return "strong";
    case IceStrength::off: break;
    }
    return "off";
}

std::optional<IceStrength> parse_ice(std::string_view s) {
    for (auto k : {IceStrength::off, IceStrength::light, IceStrength::normal, IceStrength::strong}) {
        if (ice_name(k) == s) {
            return k;
        }
    }
    return std::nullopt;
}

IceReport ice_correct(std::span<std::uint16_t> rows, std::size_t lines, const ScanInfo& info,
                      int line_shift, IceStrength strength) {
    IceReport rep;
    const std::size_t h = info.pixels;
    const std::size_t spr = 4 * h;
    const std::size_t sh = info.reference_shift;
    if (strength == IceStrength::off || !info.ir || h == 0 || lines < 8 ||
        rows.size() < lines * spr || info.dark_ir.size() < h + sh || info.white_ir.size() < h + sh) {
        return rep;
    }
    for (std::size_t c = 0; c < 3; ++c) {
        if (info.dark[c].size() < h + sh || info.white[c].size() < h + sh) {
            return rep;
        }
    }
    const auto at = [&](std::size_t r, std::size_t p) -> std::uint16_t& { return rows[r * spr + p]; };
    const auto inside = [&](long r, long p) {
        return r >= 0 && p >= 0 && r < static_cast<long>(lines) && p < static_cast<long>(h);
    };

    // 1-2. IR transmission over each CCD pixel's median.
    std::vector<float> ir(lines * h);
    std::vector<float> samples;
    for (std::size_t p = 0; p < h; ++p) {
        const double d = info.dark_ir[p + sh];
        const double k = 1.0 / std::max(1.0, info.white_ir[p + sh] - d);
        samples.clear();
        for (std::size_t r = 0; r < lines; ++r) {
            ir[r * h + p] = static_cast<float>(std::max(0.0, (at(r, 3 * h + p) - d) * k));
            if (r % 4 == 0) {
                samples.push_back(ir[r * h + p]);
            }
        }
        const auto mid = samples.begin() + static_cast<std::ptrdiff_t>(samples.size() / 2);
        std::nth_element(samples.begin(), mid, samples.end());
        const float m = std::max(1e-3f, *mid);
        for (std::size_t r = 0; r < lines; ++r) {
            ir[r * h + p] /= m;
        }
    }

    // 3. Red leak (IR ~ a + b * D_red over clean pixels) and the silver
    // check (IR against green transmission), on a sparse grid.
    const auto t_of = [&](std::size_t c, std::size_t r, std::size_t p) {
        const double d = info.dark[c][p + sh];
        return std::max(1e-4, (at(r, 3 * p + c) - d) / std::max(1.0, info.white[c][p + sh] - d));
    };
    double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0, gx = 0, gxx = 0, gxy = 0;
    for (std::size_t r = 0; r < lines; r += 3) {
        for (std::size_t p = h / 20; p < h - h / 20; p += 3) {
            const double y = ir[r * h + p];
            if (y < 0.9) {
                continue; // a defect, not the film
            }
            const double x = -std::log10(t_of(0, r, p));
            const double g = t_of(1, r, p);
            n += 1;
            sx += x;
            sy += y;
            sxx += x * x;
            sxy += x * y;
            syy += y * y;
            gx += g;
            gxx += g * g;
            gxy += g * y;
        }
    }
    if (n < 100) {
        return rep;
    }
    const double vy = syy - sy * sy / n;
    const double vg = gxx - gx * gx / n;
    const double corr = vy > 0 && vg > 0 ? (gxy - gx * sy / n) / std::sqrt(vy * vg) : 0.0;
    rep.ran = true;
    if (std::abs(corr) > kSilverCorrelation) {
        rep.silver = true;
        return rep;
    }
    const double vx = sxx - sx * sx / n;
    const double b = vx > 1e-9 ? (sxy - sx * sy / n) / vx : 0.0;
    const double a = sy / n - b * sx / n;
    rep.leak = b;
    for (std::size_t r = 0; r < lines; ++r) {
        for (std::size_t p = 0; p < h; ++p) {
            const double pred = a + b * -std::log10(t_of(0, r, p));
            ir[r * h + p] = static_cast<float>(ir[r * h + p] / std::max(0.5, pred));
        }
    }

    // 4. Noise: 1.4826 x the median absolute deviation from 1 (robust
    // against the defects themselves), floored at 0.002.
    std::vector<float> dev;
    for (std::size_t i = 0; i < lines * h; i += 7) {
        dev.push_back(std::abs(ir[i] - 1.0f));
    }
    const auto dmid = dev.begin() + static_cast<std::ptrdiff_t>(dev.size() / 2);
    std::nth_element(dev.begin(), dmid, dev.end());
    const double sigma = std::max(0.002, 1.4826 * *dmid);
    rep.sigma = sigma;
    // Seeds, grown over fainter neighbours (hysteresis), then a ring.
    const auto [sigmas, ring] = tuning(strength);
    const double seed = 1.0 - sigmas * sigma;
    const double grow = 1.0 - 2.0 * sigma;
    std::vector<std::uint8_t> mask(lines * h, 0);
    std::vector<std::size_t> stack;
    for (std::size_t i = 0; i < lines * h; ++i) {
        if (ir[i] < seed) {
            mask[i] = 1;
            stack.push_back(i);
        }
    }
    while (!stack.empty()) {
        const std::size_t i = stack.back();
        stack.pop_back();
        for (const auto [dr, dp] : kSides) {
            const long rr = static_cast<long>(i / h) + dr, pp = static_cast<long>(i % h) + dp;
            if (!inside(rr, pp)) {
                continue;
            }
            const std::size_t j = static_cast<std::size_t>(rr) * h + static_cast<std::size_t>(pp);
            if (!mask[j] && ir[j] < grow) {
                mask[j] = 1;
                stack.push_back(j);
            }
        }
    }
    for (std::size_t pass = 0; pass < ring; ++pass) {
        std::vector<std::uint8_t> next = mask;
        for (std::size_t i = 0; i < lines * h; ++i) {
            if (mask[i]) {
                continue;
            }
            for (const auto [dr, dp] : kSides) {
                const long rr = static_cast<long>(i / h) + dr, pp = static_cast<long>(i % h) + dp;
                if (inside(rr, pp) && mask[static_cast<std::size_t>(rr) * h + static_cast<std::size_t>(pp)]) {
                    next[i] = 1;
                    break;
                }
            }
        }
        mask.swap(next);
    }
    // 5. Channel c sees the film spot of IR line r at raw line r + c * s.
    const std::size_t s = static_cast<std::size_t>(std::max(0, line_shift));
    constexpr long kReach = 64;
    std::vector<std::uint16_t> orig(lines * h);
    for (std::size_t c = 0; c < 3; ++c) {
        const std::size_t off = c * s;
        if (off >= lines) {
            break;
        }
        for (std::size_t r = 0; r < lines; ++r) {
            for (std::size_t p = 0; p < h; ++p) {
                orig[r * h + p] = at(r, 3 * p + c);
            }
        }
        // Clean in this channel's raw coordinates: no defect at IR line rr - off.
        const auto clean = [&](long rr, long pp) {
            const long ri = rr - static_cast<long>(off);
            return ri < 0 || !mask[static_cast<std::size_t>(ri) * h + static_cast<std::size_t>(pp)];
        };
        for (std::size_t r = 0; r + off < lines; ++r) {
            for (std::size_t p = 0; p < h; ++p) {
                const std::size_t i = r * h + p;
                if (!mask[i]) {
                    continue;
                }
                const std::size_t raw_r = r + off;
                {
                    double sum = 0, wsum = 0;
                    for (const auto [dr, dp] : kSides) {
                        for (long k = 1; k <= kReach; ++k) {
                            const long rr = static_cast<long>(raw_r) + dr * k;
                            const long pp = static_cast<long>(p) + dp * k;
                            if (!inside(rr, pp)) {
                                break;
                            }
                            if (clean(rr, pp)) {
                                const double w = 1.0 / static_cast<double>(k);
                                sum += w * orig[static_cast<std::size_t>(rr) * h + static_cast<std::size_t>(pp)];
                                wsum += w;
                                break;
                            }
                        }
                    }
                    if (wsum > 0) {
                        at(raw_r, 3 * p + c) = static_cast<std::uint16_t>(std::lround(sum / wsum));
                    }
                }
                rep.defects += c == 0;
            }
        }
    }
    return rep;
}

} // namespace pakon::process
