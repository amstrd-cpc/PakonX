#pragma once

// The firmware loader's view of a cold scanner: vendor control requests
// only, nothing else (no bulk, no PPB). Implemented by the libusb backend
// (Linux, macOS); on Windows the frozen PowerShell reference boots the
// scanner instead (tools/pakon_boot_reference.ps1), so there is none.
//
// Hardware safety: an implementation refuses everything but the requests
// the frozen reference sends (docs/BOOT_CHAIN.md): 0xA0 / 0xA3 RAM writes
// below 0xC000 or the CPUCS byte (0x7F92 / 0xE600, data 00 or 01), the
// 0xA4 / 0x00A1 no-data request and the one 0xA9 8-byte read, and only
// to a cold 0f05:f235 REV AA07 (an F-135 board, never an F-235's AA00..AA06).

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "pakon/errors/error.hpp"

namespace pakon::usb {

class ILoaderControl {
public:
    virtual ~ILoaderControl() = default;
    // bmRequestType 0x40, wIndex 0.
    virtual VoidResult vendor_out(std::uint8_t request, std::uint16_t value,
                                  std::span<const std::uint8_t> data) = 0;
    // bmRequestType 0xC0, wIndex 0.
    virtual Result<std::vector<std::uint8_t>> vendor_in(std::uint8_t request, std::uint16_t value,
                                                        std::uint16_t length) = 0;
};

// The request filter every implementation applies (pure; unit-tested).
bool loader_request_allowed(std::uint8_t request_type, std::uint8_t request, std::uint16_t value,
                            std::span<const std::uint8_t> data, std::uint16_t in_length = 0);

// Opens the cold F-135 board (0f05:f235, bcdDevice 0xAA07) for the loader.
// Fails on Windows (no libusb backend) and when no such board is attached.
Result<std::unique_ptr<ILoaderControl>> open_cold_loader();

} // namespace pakon::usb
