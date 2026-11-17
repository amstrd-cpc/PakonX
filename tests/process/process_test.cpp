// Offline tests for pakon_process: the sidecar JSON reader, the
// row-by-row .pakraw reader, and frame separation on synthetic profiles.

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "pakon/image/bmp.hpp"
#include "pakon/image/raw_writer.hpp"
#include "pakon/process/frames.hpp"
#include "pakon/process/dx.hpp"
#include "pakon/process/json.hpp"
#include "pakon/process/look.hpp"
#include "pakon/process/pakraw.hpp"
#include "pakon/process/filmid.hpp"
#include "pakon/process/ice.hpp"
#include "pakon/process/positive.hpp"
#include "support/test_harness.hpp"

namespace {

using namespace pakon;
using namespace pakon::process;

std::string temp_path(const char* name) {
    auto path = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path.string();
}

// Pitch seed exactly `pitch` lines: line period chosen to match.
ScanInfo info_for_pitch(double pitch, std::size_t start, std::optional<std::size_t> end) {
    ScanInfo i;
    i.mode = "base4";
    i.film_start = start;
    i.film_end = end;
    i.motor_rate = 25676;
    i.line_period_ms = kFramePitchSecondsAtRate25676 / pitch * 1000.0;
    i.pixels = 1000;
    return i;
}

constexpr LineStats kBase{0.15f, 0.145f, 0.156f, 37.5f}; // bare film base (rgb: 8-bit codes)

// A strip: leader, then frames of `pitch` lines with `gap` lines of bare
// base between them; `hidden` gap indices look like picture (no gap).
std::vector<LineStats> strip(std::size_t lead, std::size_t frames, std::size_t pitch,
                             std::size_t gap, std::vector<std::size_t> hidden = {}) {
    std::vector<LineStats> p(lead, LineStats{1.0f, 0.98f, 1.02f, 250.0f}); // open gate
    for (std::size_t f = 0; f < frames; ++f) {
        const bool show = std::find(hidden.begin(), hidden.end(), f) == hidden.end();
        for (std::size_t k = 0; k < gap; ++k) {
            p.push_back(show ? kBase : LineStats{0.09f, 0.02f, 0.14f, 22.5f});
        }
        for (std::size_t k = gap; k < pitch; ++k) {
            p.push_back(LineStats{0.08f, 0.02f, 0.14f, 20.0f});
        }
    }
    for (std::size_t k = 0; k < gap; ++k) {
        p.push_back(kBase);
    }
    return p;
}

} // namespace

PAKON_TEST(json_reads_the_sidecar_shapes) {
    auto j = parse_json(R"({"schema":"pakon-scan/1","n":-1.5e2,"ok":true,"none":null,
                            "a":[[1,2],[3]],"o":{"s":"x\"y"}})");
    EXPECT(j.has_value());
    if (!j) {
        return;
    }
    EXPECT((*j)["schema"].string() == std::optional<std::string>("pakon-scan/1"));
    EXPECT((*j)["n"].number() == std::optional<double>(-150.0));
    EXPECT((*j)["ok"].boolean() == std::optional<bool>(true));
    EXPECT((*j)["none"].is_null());
    EXPECT((*j)["a"][0][1].number() == std::optional<double>(2.0));
    EXPECT((*j)["o"]["s"].string() == std::optional<std::string>("x\"y"));
    EXPECT((*j)["missing"]["deeper"].is_null());
    EXPECT(!parse_json("{\"a\":1,}").has_value());
    EXPECT(!parse_json("[1,2").has_value());
}

PAKON_TEST(pakraw_reader_reads_finished_and_unfinished_files) {
    const auto path = temp_path("pakon_process_reader.pakraw");
    {
        auto w = image::StreamRawWriter::open(path, 6, {});
        EXPECT(w.has_value());
        if (!w) {
            return;
        }
        for (std::uint16_t r = 0; r < 5; ++r) {
            const std::vector<std::uint16_t> row{r, 1, 2, 3, 4, static_cast<std::uint16_t>(65000 + r)};
            EXPECT((*w)->append(row).has_value());
        }
        EXPECT((*w)->finish().has_value());
    }
    auto r = PakrawReader::open(path);
    EXPECT(r.has_value());
    if (!r) {
        return;
    }
    EXPECT_EQ((*r)->rows(), 5u);
    std::vector<std::uint16_t> row(6);
    EXPECT((*r)->read_row(3, row).has_value());
    EXPECT_EQ(row[0], 3u);
    EXPECT_EQ(row[5], 65003u);
    EXPECT((*r)->read_row(1, row).has_value()); // backwards seek
    EXPECT_EQ(row[0], 1u);
    EXPECT(!(*r)->read_row(5, row).has_value());

    // An interrupted scan leaves rows = 0 in the header: the size counts.
    {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(0x10);
        const char zero[4] = {0, 0, 0, 0};
        f.write(zero, 4);
    }
    auto u = PakrawReader::open(path);
    EXPECT(u.has_value());
    if (u) {
        EXPECT_EQ((*u)->rows(), 5u);
    }
}

PAKON_TEST(frames_land_where_the_oem_places_them) {
    // 4-line bins: each picture is a run of 380 bins (1520 lines), so the
    // OEM places a 375-bin frame (380 - 375) / 3 = 1 bin into it.
    const auto p = strip(300, 6, 1600, 80);
    const auto split = find_frames(p, info_for_pitch(1630, 300, std::nullopt));
    EXPECT(std::abs(split.pitch - 1600.0) < 2.0);
    EXPECT(std::abs(split.base - 0.15) < 0.01);
    EXPECT_EQ(split.bin, 4u);
    EXPECT_EQ(split.frames.size(), 6u);
    for (std::size_t f = 0; f < split.frames.size(); ++f) {
        EXPECT_EQ(split.frames[f].first, 300 + 80 + 4 + f * 1600);
        EXPECT_EQ(split.frames[f].last - split.frames[f].first, 1500u);
        EXPECT(split.frames[f].placement == Placement::nice);
    }
}

