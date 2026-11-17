#include "pakon/process/frames.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace pakon::process {

namespace {

std::vector<double> numbers(const Json& j) {
    std::vector<double> out;
    if (const auto* a = j.array()) {
        out.reserve(a->size());
        for (const auto& v : *a) {
            out.push_back(v.number().value_or(0.0));
        }
    }
    return out;
}

} // namespace

Result<ScanInfo> scan_info_from_sidecar(const Json& s) {
    ScanInfo info;
    if (s["schema"].string() != std::optional<std::string>("pakon-scan/1") ||
        s["kind"].string() != std::optional<std::string>("film")) {
        return failure<ScanInfo>(ErrorKind::image_bad_input,
                                 "sidecar is not a pakon-scan/1 film window");
    }
    info.mode = s["mode"].string().value_or("");
    const auto& img = s["image"];
    info.film_start = static_cast<std::size_t>(img["film_start_line"].number().value_or(0));
    if (const auto e = img["film_end_line"].number()) {
        info.film_end = static_cast<std::size_t>(*e);
    }
    info.motor_rate = s["motor_rate"].number().value_or(0);
    info.line_period_ms = img["line_period_ms"].number().value_or(0);
    const auto& film = s["geometry"]["film"];
    const auto& cal = s["geometry"]["calibration"];
    info.pixels = static_cast<std::size_t>(film["pixels"].number().value_or(0));
    info.ir = film["ir"].boolean().value_or(false);
    info.reversal = s["film"].string() == std::optional<std::string>("e6");
    // Both windows share the resample: at Base 8 (3/4) the CCD distance
    // between the window starts shrinks with it (strip8: (70 - 6) x 3/4 =
    // 48; 1548 calibration vs 1500 film pixels, 2026-10-22).
    double shift = film["start"].number().value_or(0) - cal["start"].number().value_or(0);
    if (film["resample"].boolean().value_or(false)) {
        shift = shift * 3 / 4;
    }
    info.reference_shift = shift > 0 ? static_cast<std::size_t>(shift) : 0;
    const auto& c = s["corrections"];
    for (std::size_t ch = 0; ch < 3; ++ch) {
        info.dark[ch] = numbers(c["dark"]["planes"][ch]);
        info.white[ch] = numbers(c["white"]["planes"][ch]);
        const char* key = ch == 0 ? "r" : ch == 1 ? "g" : "b";
        info.calibration_on_time[ch] = c["on_time"][key].number().value_or(0);
        info.film_on_time[ch] = c["film_on_time"][key].number().value_or(0);
    }
    if (info.ir) {
        info.dark_ir = numbers(c["dark"]["planes"][3]);
        info.white_ir = numbers(c["white"]["planes"][3]);
    }
    if (const auto nm = numbers(s["unit"]["neg_matrix"]); nm.size() == 30) {
        std::array<double, 30> m{};
        std::copy(nm.begin(), nm.end(), m.begin());
        info.neg_matrix = m;
    }
    info.dark_green = info.dark[1];
    info.white_green = info.white[1];
    if (info.pixels == 0 || info.motor_rate <= 0 || info.line_period_ms <= 0 ||
        info.dark_green.size() < info.pixels + info.reference_shift ||
        info.white_green.size() < info.pixels + info.reference_shift) {
        return failure<ScanInfo>(ErrorKind::image_bad_input,
                                 "sidecar lacks the film geometry, motor rate, line period or "
                                 "the dark/white references");
    }
    return info;
}

