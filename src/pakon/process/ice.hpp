#pragma once

// Infrared dust and scratch removal (processing, never on the scan path):
// this stack's own design [INFERRED], not the OEM's DMLDICELib.dll
// (OEM_RE §13.5), whose interface is still [UNKNOWN].
//
// Colour dyes are transparent to infrared; dust, hair and scratches are
// not. In the -ir modes the scanner reads an IR plane in the same pass
// (scan/modes.hpp), so a pixel's IR transmission says how much of its
// light a defect took, and nothing about the picture.
//
// On a block of raw film-window rows (interleaved R, G, B, then the IR
// block of `pixels` samples, OEM_RE §10):
//   1. t_ir = (ir - dark_ir) / (white_ir - dark_ir), the calibration's IR
//      references (sidecar plane 3);
//   2. CCD fixed pattern: t_ir / median of its CCD pixel over the block,
//      so clean film sits at 1 whatever the column;
//   3. red leak: the IR row picks up a little of the red dye's density;
//      the clean pixels' IR is fitted as a + b * D_red and divided out;
//   4. defects: IR more than the strength's number of noise widths below
//      clean film (noise from the median absolute deviation), grown over
//      neighbours more than 2 noise widths down, dilated by a ring;
//   5. every defect pixel is filled per channel from the nearest clean
//      pixels on the four sides, weighted by 1 / distance. Dividing by the
//      IR transmission instead left the specks visible on 7908: the IR
//      dip is shallower and wider than the visible one (2026-11-06).
// Registration: the IR row reads the same film line as the red row; green
// and blue see it s and 2s lines later (s = PositiveParams::line_shift),
// measured on 7908 (Base 4: R 0, G +4, B +8) and 13212 (Base 16: R 0..1,
// G +8, B +16), 2026-11-06.
//
// Silver black-and-white film blocks infrared like light: its IR plane is
// a copy of the picture. The block is left as scanned when the IR follows
// the green transmission (correlation above kSilverCorrelation).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "pakon/process/frames.hpp"

namespace pakon::process {

std::string_view ice_name(IceStrength s);
std::optional<IceStrength> parse_ice(std::string_view s);

// Silver B&W 13212 (Base 16): IR vs picture 0.99; colour 7919 (Base 16)
// 0.45, 7908 (Base 4) 0.19, from the red leak (2026-11-06).
inline constexpr double kSilverCorrelation = 0.9;

struct IceReport {
    bool ran{false};
    bool silver{false};        // IR follows the picture: nothing corrected
    std::size_t defects{0};    // pixels filled
    double leak{0};            // fitted red-leak slope b
    double sigma{0};           // IR noise of clean film
};

// `rows`: `lines` rows of info.pixels * 4 samples (R, G, B interleaved,
// then IR), in place. `line_shift`: s above. No-op unless info.ir, the
// sidecar has the IR references and strength != off.
IceReport ice_correct(std::span<std::uint16_t> rows, std::size_t lines, const ScanInfo& info,
                      int line_shift, IceStrength strength);

} // namespace pakon::process