PAKON_TEST(half_frames_are_found_by_their_half_way_gaps) {
    // Half frames: a picture of 728 lines every 800 (half the full pitch).
    const auto p = strip(300, 12, 800, 72);
    auto info = info_for_pitch(1630, 300, std::nullopt);
    info.frame_format = FrameFormat::auto_detect;
    const auto split = find_frames(p, info);
    EXPECT(split.half);
    EXPECT(split.gaps_half_way >= 4u);
    EXPECT_EQ(split.frames.size(), 12u);
    for (const auto& f : split.frames) {
        EXPECT_EQ(f.last - f.first, kHalfFrameBins * 4);
    }
    // Full-frame film stays full under auto; half forces the half placement.
    auto full = info_for_pitch(1630, 300, std::nullopt);
    full.frame_format = FrameFormat::auto_detect;
    const auto f6 = find_frames(strip(300, 6, 1600, 80), full);
    EXPECT(!f6.half && f6.frames.size() == 6u);
    info.frame_format = FrameFormat::full;
    EXPECT(!find_frames(p, info).half);
}

PAKON_TEST(panoramic_frames_are_cut_gap_to_gap) {
    // XPan-like: a picture every 2900 lines (1.8 full-frame pitches).
    const auto p = strip(300, 5, 2900, 80);
    auto info = info_for_pitch(1630, 300, std::nullopt);
    info.frame_format = FrameFormat::auto_detect;
    const auto split = find_frames(p, info);
    EXPECT(split.pano && !split.half);
    EXPECT(std::abs(split.gap_pitch - 2900.0) < 5.0);
    EXPECT_EQ(split.frames.size(), 5u);
    for (const auto& f : split.frames) {
        EXPECT(f.last - f.first > 2700 && f.last - f.first <= 2820);
    }
    // Full-frame film stays full under auto.
    EXPECT(!find_frames(strip(300, 6, 1600, 80), info).pano);
}

PAKON_TEST(dx_codes_give_the_oem_iso_speed) {
    EXPECT(dx_iso(2, 0) == 100);   // first row of the Ansel table with speeds
    EXPECT(dx_iso(2, 10) == 1600);
    EXPECT(!dx_iso(0, 0));         // no speed listed
    EXPECT(!dx_iso(128, 0) && !dx_iso(5, 16) && !dx_iso(-1, 0));
}

PAKON_TEST(a_picture_shorter_than_the_frame_starts_the_frame_early) {
    // Runs of 372 bins (1488 lines): (372 - 375) / 3 = -1 bin, signed as
    // in the OEM (river1 frame 6 has such a run).
    const auto p = strip(300, 6, 1600, 112);
    const auto split = find_frames(p, info_for_pitch(1600, 300, std::nullopt));
    EXPECT_EQ(split.frames.size(), 6u);
    for (std::size_t f = 0; f < split.frames.size(); ++f) {
        EXPECT_EQ(split.frames[f].first, 300 + 112 - 4 + f * 1600);
    }
}

PAKON_TEST(a_frame_without_a_visible_gap_still_lands_on_the_pitch) {
    // Gap before frame 4 looks like picture (a dark scene edge to edge):
    // frames 3 and 4 form one run of two, placed one pitch apart.
    const auto p = strip(300, 6, 1600, 80, {3});
    const auto split = find_frames(p, info_for_pitch(1600, 300, std::nullopt));
    EXPECT_EQ(split.frames.size(), 6u);
    if (split.frames.size() == 6) {
        const double edge = static_cast<double>(split.frames[3].first);
        EXPECT(std::abs(edge - (300 + 80 + 4 + 3 * 1600)) <= 8.0);
        EXPECT_EQ(split.frames[3].first - split.frames[2].first, 1600u);
    }
}

PAKON_TEST(walking_from_the_last_nice_frame_fills_a_thin_tail) {
    // From frame 5 on the film is thin (just above base, gaps included):
    // one over-long run, so the threshold rises above it. Frames 5 and 6
    // then come from walking on from frame 4, one pitch at a time.
    auto p = strip(300, 6, 1600, 80);
    for (std::size_t i = 300 + 4 * 1600; i < p.size(); ++i) {
        p[i] = LineStats{0.12f, 0.02f, 0.14f, 30.0f};
    }
    const auto split = find_frames(p, info_for_pitch(1600, 300, std::nullopt));
    std::size_t walked = 0;
    for (const auto& f : split.frames) {
        walked += f.placement == Placement::end || f.placement == Placement::partial;
    }
    EXPECT(walked >= 2u);
    EXPECT(split.frames.size() >= 6u);
}

PAKON_TEST(fogged_leader_and_blank_tail_are_not_pictures) {
    auto p = strip(300, 6, 1600, 80);
    // Frame 1 fogged (nearly opaque), frame 6 blank (bare base).
    for (std::size_t i = 300 + 80; i < 300 + 1600; ++i) {
        p[i] = LineStats{0.004f, 0.002f, 0.006f, 1.0f};
    }
    for (std::size_t i = 300 + 5 * 1600 + 80; i < 300 + 6 * 1600; ++i) {
        p[i] = kBase;
    }
    const auto split = find_frames(p, info_for_pitch(1600, 300, std::nullopt));
    std::size_t pictures = 0;
    for (const auto& f : split.frames) {
        pictures += f.kind == FrameKind::picture;
    }
    EXPECT_EQ(pictures, 4u);
    if (!split.frames.empty()) {
        EXPECT(split.frames.front().kind == FrameKind::fogged);
    }
    // The blank frame merges with its gaps; the partial frame placed over
    // it must not be a picture.
    for (const auto& f : split.frames) {
        if (f.first >= 300 + 5 * 1600) {
            EXPECT(f.kind != FrameKind::picture);
        }
    }
}