Result<std::vector<LineStats>> line_profile(PakrawReader& raw, const ScanInfo& info) {
    const std::size_t channels = info.ir ? 4 : 3;
    if (raw.samples_per_row() != info.pixels * channels) {
        return failure<std::vector<LineStats>>(
            ErrorKind::image_bad_input,
            std::format("{} samples per row, sidecar says {} pixels x {}", raw.samples_per_row(),
                        info.pixels, channels));
    }
    std::array<std::vector<double>, 3> rgb_dark, rgb_span;
    const bool have_rgb = info.dark[0].size() >= info.pixels + info.reference_shift &&
                          info.dark[2].size() >= info.pixels + info.reference_shift &&
                          info.white[0].size() >= info.pixels + info.reference_shift &&
                          info.white[2].size() >= info.pixels + info.reference_shift;
    for (std::size_t c = 0; c < 3; ++c) {
        const auto& dv = have_rgb ? info.dark[c] : info.dark_green;
        const auto& wv = have_rgb ? info.white[c] : info.white_green;
        for (std::size_t p = 0; p < info.pixels; ++p) {
            const double d = dv[p + info.reference_shift];
            rgb_dark[c].push_back(d);
            rgb_span[c].push_back(std::max(1.0, wv[p + info.reference_shift] - d));
        }
    }
    std::vector<std::size_t> idx;
    std::vector<double> dark, span;
    for (std::size_t p = info.pixels / 10; p < info.pixels - info.pixels / 10; p += 2) {
        idx.push_back(p);
        const double d = info.dark_green[p + info.reference_shift];
        dark.push_back(d);
        span.push_back(std::max(1.0, info.white_green[p + info.reference_shift] - d));
    }
    std::vector<std::uint16_t> row(raw.samples_per_row());
    std::vector<double> t(idx.size());
    std::vector<LineStats> out;
    out.reserve(static_cast<std::size_t>(raw.rows()));
    for (std::uint64_t r = 0; r < raw.rows(); ++r) {
        if (auto rr = raw.read_row(r, row); !rr) {
            return rr.error();
        }
        double sum = 0;
        for (std::size_t k = 0; k < idx.size(); ++k) {
            t[k] = (row[3 * idx[k] + 1] - dark[k]) / span[k];
            sum += t[k];
        }
        const auto n = t.size();
        std::nth_element(t.begin(), t.begin() + static_cast<std::ptrdiff_t>(n / 10), t.end());
        const double p10 = t[n / 10];
        std::nth_element(t.begin(), t.begin() + static_cast<std::ptrdiff_t>(9 * n / 10), t.end());
        const double p90 = t[9 * n / 10];
        double rgb = 0;
        for (std::size_t c = 0; c < 3; ++c) {
            for (std::size_t p = 0; p < info.pixels; ++p) {
                // TLB's 8-bit line value: the 14-bit code (open gate 16000)
                // shifted right by 6.
                const double code = 16000.0 * (row[3 * p + c] - rgb_dark[c][p]) / rgb_span[c][p];
                rgb += std::floor(std::clamp(code, 0.0, 16383.0) / 64.0);
            }
        }
        out.push_back({static_cast<float>(sum / static_cast<double>(n)), static_cast<float>(p10),
                       static_cast<float>(p90),
                       static_cast<float>(rgb / static_cast<double>(3 * info.pixels))});
    }
    return out;
}

namespace {

struct Run {
    std::size_t first{0};
    std::size_t width{0};
};
struct RunCounts {
    std::size_t nice{0};
    std::size_t lng{0};
    std::size_t shrt{0};
};

// Start threshold from the metric histogram [DIS TLB@10005d20, mode 0].
int histogram_threshold(const std::array<std::size_t, 256>& hist, std::size_t bins) {
    const std::size_t lim = bins / 150;
    std::size_t peak = 0;
    std::size_t at = 0;
    for (std::size_t i = 0; i < 0xfa; ++i) {
        if (lim < peak && hist[i] < peak * 3 / 5) {
            break;
        }
        if (peak < hist[i]) {
            peak = hist[i];
            at = i;
        }
    }
    std::size_t lo = at;
    while (lo != 0 && peak / 20 <= hist[lo]) {
        --lo;
    }
    std::size_t hi = at;
    const std::size_t top = std::min<std::size_t>(255, at + 0x32);
    while (hi < top && peak / 10 <= hist[hi]) {
        ++hi;
    }
    if ((at - lo) * 4 < hi - at) {
        std::size_t t = at;
        while (t < 0xff && peak / 2 <= hist[t]) {
            ++t;
        }
        return static_cast<int>(t);
    }
    return static_cast<int>(hi + (hi - at) / 2);
}

} // namespace

