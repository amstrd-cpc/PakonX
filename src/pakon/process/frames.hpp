#pragma once

// Frame separation for a film window (processing, never on the scan
// path): which line ranges of a .pakraw hold the individual pictures.
//
// 1. Line profile: green transmission t = (raw - dark) / (white - dark)
//    per pixel, with the Corrections dark and open-gate references from
//    the sidecar (calibration window shifted onto the film window); per
//    line its mean and 10th / 90th percentile over the central 80 %.
// 2. Gaps: on a negative the space between frames is bare film base:
//    the line is uniform at the base transmission (p10 >= 0.85 base,
//    p90 <= 1.15 base), where a picture always has denser areas. Base =
//    median mean of the uniform lines. Runs closer than 3 % of a pitch
//    merge; runs shorter than 8 lines are dropped.
// 3. Pitch: 35 mm frames repeat every 38 mm. The seed comes from the
//    transport: film2 (Base 4, motor rate 25676, 0.904 ms lines) showed
//    gaps every 1633 lines = 1.476 s, which scales with 1/rate. The
//    pitch and phase are then fitted to the gaps found (most gaps within
//    4 % of the grid, least squares), so a thin or blank frame without a
//    visible gap does not shift the others.
//    The gaps, pitch and base feed the positive (base estimate, CCD
//    row spacing); they do not place the frames.
// 4. Frames: the OEM placement (TLB framing, see place_frames below), so
//    each frame is the same piece of film the OEM software saves.
//
// Steps 1-3 measured on this unit's scans of 2026-10-10/13 [INFERRED
// design]; step 4 [DIS TLB@100072c0].

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pakon/errors/error.hpp"
#include "pakon/process/json.hpp"
#include "pakon/process/pakraw.hpp"

