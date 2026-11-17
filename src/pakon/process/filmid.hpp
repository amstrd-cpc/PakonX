#pragma once

// Which film is this roll? (processing, after the scan) [INFERRED design]
//
// 1. DX: the decoded product / specifier (process/dx.hpp) gives the ISO
//    speed from the PIMA DX Part 1/2 table that Kodak's Ansel ships as
//    common-ProdCodeTable.dpi (128 products x 16 specifiers, 0 = unknown).
// 2. Film kind, measured on the scan itself (no DX code names a process):
//    - base: mean flat-fielded transmission R, G, B of the bare-base gap
//      lines between frames. An orange mask holds back blue far more than
//      red; a slide's frame gaps are opaque; silver and unmasked films have
//      a clear, neutral base;
//    - colour: how far the channels' densities above base part on the
//      pictures (a B&W negative's dye or silver is neutral);
//    - IR: in ICE scans, how closely the infrared follows the picture
//      (silver blocks infrared, dyes do not; process/ice.hpp).
// Thresholds are measured on this unit's rolls (see filmid.cpp).

#include <array>
#include <optional>
#include <string>

#include "pakon/process/frames.hpp"
#include "pakon/process/pakraw.hpp"

namespace pakon::process {

// ISO speed for a DX product (0..127) / specifier (0..15); nullopt when the
// table has no speed for it.
std::optional<int> dx_iso(int product, int specifier);

struct FilmGuess {
    // "c41", "c41-unmasked", "e6", "bw-silver", "bw-c41" (scan/film.hpp
    // names), or "" when the scan does not tell.
    std::string kind;
    std::array<double, 3> base{}; // bare-base transmission R, G, B
    bool have_base{false};
    // The base reads at the A/D ceiling: a clear base scanned with the
    // colour-negative exposure boost (rescan with the right film kind).
    bool clipped{false};
    double colour{0};             // mean channel spread of density above base
    std::optional<double> ir_follow; // IR vs green correlation (ICE scans)
    std::string why;              // the measurements, in words
};

FilmGuess detect_film(PakrawReader& raw, const ScanInfo& info, const FrameSplit& split);

} // namespace pakon::process