// Positive on a tiny synthetic window: flat field, base from the gap,
// denser negative -> brighter positive, base -> black, and the vertical
// flip of the CCD pixel axis.
PAKON_TEST(positive_inverts_densities_above_the_base_and_orients_by_feed) {
    constexpr std::size_t px = 10;
    const auto path = temp_path("pakon_process_positive.pakraw");
    {
        auto w = image::StreamRawWriter::open(path, 3 * px, {});
        EXPECT(w.has_value());
        if (!w) {
            return;
        }
        std::vector<std::uint16_t> row(3 * px);
        // Lines 0..99: bare base, t = 0.4 in every channel.
        for (int r = 0; r < 100; ++r) {
            std::fill(row.begin(), row.end(), static_cast<std::uint16_t>(0.4 * 60000));
            EXPECT((*w)->append(row).has_value());
        }
        // Lines 100..199: picture, density rising with the pixel index.
        for (int r = 0; r < 100; ++r) {
            for (std::size_t p = 0; p < px; ++p) {
                const double t = 0.4 * std::pow(10.0, -0.1 * static_cast<double>(p));
                for (std::size_t c = 0; c < 3; ++c) {
                    row[3 * p + c] = static_cast<std::uint16_t>(t * 60000);
                }
            }
            EXPECT((*w)->append(row).has_value());
        }
        EXPECT((*w)->finish().has_value());
    }
    auto raw = PakrawReader::open(path);
    EXPECT(raw.has_value());
    if (!raw) {
        return;
    }
    ScanInfo info = info_for_pitch(100, 0, std::nullopt);
    info.pixels = px;
    for (std::size_t c = 0; c < 3; ++c) {
        info.dark[c].assign(px, 0.0);
        info.white[c].assign(px, 60000.0);
        info.calibration_on_time[c] = 0.5;
        info.film_on_time[c] = 0.5;
    }
    FrameSplit split;
    split.gaps.push_back({0, 100});
    split.frames.push_back({100, 200, Placement::nice, FrameKind::picture});
    auto pp = measure_positive(**raw, info, split, {});
    EXPECT(pp.has_value());
    if (!pp) {
        return;
    }
    EXPECT(std::abs(pp->base[1] - 0.4) < 0.01);
    auto img = develop_frame(**raw, info, *pp, split.frames[0], Feed::tail_first);
    EXPECT(img.has_value());
    if (!img) {
        return;
    }
    const std::size_t w = 100;
    // Output row y holds CCD pixel px-1-y: the densest pixel (9) is the
    // brightest and lands in row 0; the thinnest (0, at base) is black.
    const auto at = [&](std::size_t y) { return (*img)[(y * w + 50) * 3 + 1]; };
    EXPECT(at(0) > 60000);
    EXPECT(at(px - 1) < 1000);
    for (std::size_t y = 1; y < px; ++y) {
        EXPECT(at(y) <= at(y - 1));
    }
    // Head first: the pixel axis as is, so the densest pixel is the last row.
    auto head = develop_frame(**raw, info, *pp, split.frames[0], Feed::head_first);
    EXPECT(head.has_value());
    if (head) {
        EXPECT((*head)[((px - 1) * w + 50) * 3 + 1] > 60000);
        EXPECT((*head)[(0 * w + 50) * 3 + 1] < 1000);
    }
    // The user's look: none changes nothing; +1 stop brightens the mid
    // tones; a blue balance moves blue alone.
    auto same = develop_frame(**raw, info, *pp, split.frames[0], Feed::tail_first, Adjust{});
    EXPECT(same.has_value() && *same == *img);
    Adjust brighter;
    brighter.exposure = 1.0;
    auto up = develop_frame(**raw, info, *pp, split.frames[0], Feed::tail_first, brighter);
    Adjust bluer;
    bluer.balance = {0.0, 0.0, 0.5};
    auto blue = develop_frame(**raw, info, *pp, split.frames[0], Feed::tail_first, bluer);
    EXPECT(up.has_value() && blue.has_value());
    if (up && blue) {
        const std::size_t mid = ((px / 2) * w + 50) * 3;
        EXPECT((*up)[mid + 1] > (*img)[mid + 1] + 1000);
        EXPECT_EQ((*blue)[mid + 0], (*img)[mid + 0]);
        EXPECT_EQ((*blue)[mid + 1], (*img)[mid + 1]);
        EXPECT((*blue)[mid + 2] > (*img)[mid + 2]);
    }
    // The negative for other converters: flat-fielded transmission, the
    // mask kept: the bare-base pixel 0 is t = 0.4, pixel p 0.4 x 10^-0.1p.
    auto negp = *pp;
    negp.negative = true;
    auto neg = develop_frame(**raw, info, negp, split.frames[0], Feed::head_first);
    EXPECT(neg.has_value());
    if (neg) {
        EXPECT(std::abs((*neg)[(0 * w + 50) * 3 + 1] - 0.4 * 65535) < 50);
        EXPECT(std::abs((*neg)[(5 * w + 50) * 3 + 1] - 0.4 * std::pow(10.0, -0.5) * 65535) < 50);
    }
}