namespace pakon::process {

inline constexpr double kFramePitchSecondsAtRate25676 = 1.476; // film2: 1633 x 0.904 ms

// Frame format: full 35 mm (24 x 36), half (18 x 24, two per full frame
// pitch), pano (panoramic: longer than a full frame, e.g. 24 x 65; cut gap
// to gap), or auto (from the gaps: half-way gaps -> half, gaps clearly
// further apart than a full frame -> pano, else full).
enum class FrameFormat { full, half, auto_detect, pano };

// Infrared dust removal strength (process/ice.hpp).
enum class IceStrength { off, light, normal, strong };

// What frame separation needs from <prefix>.scan.json.
struct ScanInfo {
    std::string mode;
    std::size_t film_start{0};             // first film line (detector), else 0
    std::optional<std::size_t> film_end;   // end line, when the film passed
    double motor_rate{0};
    double line_period_ms{0};
    std::size_t pixels{0};                 // film-window pixels per line
    bool ir{false};
    std::size_t reference_shift{0};        // film pixel p = calibration pixel p - shift (after resample)
    std::vector<double> dark_green;        // calibration-window references
    std::vector<double> white_green;
    // All three channels (R, G, B), calibration window, and the LED
    // on-times the white was measured at vs the film was scanned at.
    std::array<std::vector<double>, 3> dark;
    std::array<std::vector<double>, 3> white;
    std::array<double, 3> calibration_on_time{};
    std::array<double, 3> film_on_time{};
    // The unit's EEPROM colour-negative matrix (unit.neg_matrix), when the
    // sidecar has it (scans from 2026-10-20 on).
    std::optional<std::array<double, 30>> neg_matrix;
    // Colour slide (sidecar "film": "e6", scan/film.hpp): a positive,
    // developed without inversion (positive.hpp).
    bool reversal{false};
    // IR dark / open-gate references (calibration plane 3), IR modes only.
    std::vector<double> dark_ir;
    std::vector<double> white_ir;
    // Not from the sidecar: the caller's dust-removal choice (frames --ice),
    // applied wherever film rows are developed (positive and OEM look).
    IceStrength ice{IceStrength::off};
    // Not from the sidecar: the caller's frame format (frames --format).
    FrameFormat frame_format{FrameFormat::full};
};
Result<ScanInfo> scan_info_from_sidecar(const Json& sidecar);

struct LineStats {
    float mean{0}; // green, central 80 %
    float p10{0};
    float p90{0};
    // Mean over the full width of R, G, B as TLB's 8-bit line value:
    // floor(14-bit code / 64), code = 16000 t clamped to 16383 (open gate
    // 250). strip8 vs the OEM DxCode runs: all edges within 2 bins
    // (2026-10-22); a 0..255 linear transmission split runs differently.
    float rgb{0};
};
Result<std::vector<LineStats>> line_profile(PakrawReader& raw, const ScanInfo& info);

struct GapRun {
    std::size_t first{0}; // [first, last) lines of bare base
    std::size_t last{0};
};
// What a frame-sized piece of film holds. Blank: >= 80 % of its lines are
// bare base (unexposed tail; roll1 frame 38: 100 %, pictures <= 6 %).
// Fogged: median line p90 < 0.08 x base, nearly opaque (the fogged
// leader; roll1 frame 1: 0.03, pictures >= 0.19). roll1/film2, 2026-10-14.
enum class FrameKind { picture, blank, fogged };
std::string_view to_string(FrameKind k);

// How the OEM framing placed a frame (the DxCode.txt sections):
// nice = an exposed run of 95-115 % of the frame length, between = in a
// gap between two nice ones, end / beginning = walking from the last /
// first nice one to the film end / start, partial = the shortened piece
// at the film start or end.
enum class Placement { nice, between, end, beginning, partial };
std::string_view to_string(Placement p);

struct Frame {
    std::size_t first{0}; // [first, last) lines
    std::size_t last{0};
    Placement placement{Placement::nice};
    FrameKind kind{FrameKind::picture};
};

// TLB framing [DIS TLB@100072c0], on the line profile binned by `bin`
// lines (Base 4: 4, the thumbnail scale of the 1000 x 1500 frame):
//  - metric per bin = 255 - mean 8-bit RGB transmission [@10006870];
//  - ones = metric > threshold; the start threshold comes from the
//    histogram of the film bins (first peak = the film base, then where
//    it falls below 1/10) [@10005d20], then raised in steps of 2 while
//    over-long runs remain and the nice count does not drop, and lowered
//    the same way while short runs remain [@100072c0];
//  - nice runs (95-115 % of 375 bins) place a frame at run start +
//    (width - 375) / 3; a run of two frames places two [@10006930];
//  - gaps between nice frames get (distance / pitch, rounded down when
//    the rest is under a quarter pitch) evenly spaced frames [@100063d0];
//  - from the last / first nice frame towards the film end / start, the
//    next frame goes where a 375-bin window covers most ones within the
//    25 bins of play, ties to the middle [@10006ae0, @10006ca0,
//    @100064e0]; a remaining piece of at least half a pitch becomes a
//    partial frame.
// The 375 / 400 bins (frame length 1500, pitch 1600 lines at Base 4) are
// the DxCode.txt Target and its beginning steps [OEM log 2026-10-09].
// Not reproduced: the DX frame-number cross-check [@10006310] and the
// "no film here" test [@10006630]; blank and fogged frames are labelled
// by FrameKind instead.
inline constexpr std::size_t kOemFrameBins = 375;
inline constexpr std::size_t kOemPitchBins = 400;
struct Placed {
    std::size_t bin{4};
    int threshold{0};
    std::vector<Frame> frames;
};
// Lines per framing bin: 4 / 6 / 8 at Base 4 / 8 / 16.
std::size_t oem_frame_bin(std::string_view mode);
// Half frames: the same OEM placement with half the pitch (200 bins) and
// a picture of 180 bins (18 of the 19 mm a half frame advances; the full
// frame is 375 of 400) [INFERRED, scaled from the OEM's full frame].
inline constexpr std::size_t kHalfFrameBins = 180;
inline constexpr std::size_t kHalfPitchBins = 200;
Placed place_frames(std::span<const LineStats> profile, std::size_t film_start,
                    std::size_t film_end, std::size_t bin, std::size_t frame_bins = kOemFrameBins,
                    std::size_t pitch_bins = kOemPitchBins);

struct FrameSplit {
    double pitch_seed{0};
    double pitch{0};
    double base{0};
    std::size_t gaps_on_grid{0};
    std::size_t gaps_half_way{0}; // gaps at half a pitch off the grid
    std::vector<GapRun> gaps;
    std::size_t bin{4};
    int threshold{0};
    bool half{false}; // placed as half frames
    bool pano{false}; // placed gap to gap as panoramic frames
    double gap_pitch{0}; // median distance between neighbouring gaps (lines)
    std::vector<Frame> frames;
};
FrameSplit find_frames(std::span<const LineStats> profile, const ScanInfo& info);

// The user's framing correction (PakonX): every frame window moved by
// `lines` film lines (negative = towards the film start), clamped to
// [0, rows). The whole roll moves together, so picture numbers, DX
// numbers and the gaps stay as found.
void shift_frames(FrameSplit& split, long long lines, std::size_t rows);

} // namespace pakon::process
