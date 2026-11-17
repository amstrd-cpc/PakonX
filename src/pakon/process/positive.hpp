#pragma once

// Stage 1 negative -> positive (processing, never on the scan path): a
// neutral, physically grounded conversion. Not the "Pakon look" (that is
// PakonIMAu.dll's ColNeg + Ansel pipeline, OEM_RE §13 — stage 2).
//
// Per pixel and channel c (R, G, B):
//   alignment: the CCD is tri-linear — its R, G, B rows sit a fixed film
//        distance apart, ~0.00249 of a frame pitch: R must be taken s
//        lines earlier and B s lines later than G, s = round(0.00249 x
//        pitch). Measured by gradient matching: Base 4 s = 4 (roll1,
//        two frames), Base 16 IR s = 8 (film16ir1); 2026-10-14;
//   t  = (raw - dark) / (white - dark) x on_cal / on_film
//        flat field with the Corrections references; the white was
//        measured at the calibration on-time, the film lit at the boosted
//        one (LED output taken as linear in on-time [INFERRED]);
//   t' = t / base_c        base = orange-mask transmission, measured on
//                          the bare-base gap lines between the frames;
//   D  = -log10(t')        density above base;
//   x  = (D - Dmed_c) / S + m
//        ONE slope S for all channels (the mean 0.1..99.9 % density span
//        of the window's pictures; dye-layer contrasts are alike, a
//        per-channel stretch skewed the colours), per-channel offsets
//        that align the channels' median densities (grey world over the
//        whole window), m = the mean (median - 0.1 %) position;
//   x += e_frame           per-frame exposure: the frame's 99.5 % x of
//                          its brightest channel goes to 1, darkening up
//                          to max_exposure_shift but brightening only up
//                          to max_exposure_lift: a frame with no white in
//                          it (night, low key, a dark interior) was lifted
//                          up to 2.5 stops and came out too bright;
//   y  = (10^(R (x - 1)) - 10^-R) / (1 - 10^-R)   scene-linear, R decades;
//   out = sRGB-encoded y, 16 bit.
// The IR lane is not used here (Digital ICE is a later stage).
//
// Slide (info.reversal): the film is already positive and has no mask,
// so base = 1 (no gap lines needed) and x runs the other way:
//   x = (Dmed - D) / S + m,  m = mean (99.9 % - median) position,
// with ONE Dmed for all channels: the scan's white is the open gate, so
// a slide's neutral is already neutral (no grey-world). [INFERRED design]

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include "pakon/errors/error.hpp"
#include "pakon/process/frames.hpp"
#include "pakon/process/json.hpp"
#include "pakon/process/pakraw.hpp"