// A frame with no white in it (only thin densities) is brightened by at
// most max_exposure_lift, not up to the darkening limit.
PAKON_TEST(per_frame_exposure_lifts_a_dark_frame_only_a_little) {
    constexpr std::size_t px = 10;
    const auto path = temp_path("pakon_process_lift.pakraw");
    {
        auto w = image::StreamRawWriter::open(path, 3 * px, {});
        EXPECT(w.has_value());
        if (!w) {
            return;
        }
        std::vector<std::uint16_t> row(3 * px);
        const auto put = [&](double step) {
            for (std::size_t p = 0; p < px; ++p) {
                const double t = 0.4 * std::pow(10.0, -step * static_cast<double>(p));
                for (std::size_t c = 0; c < 3; ++c) {
                    row[3 * p + c] = static_cast<std::uint16_t>(t * 60000);
                }
            }
            EXPECT((*w)->append(row).has_value());
        };
        for (int r = 0; r < 100; ++r) {
            std::fill(row.begin(), row.end(), static_cast<std::uint16_t>(0.4 * 60000));
            EXPECT((*w)->append(row).has_value());
        }
        for (int r = 0; r < 100; ++r) put(0.2);  // frame 1: full range
        for (int r = 0; r < 100; ++r) put(0.06); // frame 2: thin, no white
        EXPECT((*w)->finish().has_value());
    }
    auto raw = PakrawReader::open(path);
    EXPECT(raw.has_value());
    if (!raw) {
        return;
    }
    ScanInfo info = info_for_pitch(100, 0, std::nullopt);
    info.pixels = px;
    for (std::size_t c = 0; c < 3; ++c) {
        info.dark[c].assign(px, 0.0);
        info.white[c].assign(px, 60000.0);
        info.calibration_on_time[c] = 0.5;
        info.film_on_time[c] = 0.5;
    }
    FrameSplit split;
    split.gaps.push_back({0, 100});
    split.frames.push_back({100, 200, Placement::nice, FrameKind::picture});
    split.frames.push_back({200, 300, Placement::nice, FrameKind::picture});
    auto pp = measure_positive(**raw, info, split, {});
    EXPECT(pp.has_value());
    if (!pp) {
        return;
    }
    const auto brightest = [&](const PositiveParams& p) {
        auto img = develop_frame(**raw, info, p, split.frames[1], Feed::head_first);
        EXPECT(img.has_value());
        return img ? *std::max_element(img->begin(), img->end()) : std::uint16_t{0};
    };
    auto fixed = *pp;
    fixed.per_frame_exposure = false;
    auto old = *pp;
    old.max_exposure_lift = old.max_exposure_shift;
    const auto none = brightest(fixed), now = brightest(*pp), before = brightest(old);
    EXPECT(none < now && now < before);
}

PAKON_TEST(slide_develops_without_inversion_or_gap_lines) {
    constexpr std::size_t px = 10;
    const auto path = temp_path("pakon_process_slide.pakraw");
    {
        auto w = image::StreamRawWriter::open(path, 3 * px, {});
        EXPECT(w.has_value());
        if (!w) {
            return;
        }
        // 100 picture lines, density rising with the pixel index.
        std::vector<std::uint16_t> row(3 * px);
        for (int r = 0; r < 100; ++r) {
            for (std::size_t p = 0; p < px; ++p) {
                const double t = 0.9 * std::pow(10.0, -0.25 * static_cast<double>(p));
                for (std::size_t c = 0; c < 3; ++c) {
                    row[3 * p + c] = static_cast<std::uint16_t>(t * 60000);
                }
            }
            EXPECT((*w)->append(row).has_value());
        }
        EXPECT((*w)->finish().has_value());
    }
    auto raw = PakrawReader::open(path);
    EXPECT(raw.has_value());
    if (!raw) {
        return;
    }
    ScanInfo info = info_for_pitch(100, 0, std::nullopt);
    info.pixels = px;
    info.reversal = true;
    for (std::size_t c = 0; c < 3; ++c) {
        info.dark[c].assign(px, 0.0);
        info.white[c].assign(px, 60000.0);
        info.calibration_on_time[c] = info.film_on_time[c] = 0.5;
    }
    FrameSplit split; // no gaps: a slide's frame lines are opaque, not base
    split.frames.push_back({0, 100, Placement::nice, FrameKind::picture});
    auto pp = measure_positive(**raw, info, split, {});
    EXPECT(pp.has_value());
    if (!pp) {
        return;
    }
    EXPECT(pp->reversal && pp->base[0] == 1.0);
    auto img = develop_frame(**raw, info, *pp, split.frames[0], Feed::head_first);
    EXPECT(img.has_value());
    if (!img) {
        return;
    }
    // Head first: row y is pixel y. The thinnest pixel is the brightest.
    const auto at = [&](std::size_t y) { return (*img)[(y * 100 + 50) * 3 + 1]; };
    EXPECT(at(0) > 60000);
    EXPECT(at(px - 1) < 1000);
    for (std::size_t y = 1; y < px; ++y) {
        EXPECT(at(y) <= at(y - 1));
    }
    // Missing sidecar film: a negative, and a negative needs its gap lines.
    info.reversal = false;
    EXPECT(!measure_positive(**raw, info, split, {}).has_value());
}

