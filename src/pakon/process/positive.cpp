#include "pakon/process/positive.hpp"

#include "pakon/process/ice.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <thread>

namespace pakon::process {

namespace {

constexpr std::size_t kBins = 4000; // density 0..4 in 0.001 steps
constexpr double kBinWidth = 0.001;
constexpr double kMinT = 1e-4;

// Flat-field factor per channel and film pixel: t = (raw - dark) * k.
struct FlatField {
    std::array<std::vector<double>, 3> dark;
    std::array<std::vector<double>, 3> scale;

    explicit FlatField(const ScanInfo& info) {
        for (std::size_t c = 0; c < 3; ++c) {
            dark[c].resize(info.pixels);
            scale[c].resize(info.pixels);
            const double boost = info.film_on_time[c] > 0 && info.calibration_on_time[c] > 0
                                     ? info.calibration_on_time[c] / info.film_on_time[c]
                                     : 1.0;
            for (std::size_t p = 0; p < info.pixels; ++p) {
                const std::size_t q = p + info.reference_shift;
                const double d = q < info.dark[c].size() ? info.dark[c][q] : 0.0;
                const double w = q < info.white[c].size() ? info.white[c][q] : 65535.0;
                dark[c][p] = d;
                scale[c][p] = boost / std::max(1.0, w - d);
            }
        }
    }
    double t(std::size_t c, std::size_t p, std::uint16_t v) const {
        return std::max(kMinT, (v - dark[c][p]) * scale[c][p]);
    }
};

VoidResult check(const PakrawReader& raw, const ScanInfo& info) {
    const std::size_t channels = info.ir ? 4 : 3;
    if (raw.samples_per_row() != info.pixels * channels) {
        return void_failure(ErrorKind::image_bad_input,
                            std::format("{} samples per row, sidecar says {} pixels x {}",
                                        raw.samples_per_row(), info.pixels, channels));
    }
    for (std::size_t c = 0; c < 3; ++c) {
        if (info.dark[c].size() < info.pixels + info.reference_shift ||
            info.white[c].size() < info.pixels + info.reference_shift) {
            return void_failure(ErrorKind::image_bad_input,
                                "sidecar lacks the R/G/B dark and white references");
        }
    }
    return {};
}

double percentile(const std::vector<std::uint64_t>& hist, double q) {
    std::uint64_t total = 0;
    for (const auto n : hist) {
        total += n;
    }
    if (total == 0) {
        return 0.0;
    }
    const auto target = static_cast<std::uint64_t>(q * static_cast<double>(total));
    std::uint64_t seen = 0;
    for (std::size_t b = 0; b < hist.size(); ++b) {
        seen += hist[b];
        if (seen > target) {
            return (static_cast<double>(b) + 0.5) * kBinWidth;
        }
    }
    return static_cast<double>(hist.size()) * kBinWidth;
}

double srgb(double y) {
    y = std::clamp(y, 0.0, 1.0);
    return y <= 0.0031308 ? 12.92 * y : 1.055 * std::pow(y, 1.0 / 2.4) - 0.055;
}

} // namespace

Result<PositiveParams> measure_positive(PakrawReader& raw, const ScanInfo& info,
                                        const FrameSplit& split, const PositiveOptions& opt) {
    if (auto c = check(raw, info); !c) {
        return c.error();
    }
    const FlatField ff(info);
    PositiveParams pp;
    pp.range_decades = opt.range_decades;
    std::vector<std::uint16_t> row(raw.samples_per_row());
    const std::size_t from = info.pixels / 10;
    const std::size_t to = info.pixels - info.pixels / 10;

    // Base: mean transmission over the gap lines (inner half of each run,
    // away from the frame edges).
    std::array<double, 3> sum{};
    std::size_t n = 0;
    pp.reversal = info.reversal;
    for (const auto& g : pp.reversal ? std::vector<GapRun>{} : split.gaps) {
        const std::size_t len = g.last - g.first;
        for (std::size_t r = g.first + len / 4; r < g.last - len / 4 && r < raw.rows(); r += 2) {
            if (auto rr = raw.read_row(r, row); !rr) {
                return rr.error();
            }
            for (std::size_t p = from; p < to; p += 4) {
                for (std::size_t c = 0; c < 3; ++c) {
                    sum[c] += ff.t(c, p, row[3 * p + c]);
                }
                ++n;
            }
            ++pp.base_lines;
        }
    }
    if (pp.reversal) {
        pp.base = {1.0, 1.0, 1.0}; // no mask: densities as they are
    } else if (n == 0) {
        return failure<PositiveParams>(ErrorKind::image_bad_input,
                                       "no bare-base gap lines to measure the film base on");
    } else {
        for (std::size_t c = 0; c < 3; ++c) {
            pp.base[c] = sum[c] / static_cast<double>(n);
        }
    }

    // Densities above base over every picture.
    std::array<std::vector<std::uint64_t>, 3> hist;
    for (auto& h : hist) {
        h.assign(kBins, 0);
    }
    for (const auto& f : split.frames) {
        if (f.kind != FrameKind::picture) {
            continue;
        }
        for (std::size_t r = f.first; r < f.last && r < raw.rows(); r += 4) {
            if (auto rr = raw.read_row(r, row); !rr) {
                return rr.error();
            }
            for (std::size_t p = 0; p < info.pixels; p += 4) {
                for (std::size_t c = 0; c < 3; ++c) {
                    const double d = -std::log10(ff.t(c, p, row[3 * p + c]) / pp.base[c]);
                    const auto b = static_cast<std::size_t>(std::clamp(d / kBinWidth, 0.0,
                                                                       static_cast<double>(kBins - 1)));
                    ++hist[c][b];
                }
            }
        }
    }
    double span = 0;
    for (std::size_t c = 0; c < 3; ++c) {
        pp.d_low[c] = percentile(hist[c], opt.low_percentile);
        pp.d_mid[c] = percentile(hist[c], 0.5);
        pp.d_high[c] = std::max(pp.d_low[c] + 0.05, percentile(hist[c], opt.high_percentile));
        span += pp.d_high[c] - pp.d_low[c];
    }
    pp.span = span / 3.0;
    double mid = 0;
    if (pp.reversal) {
        const double common = (pp.d_mid[0] + pp.d_mid[1] + pp.d_mid[2]) / 3.0;
        for (std::size_t c = 0; c < 3; ++c) {
            mid += (pp.d_high[c] - common) / pp.span;
        }
        pp.d_mid.fill(common);
    } else {
        for (std::size_t c = 0; c < 3; ++c) {
            mid += (pp.d_mid[c] - pp.d_low[c]) / pp.span;
        }
    }
    pp.mid_x = mid / 3.0;
    pp.line_shift = static_cast<int>(std::lround(kRowSpacingPerPitch * split.pitch));
    pp.per_frame_exposure = opt.per_frame_exposure;
    pp.max_exposure_shift = opt.max_exposure_shift;
    pp.max_exposure_lift = opt.max_exposure_lift;
    return pp;
}

Result<std::map<std::size_t, Adjust>> parse_adjustments(const Json& json) {
    using Out = std::map<std::size_t, Adjust>;
    const auto* pictures = json["pictures"].object();
    if (!pictures) {
        return failure<Out>(ErrorKind::image_bad_input, "adjust: no \"pictures\" object");
    }
    Out out;
    for (const auto& [key, v] : *pictures) {
        std::size_t n = 0;
        for (const char ch : key) {
            n = ch >= '0' && ch <= '9' && n < 100000 ? n * 10 + static_cast<std::size_t>(ch - '0') : 0;
            if (n == 0) {
                break;
            }
        }
        if (n == 0) {
            return failure<Out>(ErrorKind::image_bad_input, std::format("adjust: picture '{}'", key));
        }
        Adjust a;
        const auto field = [&](const char* name, double& to, double lo, double hi) -> bool {
            if (v[name].is_null()) {
                return true;
            }
            const auto x = v[name].number();
            if (!x || !(*x >= lo && *x <= hi)) {
                return false;
            }
            to = *x;
            return true;
        };
        if (!field("exposure", a.exposure, -3, 3) || !field("red", a.balance[0], -2, 2) ||
            !field("green", a.balance[1], -2, 2) || !field("blue", a.balance[2], -2, 2) ||
            !field("contrast", a.contrast, 0.5, 2)) {
            return failure<Out>(ErrorKind::image_bad_input,
                                std::format("adjust: picture {}: a value is out of range", n));
        }
        if (!v["rotate"].is_null()) {
            const auto r = v["rotate"].number();
            if (!r || !(*r == 0 || *r == 90 || *r == 180 || *r == 270)) {
                return failure<Out>(ErrorKind::image_bad_input,
                                    std::format("adjust: picture {}: rotate is 0, 90, 180 or 270", n));
            }
            a.rotate = static_cast<int>(*r);
        }
        out[n] = a;
    }
    return out;
}

Result<std::vector<std::uint16_t>> develop_frame(PakrawReader& raw, const ScanInfo& info,
                                                 const PositiveParams& pp, const Frame& f,
                                                 Feed feed, const Adjust& adj) {
    if (auto c = check(raw, info); !c) {
        return c.error();
    }
    const FlatField ff(info);
    const std::size_t w = f.last - f.first;
    const std::size_t h = info.pixels;
    const std::size_t spr = raw.samples_per_row();
    const auto s = static_cast<std::size_t>(std::max(0, pp.line_shift));

    // The frame plus s lines either side (R and B come from neighbours).
    const std::size_t lo = f.first >= s ? f.first - s : 0;
    const std::size_t hi = std::min<std::size_t>(static_cast<std::size_t>(raw.rows()), f.last + s);
    std::vector<std::uint16_t> rows((hi - lo) * spr);
    for (std::size_t r = lo; r < hi; ++r) {
        if (auto rr = raw.read_row(r, std::span<std::uint16_t>(rows).subspan((r - lo) * spr, spr)); !rr) {
            return rr.error();
        }
    }
    ice_correct(rows, hi - lo, info, pp.line_shift, info.ice);
    const auto sample = [&](std::size_t line, std::ptrdiff_t shift, std::size_t p, std::size_t c) {
        const auto want = static_cast<std::ptrdiff_t>(line) + shift;
        const auto clamped = std::clamp<std::ptrdiff_t>(want, static_cast<std::ptrdiff_t>(lo),
                                                        static_cast<std::ptrdiff_t>(hi) - 1);
        return rows[static_cast<std::size_t>(clamped - static_cast<std::ptrdiff_t>(lo)) * spr + 3 * p + c];
    };
    const std::array<std::ptrdiff_t, 3> shift{-static_cast<std::ptrdiff_t>(s), 0,
                                              static_cast<std::ptrdiff_t>(s)};
    const auto x_of = [&](std::size_t line, std::size_t p, std::size_t c) {
        const double d = -std::log10(ff.t(c, p, sample(line, shift[c], p, c)) / pp.base[c]);
        return (pp.reversal ? pp.d_mid[c] - d : d - pp.d_mid[c]) / pp.span + pp.mid_x;
    };

    if (pp.negative) {
        std::vector<std::uint16_t> neg(w * h * 3);
        for (std::size_t r = 0; r < w; ++r) {
            const std::size_t x_out = picture_col(r, w, feed);
            for (std::size_t p = 0; p < h; ++p) {
                const std::size_t y_out = picture_row(p, h, feed);
                for (std::size_t c = 0; c < 3; ++c) {
                    const double t = std::clamp(ff.t(c, p, sample(f.first + r, shift[c], p, c)), 0.0, 1.0);
                    neg[(y_out * w + x_out) * 3 + c] = static_cast<std::uint16_t>(std::lround(t * 65535.0));
                }
            }
        }
        return neg;
    }

    // Per-frame exposure: the 99.5 % of the brightest channel to 1.
    double exposure = 0;
    if (pp.per_frame_exposure) {
        std::vector<double> xs;
        for (std::size_t r = f.first; r < f.last; r += 4) {
            for (std::size_t p = h / 20; p < h - h / 20; p += 4) {
                xs.push_back(std::max({x_of(r, p, 0), x_of(r, p, 1), x_of(r, p, 2)}));
            }
        }
        if (!xs.empty()) {
            const auto k = static_cast<std::ptrdiff_t>(0.995 * static_cast<double>(xs.size() - 1));
            std::nth_element(xs.begin(), xs.begin() + k, xs.end());
            exposure = std::clamp(1.0 - xs[static_cast<std::size_t>(k)], -pp.max_exposure_shift,
                                  pp.max_exposure_lift);
        }
    }

    const double floor = std::pow(10.0, -pp.range_decades);
    // The user's look (Adjust): grey pivot and per-channel shift in x.
    const bool adjusted = !adj.identity();
    const double grey = 1.0 + std::log10(0.18 * (1.0 - floor) + floor) / pp.range_decades;
    std::array<double, 3> shift_x{};
    for (std::size_t c = 0; c < 3; ++c) {
        shift_x[c] = (adj.exposure + adj.balance[c]) * std::log10(2.0) / pp.range_decades;
    }
    std::vector<std::uint16_t> out(w * h * 3);
    // Film lines in bands, one thread each: every output pixel depends only
    // on the shared, read-only inputs, so the result is the same as serial.
    const auto develop_lines = [&](std::size_t from, std::size_t to) {
        for (std::size_t r = from; r < to; ++r) {
            const std::size_t x_out = picture_col(r, w, feed);
            for (std::size_t p = 0; p < h; ++p) {
                const std::size_t y_out = picture_row(p, h, feed);
                for (std::size_t c = 0; c < 3; ++c) {
                    double x = x_of(f.first + r, p, c) + exposure;
                    if (adjusted) {
                        x = grey + (x - grey) * adj.contrast + shift_x[c];
                    }
                    x = std::clamp(x, 0.0, 1.0);
                    const double y = (std::pow(10.0, pp.range_decades * (x - 1.0)) - floor) / (1.0 - floor);
                    out[(y_out * w + x_out) * 3 + c] =
                        static_cast<std::uint16_t>(std::lround(srgb(y) * 65535.0));
                }
            }
        }
    };
    const std::size_t bands = std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, 16);
    std::vector<std::thread> workers; // std::thread: Apple's libc++ has no jthread
    for (std::size_t b = 1; b < bands; ++b) {
        workers.emplace_back(develop_lines, w * b / bands, w * (b + 1) / bands);
    }
    develop_lines(0, w / bands);
    for (auto& t : workers) {
        t.join();
    }
    return out;
}