namespace pakon::process {

struct PositiveOptions {
    double range_decades{2.2};
    double low_percentile{0.001};
    double high_percentile{0.999};
    bool per_frame_exposure{true};
    double max_exposure_shift{0.35}; // in x units, darker
    double max_exposure_lift{0.1};   // in x units, brighter (0.1 x 2.2 decades = 0.73 stop)
};

// Tri-linear CCD row spacing as a fraction of the frame pitch (above).
inline constexpr double kRowSpacingPerPitch = 0.00249;

struct PositiveParams {
    std::array<double, 3> base{};    // base transmission per channel
    std::array<double, 3> d_low{};   // 0.1 % density per channel
    std::array<double, 3> d_mid{};   // median density per channel
    std::array<double, 3> d_high{};  // 99.9 % density per channel
    double span{1.0};                // common slope S
    double mid_x{0.5};               // m
    int line_shift{0};               // s: R from line - s, B from line + s
    std::size_t base_lines{0};       // gap lines the base came from
    double range_decades{2.2};
    bool per_frame_exposure{true};
    double max_exposure_shift{0.35};
    double max_exposure_lift{0.1};
    bool reversal{false};            // slide: positive film, no inversion
    // The flat-fielded negative instead of the positive: linear
    // transmission t per channel (orange mask kept, R/B rows aligned,
    // ICE applied), 0..1 -> 0..65535, for other converters (Negative Lab
    // Pro's "Scanner - Linear" source). No exposure, look or inversion.
    bool negative{false};
};

// Roll-wide parameters from the gaps (base) and the pictures (densities).
Result<PositiveParams> measure_positive(PakrawReader& raw, const ScanInfo& info,
                                        const FrameSplit& split, const PositiveOptions& opt = {});

// Which end of the film went in first. It decides the picture's
// orientation in the stream (feeding end-for-end rotates it 180 degrees):
//  - head first (the leader first; the normal load — the OEM's status
//    word flags "Tail first", bit 0x20000000, OEM_RE §8): the CCD pixel
//    axis is the picture's vertical as is, the travel axis runs right to
//    left. roll1 (fogged leader scanned first): faces upright only so.
//  - tail first: the opposite on both axes. film2: its seven-segment date
//    imprint reads "22:18" at the bottom right only so (2026-10-14).
// Only the viewing outputs are oriented; the .pakraw keeps stream order.
enum class Feed { head_first, tail_first };
constexpr std::size_t picture_row(std::size_t p, std::size_t pixels, Feed feed) {
    return feed == Feed::tail_first ? pixels - 1 - p : p;
}
constexpr std::size_t picture_col(std::size_t line, std::size_t lines, Feed feed) {
    return feed == Feed::head_first ? lines - 1 - line : line;
}

// The user's look for one picture (PakonX), applied in x after the
// per-frame exposure:
//   x' = g + (x - g) * contrast + (exposure + balance_c) * log10(2) / R
// g = the x of 18 % grey. Exposure and balance are in stops of
// scene-linear y (one stop doubles y: y = 10^(R (x - 1)) near the top),
// balance per channel R, G, B; contrast scales x about grey. Every step
// is a per-channel function of the output value, so a screen preview
// can show it exactly with a lookup table (apps/pakonx/web/app.js).
//
// `rotate` (clockwise degrees: 0, 90, 180 or 270) turns the finished
// picture when it is saved (rotate_rgb); it is not part of identity(),
// which is about tone only.
struct Adjust {
    double exposure{0.0};
    std::array<double, 3> balance{};
    double contrast{1.0};
    int rotate{0};
    bool identity() const {
        return exposure == 0.0 && balance == std::array<double, 3>{} && contrast == 1.0;
    }
};

// <prefix>.adjust.json (schema "pakon-adjust/1"):
//   {"pictures": {"4": {"exposure": 0.5, "red": 0, "green": 0,
//                       "blue": -0.2, "contrast": 1.1, "rotate": 90}}}
// Keys are picture numbers (1-based); absent fields keep the default.
// Out-of-range values (exposure beyond +-3, balance +-2, contrast outside
// 0.5..2, rotate other than 0/90/180/270) are refused. Other top-level
// keys (PakonX's "shift") are not read here.
Result<std::map<std::size_t, Adjust>> parse_adjustments(const Json& json);

// Black-and-white film (PakonX look): every pixel of a developed frame
// becomes grey, Rec. 709 luma of its sRGB values, in place (R = G = B).
void to_grey(std::span<std::uint16_t> rgb);

// Interleaved RGB (3 samples per pixel), w x h, turned clockwise by
// `degrees` (0, 90, 180, 270); w and h become the turned size.
std::vector<std::uint16_t> rotate_rgb(std::span<const std::uint16_t> px, std::uint32_t& w,
                                      std::uint32_t& h, int degrees);

// One frame as interleaved 16-bit sRGB, width = film travel, height =
// CCD pixels, oriented for viewing (picture_row / picture_col).
Result<std::vector<std::uint16_t>> develop_frame(PakrawReader& raw, const ScanInfo& info,
                                                 const PositiveParams& params, const Frame& frame,
                                                 Feed feed, const Adjust& adjust = {});

// An 8-bit view of a developed frame for screens (the PakonX light table):
// box-averaged so the long side is at most `max_side`, the 16-bit sRGB
// values taken to 8 bits (rounded).
struct Preview8 {
    std::size_t width{0};
    std::size_t height{0};
    std::vector<std::uint8_t> rgb; // interleaved, top row first
};
Preview8 preview8(std::span<const std::uint16_t> rgb16, std::size_t width, std::size_t height,
                  std::size_t max_side);

} // namespace pakon::process
