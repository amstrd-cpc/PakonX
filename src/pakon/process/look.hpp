#pragma once

// Stage 2, the OEM "Pakon look" (processing, never on the scan path): our
// frames in TLB's form, ready for the OEM's own PakonIMAu.dll, which a
// 32-bit helper (tools/oem_host) drives with the call sequence TLB uses
// (OEM_RE.md §13). This module only prepares the data and the helper's
// script; it never loads OEM code.
//
// Per frame (LookGeometry: Base 4 1000 x 1500, Base 8 1500 x 2250, Base 16
// 2000 x 3000 CCD pixels x lines, the OEM frame):
//   t  = (raw - dark) / (white - dark), the sidecar references; R taken
//        s lines earlier and B s lines later than G (tri-linear CCD), CCD
//        pixel x - 1 (the OEM window starts one pixel earlier) [river1
//        2026-10-14, strip8 2026-10-22: s = 4 / 6 at Base 4 / 8];
//   i  = trunc(16000 t), clamped to 14 bits: the open gate at 16000
//        [DIS TLB@1001f550, gain 125 * 2^32 / span];
//   D  = LUT[i] = trunc(-3500 log10(i / 16383)), LUT[0] = 16383
//        [DIS TLB@1000dfc0];
//   out_c = NegMatrix row c . (R, G, B, R2, G2, B2, RG, RB, GB, 1) on D,
//        rounded to u16 — the unit's EEPROM matrix [DIS TLB@1000cf80,
//        EEPROM 0x026]. River1 frame 1 vs the OEM's own input, no free
//        parameter: bias <= 2, rms 40 per pixel = the grain of two scans;
//        strip8 frame 2 (Base 8): bias <= 2.3.
// The Ansel scene thumbnail is 250 x 375 in every base: the d x d box mean
// (d = 4 / 6 / 8) rotated 180 degrees. dmin (the film base) is the 1st
// percentile per channel over the thumbnails, a stand-in for TLB's own
// measurement. Ansel also takes the film's DX product / specifier
// (AddScene descriptor +0x60 / +0x64; river 96/14, strip8 39/1) and
// balances by film type; -1/-1 = not read.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pakon/errors/error.hpp"
#include "pakon/process/dx.hpp"
#include "pakon/process/frames.hpp"
#include "pakon/process/pakraw.hpp"

namespace pakon::process {

inline constexpr std::size_t kThumbWidth = 250;
inline constexpr std::size_t kThumbHeight = 375;
inline constexpr int kLookPixelOffset = -1;
inline constexpr int kLookOpenGate = 16000;

// The OEM frame per base and what TLB asks PakonIMAu to make of it (VM
// proxy runs: Base 4 2026-10-14, Base 8 / 16 2026-10-22).
struct LookGeometry {
    std::size_t width{0};    // CCD pixels
    std::size_t height{0};   // lines
    std::size_t div{0};      // thumbnail box = framing bin, lines
    int row_shift{0};        // R / B lines vs G
    std::size_t out_width{0};  // PIScaleAndRotatePlanar output (rotated 270)
    std::size_t out_height{0};
};
// Base 16's row shift 8 comes from film16ir1 (stage-1 gradient matching),
// not yet from an OEM dump.
Result<LookGeometry> look_geometry(std::string_view mode);

// 3 planes R, G, B of width x height, row-major.
struct LookPlanes {
    std::size_t width{0};
    std::size_t height{0};
    std::array<std::vector<std::uint16_t>, 3> plane;
};

using NegMatrix = std::array<double, 30>;

// TLB's 14-bit transmission code -> density (x 3500 per decade).
int look_density_lut(int code);

// One pixel's R, G, B transmission -> TLB's input to PakonIMAu.
std::array<std::uint16_t, 3> look_pixel(const NegMatrix& m, double t_r, double t_g, double t_b);

// Lines [first, first + g.height) of the film window.
Result<LookPlanes> look_frame(PakrawReader& raw, const ScanInfo& info, const NegMatrix& m,
                              const LookGeometry& g, std::size_t first);

// The Ansel thumbnail: 3 planes of kThumbWidth x kThumbHeight.
std::vector<std::uint16_t> look_thumbnail(const LookPlanes& p, std::size_t div);

// 1st percentile per channel over the thumbnails (every 7th sample).
std::array<std::uint16_t, 3> look_dmin(const std::vector<std::vector<std::uint16_t>>& thumbs);

// The oem_host script for `frames` frames whose files sit in `work`
// (densNN.raw, thumbNN.raw). Descriptor templates come from `capture`
// (TLB's calls captured at Base 4); the per-base fields are poked: roll
// +0x18/+0x1c = div, AddScene +0x0c height, +0x54.. dmin, +0x60/+0x64 DX,
// balance and colour-adjust width/height [replays byte-identical to the
// OEM at Base 8, 2026-10-22]. PakonIMAu writes `work`/lookNN.jpg itself
// (PISaveFilePlanar_8: JPEG, quality 90, 300 dpi, as TLB saves).
// `only` (1-based, ascending) limits the per-picture part to those
// pictures; the roll analysis always takes all `frames`, so several hosts
// can each develop a share of one roll with the same balance.
// `bw`: black-and-white film, as TLB runs it with iFilmColor 4 (BnW_NORMAL;
// TLXClientDemo greys it out, TLX_ScanEnable re-enables it): the same
// densities and calls, with roll +0x20 = 4, scene balance +0x48 = 4 and
// colour adjust +0x4c = 1 [VM proxy bwref, 2026-10-23: a replay with these
// three is byte-identical to the OEM's B&W JPEG].
std::string look_script(std::size_t frames, const std::string& work, const std::string& capture,
                        const LookGeometry& g, const std::array<std::uint16_t, 3>& dmin,
                        const FilmDx& dx, std::span<const std::size_t> only = {}, bool bw = false,
                        bool raw = false);
// `raw`: also keep each picture's final 8-bit planar RGB (the buffer
// PISaveFilePlanar_8 compresses) as `work`/lookNN.rgb, for saving the OEM
// look as TIFF or at another JPEG quality without a second JPEG loss.

} // namespace pakon::process
