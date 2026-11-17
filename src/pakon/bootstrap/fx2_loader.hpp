#pragma once

// The portable firmware loader (Linux, macOS): boots a cold F-135 board
// (0f05:f235 REV AA07) into Pakon7, request for request as the frozen
// reference tools/pakon_boot_reference.ps1 (docs/BOOT_CHAIN.md,
// docs/LOADER_DESIGN.md). Windows keeps running the reference itself.
//
// Same gates, same order, same abort-never-retry:
//   STEP-A host side, before any USB request: MD5 of both files, the
//          stage-1 records (360, 4476 B, addr <= 0x1B3F, terminator), the
//          Pakon7 shape (728 records / 10326 B = 709/10128 above 0x1B3F +
//          19/198 at or below, max addr 0x478F, nothing >= 0xC000);
//   STEP1/2  CPUCS halt pair (0xA0 7F92 then E600, data 01), twice;
//   STEP3  360 x 0xA0 stage-1 records;   STEP4 CPUCS run pair (00);
//   STEP5  0xA4 00A1, no data;           STEP6 one 0xA9 read, 8 bytes,
//          must be C0 05 0F 35 F2 07 AA (byte 7 free) or the run stops;
//   STEP7  0xA3 for every Pakon7 record above 0x1B3F, in file order;
//   STEP8  halt pair;  STEP9 0xA0 for the records at or below 0x1B3F;
//   STEP10 halt pair;  STEP11 run pair: Pakon7 runs and re-enumerates.
// Every write is volatile FX2 RAM: unplugging restores the cold state.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "pakon/errors/error.hpp"
#include "pakon/usb/loader_control.hpp"

namespace pakon::bootstrap {

inline constexpr const char* kLoaderMd5 = "6be781bad6fb9cf7ade4095baed336e0";  // F235Ldr.sys
inline constexpr const char* kPakon7Md5 = "07f5001a951c4be179008d948be82b69";  // Pakon7.hex

std::string md5_hex(std::span<const std::uint8_t> data);

struct FirmwareRecord {
    std::uint16_t address{0};
    std::vector<std::uint8_t> data; // 1..16 bytes
};

struct FirmwareImages {
    std::vector<FirmwareRecord> stage1; // 360, from F235Ldr.sys
    std::vector<FirmwareRecord> pakon7; // 728, Pakon7.hex in file order
};

// STEP-A: every host-side gate; the error text names the failed gate.
Result<FirmwareImages> load_images(std::span<const std::uint8_t> loader_sys, const std::string& pakon7_hex);

// STEP1..STEP11 over `usb`; `log` gets the reference's STEP lines.
// Ok when STEP11 was acknowledged.
VoidResult boot(usb::ILoaderControl& usb, const FirmwareImages& images, std::string& log);

} // namespace pakon::bootstrap