Preview8 preview8(std::span<const std::uint16_t> rgb16, std::size_t width, std::size_t height,
                  std::size_t max_side) {
    Preview8 p;
    if (width == 0 || height == 0 || max_side == 0 || rgb16.size() < width * height * 3) {
        return p;
    }
    const std::size_t factor = std::max<std::size_t>(1, (std::max(width, height) + max_side - 1) / max_side);
    p.width = std::max<std::size_t>(1, width / factor);
    p.height = std::max<std::size_t>(1, height / factor);
    p.rgb.resize(p.width * p.height * 3);
    const std::size_t n = factor * factor;
    for (std::size_t y = 0; y < p.height; ++y) {
        for (std::size_t x = 0; x < p.width; ++x) {
            for (std::size_t c = 0; c < 3; ++c) {
                std::size_t s = 0;
                for (std::size_t dy = 0; dy < factor; ++dy) {
                    const std::uint16_t* row = rgb16.data() + ((y * factor + dy) * width + x * factor) * 3;
                    for (std::size_t dx = 0; dx < factor; ++dx) {
                        s += row[3 * dx + c];
                    }
                }
                // mean / 257 maps 0..65535 onto 0..255 exactly at the ends
                p.rgb[(y * p.width + x) * 3 + c] =
                    static_cast<std::uint8_t>((s + n * 257 / 2) / (n * 257));
            }
        }
    }
    return p;
}

