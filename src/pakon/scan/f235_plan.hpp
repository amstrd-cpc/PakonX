#pragma once

// F-235 scan plan: the frames of one film pass, in the order TLA.dll's
// bBeforeScan / FpgaSettings / exposures / drive use (docs/F235.md), and the
// teardown. UNVERIFIED (no F-235 captured): for dry runs and the simulator.
// The calibration inputs (lamp level, exposures, gains, offsets) come from
// the caller; on a real unit they come from its EEPROM / calibration, which is
// not decoded yet.

#include <cstdint>
#include <string>
#include <vector>

#include "pakon/errors/error.hpp"
#include "pakon/ppb/packet.hpp"

namespace pakon::scan {

struct F235Settings {
    std::uint32_t film_colour{1};     // TLA film colour: 1, 2, 4, 8, 0x1000, 0x2000
    std::uint16_t lamp_level{0};      // 0x2CEC..0x3A34
    std::uint16_t lamp_ir{0};         // 0 = no IR; else <= 0x5DC (IR mode on)
    std::uint8_t rollers{0};          // 0..3
    std::uint16_t pixel_start{0};
    std::uint16_t pixel_count{0};     // start + count <= 0x848 (0x845 binned), count >= 6
    bool binning{false};
    std::uint16_t integration{0};     // <= 0xFF0
    std::uint16_t ir_window{0};       // IR only: <= integration - 0x6E
    int offsets[3]{};                 // R, G, B: -255..255
    std::uint16_t gains[3]{};         // <= 0x3F
    std::uint16_t exposures[4]{};     // R, G, B, IR: <= 0xFFF
    std::uint16_t drive_rate{0};      // 200..3700 (hardware version >= 1000)
    bool reverse{false};
};

struct F235Step {
    std::string what;
    ppb::Frame frame;
};

Result<std::vector<F235Step>> f235_scan_plan(const F235Settings& s);
std::vector<F235Step> f235_teardown(bool ir);

} // namespace pakon::scan
