#include "pakon/scan/f235_plan.hpp"

#include <cstdlib>
#include <format>

#include "pakon/protocol/f235_commands.hpp"

namespace pakon::scan {

namespace f = protocol::f235;

namespace {

// Filter wheel position for a film colour [DIS TLA@0x10033250].
int wheel(std::uint32_t colour) {
    if (colour == 1 || colour == 8) return 0xE3;
    if (colour == 2 || colour == 4) return 0xE4;
    if (colour == 0x1000 || colour == 0x2000) return 0xE5;
    return -1;
}

std::uint16_t sign_magnitude(int v) { // the F-135+ encoding too (OEM_RE §3)
    return static_cast<std::uint16_t>(std::abs(v) | (v < 0 ? 0x100 : 0));
}

} // namespace

Result<std::vector<F235Step>> f235_scan_plan(const F235Settings& s) {
    using Out = std::vector<F235Step>;
    const auto refuse = [](std::string why) { return failure<Out>(ErrorKind::scanner_unexpected_state, "F-235 plan: " + why); };
    const bool ir = s.lamp_ir != 0;
    const int pos = wheel(s.film_colour);
    if (pos < 0) return refuse(std::format("film colour 0x{:x} has no filter position", s.film_colour));
    if (s.lamp_level < f::kLampMin || s.lamp_level > f::kLampMax) return refuse("lamp level outside 0x2CEC..0x3A34");
    if (s.lamp_ir > f::kLampIrMax) return refuse("IR lamp level above 0x5DC");
    if (s.rollers > 3) return refuse("rollers position above 3");
    if (s.pixel_count < 6 || s.pixel_start + s.pixel_count > (s.binning ? 0x845 : 0x848)) return refuse("pixel window");
    if (s.integration > 0xFF0) return refuse("integration above 0xFF0");
    if (ir && (s.integration < 0x6E || s.ir_window > s.integration - 0x6E)) return refuse("IR window above integration - 0x6E");
    for (int c = 0; c < 3; ++c) {
        if (s.offsets[c] < -255 || s.offsets[c] > 255) return refuse("A/D offset outside -255..255");
        if (s.gains[c] > 0x3F) return refuse("A/D gain above 0x3F");
    }
    for (auto e : s.exposures) {
        if (e > 0xFFF) return refuse("exposure above 0xFFF");
    }
    if (s.drive_rate < 200 || s.drive_rate > 3700) return refuse("drive rate outside 200..3700");

    Out p;
    std::uint16_t ctrl = 0;
    const auto control = [&](std::uint16_t set, const char* why) {
        ctrl = static_cast<std::uint16_t>(ctrl | set);
        p.push_back({std::format("FPGA control |= 0x{:03x} ({}) -> 0x{:03x}", set, why, ctrl), f::ccd(f::kCcdFpga, 0, ctrl)});
    };
    p.push_back({"data flow start", f::data_flow_start()});                                // bBeforeScan
    p.push_back({std::format("pressure rollers {}", s.rollers), f::rollers(s.rollers)});
    p.push_back({std::format("lamp level 0x{:04x}", s.lamp_level), f::lamp_level(s.lamp_level)});
    p.push_back({std::format("filter wheel 0x{:02x}", pos), f::filter_wheel(static_cast<std::uint8_t>(pos))});
    if (ir) p.push_back({std::format("IR lamp 0x{:03x}", s.lamp_ir), f::lamp_ir(s.lamp_ir)});
    // FpgaSettings [DIS TLA@0x10032d20]
    p.push_back({std::format("integration 0x{:03x}", s.integration), f::ccd(f::kCcdFpga, 6, s.integration)});
    control(f::kCtrlFirstSettings, "first settings");
    p.push_back({"FPGA sub 0x0B = 0", f::ccd(f::kCcdFpga, 0x0B, 0)});
    if (ir) {
        control(f::kCtrlIr, "IR mode");
        p.push_back({std::format("IR window 0x{:03x}", s.ir_window), f::ccd(f::kCcdFpga, 10, s.ir_window)});
    }
    if (s.binning) control(f::kCtrlBinning, "binning");
    p.push_back({std::format("pixel count {}", s.pixel_count), f::ccd(f::kCcdFpga, 4, s.pixel_count)});
    p.push_back({std::format("pixel end {}", s.pixel_start + s.pixel_count),
                 f::ccd(f::kCcdFpga, 5, static_cast<std::uint16_t>(s.pixel_start + s.pixel_count))});
    // A/D offsets then gains [DIS TLA@0x1002fad0, 0x1002f9c0]
    for (std::uint8_t c = 0; c < 3; ++c) {
        p.push_back({std::format("A/D offset {} = {}", "RGB"[c], s.offsets[c]), f::ccd(f::kCcdAd, 5 + c, sign_magnitude(s.offsets[c]))});
    }
    for (std::uint8_t c = 0; c < 3; ++c) {
        p.push_back({std::format("A/D gain {} = {}", "RGB"[c], s.gains[c]), f::ccd(f::kCcdAd, 2 + c, s.gains[c])});
    }
    // Exposures [DIS TLA@0x10032680]
    const std::uint8_t sub[4]{1, 2, 3, 7};
    for (int c = 0; c < (ir ? 4 : 3); ++c) {
        p.push_back({std::format("exposure {} = 0x{:03x}", c == 3 ? 'I' : "RGB"[c], s.exposures[c]),
                     f::ccd(f::kCcdFpga, sub[c], s.exposures[c])});
    }
    p.push_back({"FPGA sub 8 = 0x860", f::ccd(f::kCcdFpga, 8, 0x860)});
    p.push_back({"FPGA sub 9 = 0x860", f::ccd(f::kCcdFpga, 9, 0x860)});
    control(f::kCtrlExposures, "exposures");
    control(f::kCtrlAcquire, "acquire");
    // Drive [DIS TLA@0x100100d0]
    p.push_back({std::format("drive rate {}", s.drive_rate), f::drive_rate(s.drive_rate)});
    p.push_back({s.reverse ? "drive reverse" : "drive forward", f::drive_go(s.reverse)});
    return p;
}

std::vector<F235Step> f235_teardown(bool ir) {
    // Acquire off (control without bit 0), the drive and every motor stopped
    // [DIS TLA@0x1000e4d0, 0x100015e0], the lamp off [DIS TLA@0x10033c70].
    std::uint16_t idle = f::kCtrlFirstSettings | f::kCtrlExposures | (ir ? f::kCtrlIr : 0);
    return {{"acquire off", f::ccd(f::kCcdFpga, 0, idle)},
            {"drive stop", f::drive_stop()},
            {"stop motors", f::stop_motors()},
            {"lamp off", f::lamp_off()}};
}

} // namespace pakon::scan