PAKON_TEST(ice_fills_an_infrared_defect_in_each_channel_and_spares_silver) {
    constexpr std::size_t px = 40, lines = 60, s = 3;
    ScanInfo info;
    info.pixels = px;
    info.ir = true;
    for (std::size_t c = 0; c < 3; ++c) {
        info.dark[c].assign(px, 0.0);
        info.white[c].assign(px, 60000.0);
    }
    info.dark_ir.assign(px, 0.0);
    info.white_ir.assign(px, 60000.0);
    // Film: a gentle picture (varies by line and pixel), IR flat at 0.9
    // with a little noise; a dust speck at IR line 20, pixels 10..11, and
    // the same speck in R at line 20, G at 23, B at 26 (s = 3).
    std::vector<std::uint16_t> rows(lines * 4 * px);
    const auto picture = [](std::size_t r, std::size_t p, std::size_t c) {
        return 20000.0 + 3000.0 * std::sin(0.1 * static_cast<double>(r + 2 * p + 5 * c));
    };
    for (std::size_t r = 0; r < lines; ++r) {
        for (std::size_t p = 0; p < px; ++p) {
            for (std::size_t c = 0; c < 3; ++c) {
                const bool dust = r == 20 + c * s && (p == 10 || p == 11);
                rows[r * 4 * px + 3 * p + c] = static_cast<std::uint16_t>(picture(r, p, c) * (dust ? 0.3 : 1.0));
            }
            const double noise = 60.0 * static_cast<double>((r * 7 + p * 13) % 5) - 120.0;
            const bool dust = r == 20 && (p == 10 || p == 11);
            rows[r * 4 * px + 3 * px + p] = static_cast<std::uint16_t>((dust ? 18000.0 : 54000.0) + noise);
        }
    }
    auto silver = rows;
    const auto rep = ice_correct(rows, lines, info, static_cast<int>(s), IceStrength::normal);
    EXPECT(rep.ran && !rep.silver);
    EXPECT(rep.defects >= 2);
    for (std::size_t c = 0; c < 3; ++c) {
        const double v = rows[(20 + c * s) * 4 * px + 3 * 10 + c];
        EXPECT(std::abs(v - picture(20 + c * s, 10, c)) < 2500.0); // filled near the picture
    }
    // A clean pixel far from the speck is untouched.
    EXPECT_EQ(rows[40 * 4 * px + 3 * 30 + 1], static_cast<std::uint16_t>(picture(40, 30, 1)));
    // Off does nothing.
    auto same = silver;
    EXPECT(!ice_correct(same, lines, info, static_cast<int>(s), IceStrength::off).ran && same == silver);

    // Silver: the IR copies the green picture, so nothing is corrected.
    for (std::size_t r = 0; r < lines; ++r) {
        for (std::size_t p = 0; p < px; ++p) {
            silver[r * 4 * px + 3 * px + p] = silver[r * 4 * px + 3 * p + 1];
        }
    }
    auto before = silver;
    const auto sv = ice_correct(silver, lines, info, static_cast<int>(s), IceStrength::strong);
    EXPECT(sv.silver && silver == before);
    EXPECT(parse_ice("strong") == IceStrength::strong && !parse_ice("max"));
}

PAKON_TEST(adjustments_parse_by_picture_and_refuse_wild_values) {
    auto j = parse_json(R"({"schema":"pakon-adjust/1","pictures":{"4":{"exposure":0.5,"blue":-0.2},"12":{"contrast":1.2}}})");
    EXPECT(j.has_value());
    if (!j) {
        return;
    }
    auto a = parse_adjustments(*j);
    EXPECT(a.has_value());
    if (a) {
        EXPECT_EQ(a->size(), std::size_t{2});
        EXPECT(a->at(4).exposure == 0.5 && a->at(4).balance[2] == -0.2 && a->at(4).contrast == 1.0);
        EXPECT(a->at(12).contrast == 1.2 && a->at(12).exposure == 0.0);
    }
    for (const char* bad : {R"({"pictures":{"0":{}}})", R"({"pictures":{"x":{}}})",
                            R"({"pictures":{"1":{"exposure":9}}})", R"({"pictures":{"1":{"contrast":"hi"}}})",
                            R"({"nothing":1})"}) {
        auto b = parse_json(bad);
        EXPECT(b.has_value() && !parse_adjustments(*b).has_value());
    }
}

PAKON_TEST(rotation_is_read_from_the_adjustments_and_turns_clockwise) {
    auto j = parse_json(R"({"pictures":{"2":{"rotate":90},"3":{"rotate":270,"exposure":0.5}}})");
    EXPECT(j.has_value());
    if (j) {
        auto a = parse_adjustments(*j);
        EXPECT(a.has_value() && a->at(2).rotate == 90 && a->at(3).rotate == 270 && a->at(3).identity() == false);
    }
    for (const char* bad : {R"({"pictures":{"1":{"rotate":45}}})", R"({"pictures":{"1":{"rotate":"90"}}})"}) {
        auto b = parse_json(bad);
        EXPECT(b.has_value() && !parse_adjustments(*b).has_value());
    }
    // 3 x 2 picture, one sample per pixel shown (R; G and B = R + 100):
    //   1 2 3
    //   4 5 6
    std::vector<std::uint16_t> px;
    for (std::uint16_t v = 1; v <= 6; ++v) {
        px.insert(px.end(), {v, static_cast<std::uint16_t>(v + 100), static_cast<std::uint16_t>(v + 100)});
    }
    const auto red = [](const std::vector<std::uint16_t>& p) {
        std::vector<std::uint16_t> r;
        for (std::size_t i = 0; i < p.size(); i += 3) {
            r.push_back(p[i]);
        }
        return r;
    };
    std::uint32_t w = 3, h = 2;
    const auto cw = rotate_rgb(px, w, h, 90);
    EXPECT(w == 2 && h == 3);
    EXPECT(red(cw) == (std::vector<std::uint16_t>{4, 1, 5, 2, 6, 3}));
    EXPECT(cw[1] == 104);
    w = 3, h = 2;
    EXPECT(red(rotate_rgb(px, w, h, 180)) == (std::vector<std::uint16_t>{6, 5, 4, 3, 2, 1}) && w == 3 && h == 2);
    w = 3, h = 2;
    EXPECT(red(rotate_rgb(px, w, h, 270)) == (std::vector<std::uint16_t>{3, 6, 2, 5, 1, 4}) && w == 2 && h == 3);
    w = 3, h = 2;
    EXPECT(red(rotate_rgb(px, w, h, 0)) == red(px));
}