std::size_t oem_frame_bin(std::string_view mode) {
    if (mode.starts_with("base16")) {
        return 8;
    }
    if (mode.starts_with("base8")) {
        return 6;
    }
    return 4;
}

Placed place_frames(std::span<const LineStats> profile, std::size_t film_start,
                    std::size_t film_end, std::size_t bin, std::size_t frame_bins,
                    std::size_t pitch_bins) {
    Placed out;
    const std::size_t T = frame_bins;
    const std::size_t P = std::max(pitch_bins, frame_bins + 2);
    const std::size_t kNiceLow = T * 95 / 100;
    const std::size_t kNiceHigh = T * 115 / 100;
    out.bin = std::max<std::size_t>(1, bin);
    const std::size_t n = std::min(film_end, profile.size()) / out.bin;
    std::vector<int> metric(n);
    for (std::size_t i = 0; i < n; ++i) {
        double s = 0;
        for (std::size_t k = 0; k < out.bin; ++k) {
            s += profile[i * out.bin + k].rgb;
        }
        metric[i] = 255 - static_cast<int>(s / static_cast<double>(out.bin));
    }
    // Film bins, clear of the leader / tail transition by 8 bins.
    constexpr std::size_t margin = 8;
    const std::size_t start = film_start / out.bin + margin;
    if (n < margin || start + P > n - margin) {
        return out;
    }
    const std::size_t end = n - margin; // exclusive
    std::array<std::size_t, 256> hist{};
    for (std::size_t i = start; i < end; ++i) {
        ++hist[static_cast<std::size_t>(std::clamp(metric[i], 0, 255))];
    }

    std::vector<std::uint8_t> ones(n, 0);
    std::vector<Run> runs;
    const auto split_at = [&](int t) {
        RunCounts c;
        runs.clear();
        for (std::size_t i = start; i < end; ++i) {
            ones[i] = metric[i] > t;
        }
        for (std::size_t i = start; i < end;) {
            if (!ones[i]) {
                ++i;
                continue;
            }
            std::size_t j = i;
            while (j < end && ones[j]) {
                ++j;
            }
            runs.push_back({i, j - i});
            const std::size_t w = j - i;
            if (w > kNiceLow && w < kNiceHigh) {
                ++c.nice;
            } else if (w >= kNiceHigh) {
                ++c.lng;
            } else {
                ++c.shrt;
            }
            i = j;
        }
        return c;
    };
    int best = histogram_threshold(hist, end - start);
    RunCounts cnt = split_at(best);
    // Raise while merged runs remain, lower while broken runs remain; a
    // step is kept while the nice count does not drop.
    for (int t = best; t > 0 && t <= 0xf9 && cnt.lng >= 1;) {
        const RunCounts c = split_at(t + 2);
        if (c.nice < cnt.nice) {
            break;
        }
        t += 2;
        best = t;
        cnt = c;
    }
    cnt = split_at(best);
    for (int t = best; t >= 0x1a && t <= 0xff && !(cnt.shrt < 1 && runs.size() > 1);) {
        const RunCounts c = split_at(t - 2);
        if (c.nice < cnt.nice) {
            break;
        }
        t -= 2;
        best = t;
        cnt = c;
    }
    split_at(best);
    out.threshold = best;

    const std::size_t play = P - T;
    const std::size_t g = play / 2;
    struct Slot {
        std::size_t width;
        Placement how;
    };
    std::vector<std::pair<std::size_t, Slot>> placed; // left bin -> slot
    const auto add = [&](std::size_t left, std::size_t width, Placement how) {
        placed.push_back({left, {width, how}});
    };
    // Signed, truncating like the OEM: a run shorter than the frame
    // starts the frame before the run.
    const auto shifted = [](std::size_t first, long excess) {
        return static_cast<std::size_t>(static_cast<long>(first) + excess / 3);
    };
    for (const auto& r : runs) {
        const long w = static_cast<long>(r.width);
        if (r.width > kNiceLow && r.width < kNiceHigh) {
            add(shifted(r.first, w - static_cast<long>(T)), T, Placement::nice);
        } else if (r.width > kNiceLow + P && r.width < kNiceHigh + P) {
            add(shifted(r.first, w - static_cast<long>(P + T)), T, Placement::nice);
            add(r.first + P, T, Placement::nice);
        }
    }
    const auto by_left = [](const auto& a, const auto& b) { return a.first < b.first; };
    std::sort(placed.begin(), placed.end(), by_left);
    const std::size_t nice = placed.size();
    for (std::size_t k = 0; k + 1 < nice; ++k) {
        const std::size_t a = placed[k].first;
        const std::size_t d = placed[k + 1].first - a;
        std::size_t m = d / P;
        if (d - m * P < P / 4 && m > 0) {
            --m;
        }
        for (std::size_t j = 1; j <= m; ++j) {
            add(a + j * d / (m + 1), T, Placement::between);
        }
    }
    // Best window of T bins starting in [s, s + play), ties to the middle.
    const auto window = [&](std::size_t s) {
        const std::size_t half = play / 2;
        std::size_t best_sum = 0, best_dist = half, best_i = half;
        bool any = false;
        for (std::size_t i = 0; i < play; ++i) {
            std::size_t sum = 0;
            for (std::size_t k = s + i; k < std::min(s + i + T, n); ++k) {
                sum += ones[k];
            }
            const std::size_t dist = i > half ? i - half : half - i;
            if (best_sum <= sum && (sum != best_sum || dist < best_dist)) {
                best_sum = sum;
                best_dist = dist;
                best_i = i;
                any = true;
            }
        }
        return s + (any && best_sum > 0 ? best_i : half);
    };
    if (nice > 0) {
        const auto [last_left, last_slot] = *std::max_element(placed.begin(), placed.end(), by_left);
        std::size_t x = last_left + last_slot.width + g;
        while (x < end && end - x >= g + P) {
            const std::size_t left = window(x);
            add(left, T, Placement::end);
            x = left + T + g;
        }
        if (x < end && end - x >= P / 2 + g) {
            const std::size_t left = x + play;
            add(left, std::min(T, end - left), Placement::partial);
        }
        const std::size_t first_left =
            std::min_element(placed.begin(), placed.end(), by_left)->first;
        x = first_left - std::min(first_left, g);
        while (x > start && x - start >= g + P) {
            const std::size_t left = window(x - P);
            add(left, T, Placement::beginning);
            x = left - g;
        }
        if (x > start && x - start >= P / 2 + g) {
            const std::size_t left = x - g > start + T ? x - g - T : start + 1;
            add(left, std::min(T, x - g - left), Placement::partial);
        }
    }
    std::sort(placed.begin(), placed.end(), by_left);
    for (const auto& [left, slot] : placed) {
        const std::size_t first = left * out.bin;
        const std::size_t last = std::min((left + slot.width) * out.bin, film_end);
        if (last > first) {
            out.frames.push_back({first, last, slot.how, FrameKind::picture});
        }
    }
    return out;
}