std::vector<std::uint16_t> rotate_rgb(std::span<const std::uint16_t> px, std::uint32_t& w,
                                      std::uint32_t& h, int degrees) {
    const std::size_t sw = w, sh = h;
    const int turn = ((degrees / 90) % 4 + 4) % 4;
    if (turn == 0) {
        return {px.begin(), px.end()};
    }
    std::vector<std::uint16_t> out(px.size());
    const std::size_t ow = turn == 2 ? sw : sh;
    for (std::size_t y = 0; y < sh; ++y) {
        for (std::size_t x = 0; x < sw; ++x) {
            // Where source (x, y) lands, turned clockwise.
            std::size_t ox = x, oy = y;
            if (turn == 1) {
                ox = sh - 1 - y;
                oy = x;
            } else if (turn == 2) {
                ox = sw - 1 - x;
                oy = sh - 1 - y;
            } else {
                ox = y;
                oy = sw - 1 - x;
            }
            for (std::size_t c = 0; c < 3; ++c) {
                out[(oy * ow + ox) * 3 + c] = px[(y * sw + x) * 3 + c];
            }
        }
    }
    if (turn != 2) {
        std::swap(w, h);
    }
    return out;
}

void to_grey(std::span<std::uint16_t> rgb) {
    for (std::size_t i = 0; i + 2 < rgb.size(); i += 3) {
        const auto y = static_cast<std::uint16_t>(
            std::lround(0.2126 * rgb[i] + 0.7152 * rgb[i + 1] + 0.0722 * rgb[i + 2]));
        rgb[i] = rgb[i + 1] = rgb[i + 2] = y;
    }
}

} // namespace pakon::process