PAKON_TEST(a_shift_moves_every_frame_window_and_stays_on_the_film) {
    FrameSplit s;
    s.frames = {Frame{100, 1100}, Frame{1200, 2200}, Frame{2300, 3300}};
    shift_frames(s, -150, 3400);
    EXPECT(s.frames[0].first == 0 && s.frames[0].last == 950);
    EXPECT(s.frames[1].first == 1050 && s.frames[1].last == 2050);
    shift_frames(s, 300, 3400);
    EXPECT(s.frames[2].first == 2450 && s.frames[2].last == 3400);
    EXPECT(s.frames.size() == 3u);
}

PAKON_TEST(frames_stop_at_the_film_end_and_skip_short_pieces) {
    const auto p = strip(300, 6, 1600, 80);
    // Film ends half way into frame 6: that piece is too short to keep.
    const auto split = find_frames(p, info_for_pitch(1600, 300, 300 + 5 * 1600 + 800));
    EXPECT_EQ(split.frames.size(), 5u);
}

PAKON_TEST(look_density_follows_tlbs_lut_and_matrix) {
    // LUT[i] = trunc(-3500 log10(i / 16383)), LUT[0] = 16383 [TLB@1000dfc0].
    EXPECT_EQ(look_density_lut(0), 16383);
    EXPECT_EQ(look_density_lut(1), 14750); // = ClientColNegLut.txt row 1
    EXPECT_EQ(look_density_lut(16383), 0);
    EXPECT_EQ(look_density_lut(16000), 35); // the open gate: 35.96, truncated
    // Matrix: identity on the linear terms plus constants 100/200/300.
    NegMatrix m{};
    m[0] = 1;
    m[11] = 1;
    m[22] = 1;
    m[9] = 100;
    m[19] = 200;
    m[29] = 300;
    const auto open = look_pixel(m, 1.0, 1.0, 1.0);
    EXPECT_EQ(open[0], 135u);
    EXPECT_EQ(open[1], 235u);
    EXPECT_EQ(open[2], 335u);
    const auto dense = look_pixel(m, 0.01, 0.1, 2.0); // D 2, 1, beyond the gate
    EXPECT_EQ(dense[0], 100u + static_cast<unsigned>(look_density_lut(160)));
    EXPECT_EQ(dense[1], 200u + static_cast<unsigned>(look_density_lut(1600)));
    EXPECT_EQ(dense[2], 300u); // clamped to code 16383, density 0
    // A cross term: RB on output R.
    NegMatrix x{};
    x[7] = 0.001; // R*B
    const auto rb = look_pixel(x, 0.1, 1.0, 0.1);
    const double d = look_density_lut(1600);
    EXPECT_EQ(rb[0], static_cast<std::uint16_t>(0.001 * d * d + 0.5));
}

PAKON_TEST(dx_records_decode_to_the_film_code_and_frame_numbers) {
    // Records as read on this unit (2026-10-13 USBPcap, the 30-byte 0x90
    // payloads): the OEM's Base 16 strip (DxCode.txt: product 39,
    // specifier 1) and our Base 4 strip.
    const auto json = parse_json(R"({"dx":{"records":[
        {"film_row":100,"hex":"3e4704230104c20905013ddccc07023dfccc08023e20cc0000000000000000"},
        {"film_row":200,"hex":"454903330108c20905014450cc07024528cc000000000000000000000000"},
        {"film_row":300,"hex":"38930435013168cc030101c209050137d8cc07023871cc00000000000000"},
        {"film_row":400,"hex":"4afe0333010d820905014a68cc07024af4cc000000000000000000000000"},
        {"film_row":500,"hex":"08310127010827cc0000000000000000000000000000000000000000000"}
    ]}})");
    EXPECT(json.has_value());
    if (!json) {
        return;
    }
    const auto d = decode_dx(*json);
    EXPECT_EQ(d.records, 5u);
    EXPECT_EQ(d.codes, 4u);
    EXPECT_EQ(d.rejected, 1u); // 33 01 0d 82 09: odd parity
    EXPECT(d.film.has_value());
    if (d.film) {
        EXPECT_EQ(d.film->product, 39);
        EXPECT_EQ(d.film->specifier, 1);
    }
    EXPECT_EQ(d.votes, 3);
    EXPECT_EQ(d.frames.size(), 3u);
    if (d.frames.size() == 3) {
        EXPECT_EQ(d.frames[0].code, 1); // 23 01 04 ..: code 1
        EXPECT(d.frames[0].flag);
        EXPECT_EQ(d.frames[0].read_row, 100u);
        EXPECT_EQ(d.frames[1].code, 2);
        EXPECT_EQ(d.frames[2].code, 0);
        // Code 1 has no mark yet; code 2 takes record 1's mark 0x3ddc,
        // 0x3e47 - 0x3ddc = 107 rows before that record's read row 100:
        // before the window, so no row either.
        EXPECT(!d.frames[0].mark_row.has_value());
    }
    // Our Base 4 strip: product 95, specifier 7, frame 25A.
    DxDecode ours;
    std::vector<std::pair<FilmDx, int>> slots;
    const std::uint8_t rec[30] = {0x60, 0x00, 0x01, 0x03, 0x01, 0x64, 0xce, 0x17};
    decode_dx_record(rec, 7, ours, slots);
    EXPECT_EQ(ours.frames.size(), 1u);
    EXPECT(!slots.empty());
    if (!slots.empty() && !ours.frames.empty()) {
        EXPECT_EQ(slots[0].first.product, 95);
        EXPECT_EQ(slots[0].first.specifier, 7);
        EXPECT_EQ(ours.frames[0].code, 25);
    }
    // A sidecar without DX records decodes to nothing.
    const auto none = decode_dx(*parse_json("{}"));
    EXPECT_EQ(none.records, 0u);
    EXPECT(!none.film.has_value());
}