FrameSplit find_frames(std::span<const LineStats> profile, const ScanInfo& info) {
    FrameSplit fs;
    const std::size_t start = std::min(info.film_start, profile.size());
    const std::size_t end = std::min(info.film_end.value_or(profile.size()), profile.size());
    fs.pitch_seed = kFramePitchSecondsAtRate25676 * 25676.0 / info.motor_rate /
                    (info.line_period_ms / 1000.0);
    fs.pitch = fs.pitch_seed;
    if (end <= start + 1) {
        return fs;
    }

    // Base transmission: median mean of the uniform lines in the film.
    std::vector<double> uniform;
    for (std::size_t i = start; i < end; ++i) {
        const auto& l = profile[i];
        if (l.mean > 0 && (l.p90 - l.p10) / l.mean < 0.2f) {
            uniform.push_back(l.mean);
        }
    }
    if (uniform.empty()) {
        return fs;
    }
    std::nth_element(uniform.begin(), uniform.begin() + static_cast<std::ptrdiff_t>(uniform.size() / 2),
                     uniform.end());
    fs.base = uniform[uniform.size() / 2];

    // Gap runs.
    std::vector<GapRun> runs;
    for (std::size_t i = start; i < end;) {
        const auto is_gap = [&](std::size_t k) {
            return profile[k].p10 >= 0.85 * fs.base && profile[k].p90 <= 1.15 * fs.base;
        };
        if (!is_gap(i)) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < end && is_gap(j)) {
            ++j;
        }
        if (!runs.empty() && static_cast<double>(i - runs.back().last) < 0.03 * fs.pitch_seed) {
            runs.back().last = j;
        } else {
            runs.push_back({i, j});
        }
        i = j;
    }
    for (const auto& r : runs) {
        if (r.last - r.first >= 8) {
            fs.gaps.push_back(r);
        }
    }
    const auto centre = [](const GapRun& g) { return 0.5 * static_cast<double>(g.first + g.last); };

    // Fit phase and pitch to the gaps.
    double anchor = fs.gaps.empty() ? static_cast<double>(start) : centre(fs.gaps.front());
    double pitch = fs.pitch_seed;
    std::vector<std::pair<long, double>> best_hits;
    for (const auto& a : fs.gaps) {
        for (int k = -50; k <= 50; ++k) {
            const double p = fs.pitch_seed * (1.0 + k / 1000.0);
            std::vector<std::pair<long, double>> hits;
            for (const auto& g : fs.gaps) {
                const long n = std::lround((centre(g) - centre(a)) / p);
                if (std::abs(centre(g) - (centre(a) + static_cast<double>(n) * p)) <= 0.04 * p) {
                    hits.emplace_back(n, centre(g));
                }
            }
            if (hits.size() > best_hits.size()) {
                best_hits = std::move(hits);
                anchor = centre(a);
                pitch = p;
            }
        }
    }
    if (best_hits.size() >= 2) {
        double mn = 0, mc = 0;
        for (const auto& [n, c] : best_hits) {
            mn += static_cast<double>(n);
            mc += c;
        }
        mn /= static_cast<double>(best_hits.size());
        mc /= static_cast<double>(best_hits.size());
        double num = 0, den = 0;
        for (const auto& [n, c] : best_hits) {
            num += (static_cast<double>(n) - mn) * (c - mc);
            den += (static_cast<double>(n) - mn) * (static_cast<double>(n) - mn);
        }
        if (den > 0) {
            pitch = num / den;
            anchor = mc - pitch * mn;
        }
    }
    fs.pitch = pitch;
    fs.gaps_on_grid = best_hits.size();

    // The OEM frames 375 bins of 4 / 6 / 8 lines at Base 4 / 8 / 16 (frames
    // of 1500 / 2250 / 3000 lines; DxCode.txt Target 375 in each base, VM
    // proxy runs 2026-10-14 and 2026-10-22).
    const std::size_t bin = oem_frame_bin(info.mode);
    auto placed = place_frames(profile, start, end, bin);
    // Half frames leave bare base half way between the full-pitch gaps too.
    for (const auto& g : fs.gaps) {
        const double k = (centre(g) - anchor) / fs.pitch;
        fs.gaps_half_way += std::abs(k - std::floor(k) - 0.5) <= 0.04;
    }
    // Auto: half frames when most full-pitch gaps have a half-way partner.
    const bool half = info.frame_format == FrameFormat::half ||
                      (info.frame_format == FrameFormat::auto_detect && fs.gaps_on_grid >= 3 &&
                       2 * fs.gaps_half_way >= fs.gaps_on_grid);
    // Pano: neighbouring gaps clearly further apart than a full frame's
    // pitch (an XPan 24 x 65 frame is ~1.8 pitches) [INFERRED, no pano scan yet].
    std::vector<double> spacing;
    for (std::size_t k = 1; k < fs.gaps.size(); ++k) {
        spacing.push_back(centre(fs.gaps[k]) - centre(fs.gaps[k - 1]));
    }
    if (!spacing.empty()) {
        std::nth_element(spacing.begin(), spacing.begin() + static_cast<std::ptrdiff_t>(spacing.size() / 2), spacing.end());
        fs.gap_pitch = spacing[spacing.size() / 2];
    }
    const bool pano = !half && (info.frame_format == FrameFormat::pano ||
                                (info.frame_format == FrameFormat::auto_detect && spacing.size() >= 3 &&
                                 fs.gap_pitch > 1.35 * fs.pitch_seed));
    if (half) {
        placed = place_frames(profile, start, end, bin, kHalfFrameBins, kHalfPitchBins);
        fs.half = true;
    } else if (pano && fs.gaps.size() >= 2) {
        // Gap to gap: every stretch between two gaps is one picture; the
        // pieces before the first and after the last gap count when they
        // are at least 80 % of the typical picture.
        placed.frames.clear();
        std::vector<double> lens;
        for (std::size_t k = 1; k < fs.gaps.size(); ++k) {
            lens.push_back(static_cast<double>(fs.gaps[k].first - fs.gaps[k - 1].last));
        }
        std::nth_element(lens.begin(), lens.begin() + static_cast<std::ptrdiff_t>(lens.size() / 2), lens.end());
        const double typical = lens[lens.size() / 2];
        const auto add = [&](std::size_t a, std::size_t b, Placement how) {
            if (b > a && static_cast<double>(b - a) >= 0.8 * typical) {
                placed.frames.push_back({a, b, how, FrameKind::picture});
            }
        };
        add(start, fs.gaps.front().first, Placement::beginning);
        for (std::size_t k = 1; k < fs.gaps.size(); ++k) {
            add(fs.gaps[k - 1].last, fs.gaps[k].first, Placement::nice);
        }
        add(fs.gaps.back().last, end, Placement::end);
        fs.pano = true;
    }
    fs.bin = placed.bin;
    fs.threshold = placed.threshold;
    fs.frames = std::move(placed.frames);

    for (auto& f : fs.frames) {
        std::size_t base_like = 0;
        std::vector<float> p90;
        p90.reserve(f.last - f.first);
        for (std::size_t i = f.first; i < f.last; ++i) {
            const auto& l = profile[i];
            base_like += l.p10 >= 0.85 * fs.base && l.p90 <= 1.15 * fs.base;
            p90.push_back(l.p90);
        }
        std::nth_element(p90.begin(), p90.begin() + static_cast<std::ptrdiff_t>(p90.size() / 2),
                         p90.end());
        if (static_cast<double>(base_like) >= 0.8 * static_cast<double>(f.last - f.first)) {
            f.kind = FrameKind::blank;
        } else if (p90[p90.size() / 2] < 0.08 * fs.base) {
            f.kind = FrameKind::fogged;
        }
    }
    return fs;
}

std::string_view to_string(Placement p) {
    switch (p) {
    case Placement::nice: return "nice";
    case Placement::between: return "between";
    case Placement::end: return "end";
    case Placement::beginning: return "beginning";
    case Placement::partial: return "partial";
    }
    return "?";
}

std::string_view to_string(FrameKind k) {
    switch (k) {
    case FrameKind::picture: return "picture";
    case FrameKind::blank: return "blank";
    case FrameKind::fogged: return "fogged";
    }
    return "?";
}

void shift_frames(FrameSplit& split, long long lines, std::size_t rows) {
    const auto move = [&](std::size_t at) {
        const long long to = static_cast<long long>(at) + lines;
        return static_cast<std::size_t>(std::clamp<long long>(to, 0, static_cast<long long>(rows)));
    };
    for (auto& f : split.frames) {
        f.first = move(f.first);
        f.last = std::max(move(f.last), std::min(f.first + 1, rows));
    }
}

} // namespace pakon::process