PAKON_TEST(dx_codes_name_the_pictures_like_the_oem) {
    // strip8dx (Base 8, 2026-10-22): codes 39 / 46 / 47 at mark rows
    // 7659 / 15956 / 17183; the OEM named this strip's pictures 19A..24A.
    DxDecode d;
    d.frames = {{10186, 7659, 39, true}, {17288, 15956, 46, true}, {19654, 17183, 47, true}};
    const std::vector<std::pair<std::size_t, std::size_t>> pictures{
        {6810, 9060}, {9078, 11328}, {11538, 13788}, {13938, 16188}, {16242, 18492}, {18642, 20892}};
    const auto n = dx_frame_numbers(d, pictures, 2336.7);
    EXPECT_EQ(n.size(), 6u);
    for (std::size_t i = 0; i < n.size(); ++i) {
        EXPECT(n[i].has_value());
        if (n[i]) {
            EXPECT_EQ(*n[i], static_cast<int>(19 + i));
        }
    }
    EXPECT(!dx_frame_numbers(DxDecode{}, pictures, 2336.7)[0].has_value());
}

PAKON_TEST(base8_reference_shift_follows_the_resample) {
    // strip8: film window 70..2070, calibration 6..2070, both resampled 3/4:
    // film pixel p = calibration pixel p + 48 (not 64).
    std::string ones;
    for (int i = 0; i < 1548; ++i) {
        ones += i ? ",1" : "1";
    }
    const auto json = parse_json(
        R"({"schema":"pakon-scan/1","kind":"film","mode":"base8","motor_rate":11300,)"
        R"("image":{"film_start_line":0,"line_period_ms":1.355},)"
        R"("geometry":{"calibration":{"start":6,"resample":true,"pixels":1548},)"
        R"("film":{"start":70,"resample":true,"pixels":1500}},)"
        R"("corrections":{"dark":{"planes":[[)" + ones + "],[" + ones + "],[" + ones +
        R"(]]},"white":{"planes":[[)" + ones + "],[" + ones + "],[" + ones + "]]}}}");
    EXPECT(json.has_value());
    if (!json) {
        return;
    }
    const auto info = scan_info_from_sidecar(*json);
    EXPECT(info.has_value());
    if (info) {
        EXPECT_EQ(info->reference_shift, 48u);
        EXPECT(info->mode == "base8");
    }
    EXPECT_EQ(oem_frame_bin("base8"), 6u);
    EXPECT_EQ(oem_frame_bin("base16-ir"), 8u);
    EXPECT_EQ(oem_frame_bin("base4"), 4u);
}

PAKON_TEST(preview8_box_averages_and_writes_a_bmp) {
    // 4 x 2 frame, 16-bit sRGB: left half black, right half white.
    std::vector<std::uint16_t> rgb(4 * 2 * 3, 0);
    for (std::size_t y = 0; y < 2; ++y) {
        for (std::size_t x = 2; x < 4; ++x) {
            for (std::size_t c = 0; c < 3; ++c) {
                rgb[(y * 4 + x) * 3 + c] = 65535;
            }
        }
    }
    const auto p = preview8(rgb, 4, 2, 2); // long side 4 -> 2: factor 2
    EXPECT_EQ(p.width, 2u);
    EXPECT_EQ(p.height, 1u);
    EXPECT_EQ(p.rgb.size(), 6u);
    if (p.rgb.size() == 6) {
        EXPECT_EQ(p.rgb[0], 0u);
        EXPECT_EQ(p.rgb[3], 255u);
    }
    EXPECT(preview8(rgb, 4, 2, 0).rgb.empty());

    const auto path = temp_path("pakon_preview.bmp");
    // 3 x 2 image: rows pad to 12 bytes; top-left pixel red.
    std::vector<std::uint8_t> img(3 * 2 * 3, 0);
    img[0] = 255;
    EXPECT(image::write_bmp24(path, 3, 2, img).has_value());
    std::ifstream f(path, std::ios::binary);
    std::vector<unsigned char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    EXPECT_EQ(b.size(), 54u + 2 * 12);
    if (b.size() == 54 + 24) {
        EXPECT(b[0] == 0x42 && b[1] == 0x4d); // "BM"
        EXPECT_EQ(b[18], 3u);                   // width
        EXPECT_EQ(b[22], 2u);                   // height (bottom-up)
        EXPECT_EQ(b[28], 24u);                  // bits per pixel
        // Bottom-up: the top row is the second stored row; red is stored B G R.
        EXPECT_EQ(b[54 + 12 + 2], 255u);
        EXPECT_EQ(b[54 + 12 + 0], 0u);
    }
    EXPECT(!image::write_bmp24(path, 3, 2, std::vector<std::uint8_t>(5)).has_value());
}

PAKON_TEST(look_geometry_per_base) {
    const auto b4 = look_geometry("base4");
    const auto b8 = look_geometry("base8-ir");
    const auto b16 = look_geometry("base16");
    EXPECT(b4.has_value() && b8.has_value() && b16.has_value());
    if (b4 && b8 && b16) {
        // Every base thumbnails to 250 x 375.
        for (const auto& g : {*b4, *b8, *b16}) {
            EXPECT_EQ(g.width / g.div, kThumbWidth);
            EXPECT_EQ(g.height / g.div, kThumbHeight);
        }
        EXPECT_EQ(b8->out_width, 2250u); // rotated, not upscaled
        EXPECT_EQ(b4->out_width, 3000u); // upscaled 2x
        EXPECT_EQ(b16->row_shift, 8);
    }
    EXPECT(!look_geometry("base32").has_value());
}

PAKON_TEST(look_thumbnail_is_the_4x4_mean_rotated_180) {
    const auto g = *look_geometry("base4");
    LookPlanes p;
    p.width = g.width;
    p.height = g.height;
    for (auto& pl : p.plane) {
        pl.assign(g.width * g.height, 0);
    }
    // Top-left 4x4 block of R = 100..115 (mean 107.5 -> 108).
    for (std::size_t y = 0; y < 4; ++y) {
        for (std::size_t x = 0; x < 4; ++x) {
            p.plane[0][y * g.width + x] = static_cast<std::uint16_t>(100 + 4 * y + x);
        }
    }
    const auto t = look_thumbnail(p, g.div);
    EXPECT_EQ(t.size(), 3 * kThumbWidth * kThumbHeight);
    EXPECT_EQ(t[kThumbWidth * kThumbHeight - 1], 108u); // bottom-right after 180
    EXPECT_EQ(t[0], 0u);
    const auto dmin = look_dmin({t});
    EXPECT_EQ(dmin[0], 0u);
}

PAKON_TEST(a_bw_look_script_sets_tlbs_three_bw_fields) {
    const auto colour = look_script(2, "w", "c", *look_geometry("base8"), {297, 575, 732}, {-1, -1});
    const auto bw = look_script(2, "w", "c", *look_geometry("base8"), {297, 575, 732}, {-1, -1}, {}, true);
    EXPECT(colour.find("0x48 4") == std::string::npos && colour.find("0x4c 1") == std::string::npos &&
           colour.find("poke roll 0x20") == std::string::npos);
    EXPECT(bw.find("poke roll 0x20 4\n") != std::string::npos);
    EXPECT(bw.find("poke bal1 0x48 4\n") != std::string::npos && bw.find("poke bal2 0x48 4\n") != std::string::npos);
    EXPECT(bw.find("poke ca1 0x4c 1\n") != std::string::npos && bw.find("poke ca2 0x4c 1\n") != std::string::npos);
    // The balance field comes before its call, the adjust field before its call.
    EXPECT(bw.find("poke bal1 0x48 4") < bw.find("call PIAnselColorSceneBalancePlanar @roll @scene1"));
    EXPECT(bw.find("poke ca1 0x4c 1") < bw.find("call PIColorAdjustPlanar @ca1"));
    const auto raw = look_script(2, "w", "c", *look_geometry("base8"), {297, 575, 732}, {-1, -1}, {}, false, true);
    EXPECT(raw.find("save out1 10125000 \"w") != std::string::npos && raw.find("look02.rgb") != std::string::npos);
    EXPECT(raw.find("save out1") < raw.find("buf out1 zero 4") && colour.find("save out") == std::string::npos);
    std::vector<std::uint16_t> px{1000, 2000, 3000, 65535, 0, 0};
    to_grey(px);
    EXPECT(px[0] == px[1] && px[1] == px[2] && px[0] == 1860u); // 212.6 + 1430.4 + 216.6
    EXPECT(px[3] == px[4] && px[3] == 13933u);
}

PAKON_TEST(a_look_script_share_develops_its_pictures_after_the_whole_roll) {
    const std::vector<std::size_t> share{2, 4};
    const auto s = look_script(4, "w", "c", *look_geometry("base8"), {600, 800, 1100}, {39, 1}, share);
    const auto count = [&](const std::string& what) {
        std::size_t n = 0;
        for (auto at = s.find(what); at != std::string::npos; at = s.find(what, at + 1)) {
            ++n;
        }
        return n;
    };
    EXPECT_EQ(count("call PIAnselAddScene"), 4u);              // the whole roll's analysis
    EXPECT_EQ(count("call PIAnselColorSceneBalancePlanar"), 2u); // its own pictures only
    EXPECT(s.find("look02.jpg") != std::string::npos && s.find("look04.jpg") != std::string::npos);
    EXPECT(s.find("look01.jpg") == std::string::npos && s.find("look03.jpg") == std::string::npos);
    EXPECT(look_script(4, "w", "c", *look_geometry("base8"), {600, 800, 1100}, {39, 1}) ==
           look_script(4, "w", "c", *look_geometry("base8"), {600, 800, 1100}, {39, 1}, {}));
}

PAKON_TEST(look_script_follows_tlbs_call_sequence) {
    const auto s = look_script(2, "C:/w", "C:/cap dir", *look_geometry("base8"), {607, 876, 1111},
                               {39, 1});
    const auto at = [&](const char* needle) { return s.find(needle); };
    EXPECT(at("buf roll blob \"C:/cap dir/roll.bin\" 4096") != std::string::npos);
    EXPECT(at("poke desc1 0x54 25f") != std::string::npos); // dmin R 607
    EXPECT(at("poke desc2 0x5c 457") != std::string::npos); // dmin B 1111
    // Every scene is added before the roll is analysed, then balanced.
    EXPECT(at("call PIAnselAddScene @roll @scene2 @desc2") < at("call PIAnselAnalyzeRoll @roll"));
    EXPECT(at("call PIAnselAnalyzeRoll @roll") < at("call PIAnselColorSceneBalancePlanar"));
    EXPECT(at("buf path2 wstr \"C:/w/look02.jpg\"") != std::string::npos);
    EXPECT(at("call PISaveFilePlanar_8 @out2 8ca 5dc 3 @path2 0 5a 12c 0") != std::string::npos);
    // Base 8 fields poked into the Base 4 templates.
    EXPECT(at("poke roll 0x18 6") != std::string::npos);
    EXPECT(at("poke desc1 0x60 27") != std::string::npos); // DX product 39
    EXPECT(at("poke bal1 8 5dc") != std::string::npos);
    EXPECT(at("call PIScaleAndRotatePlanar @big1 1 5dc 8ca 8ca 5dc 3 10e 2") != std::string::npos);
    EXPECT(at("poke ca1 8 8ca") != std::string::npos);
    const auto unknown = look_script(1, "w", "c", *look_geometry("base4"), {0, 0, 0}, {});
    EXPECT(unknown.find("poke desc1 0x64 ffffffff") != std::string::npos);
}

int main() { return pakon::test::run_all(); }
