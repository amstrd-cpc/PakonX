// Platform-independent helpers for the USB layer.

#include "pakon/usb/loader_control.hpp"
#include "pakon/usb/transport.hpp"

namespace pakon::usb {

std::vector<DeviceInfo> enumerate_pakon() {
    std::vector<DeviceInfo> result;
    for (auto& device : enumerate()) {
        if (device.is_pakon()) {
            result.push_back(std::move(device));
        }
    }
    return result;
}

// Exactly the request shapes tools/pakon_boot_reference.ps1 sends.
bool loader_request_allowed(std::uint8_t request_type, std::uint8_t request, std::uint16_t value,
                            std::span<const std::uint8_t> data, std::uint16_t in_length) {
    if (request_type == 0xC0) {
        return request == 0xA9 && value == 0 && in_length == 8 && data.empty();
    }
    if (request_type != 0x40) {
        return false;
    }
    if (request == 0xA4) {
        return value == 0x00A1 && data.empty();
    }
    if (request != 0xA0 && request != 0xA3) {
        return false;
    }
    if (value == 0x7F92 || value == 0xE600) { // CPUCS: halt (01) / run (00), 0xA0 only
        return request == 0xA0 && data.size() == 1 && data[0] <= 1;
    }
    // A RAM record: 1..16 bytes, wholly below 0xC000.
    return !data.empty() && data.size() <= 16 && value + data.size() <= 0xC000;
}

#ifdef _WIN32
Result<std::unique_ptr<ILoaderControl>> open_cold_loader() {
    return failure<std::unique_ptr<ILoaderControl>>(
        ErrorKind::usb_not_supported,
        "on Windows the scanner boots with tools/pakon_boot_reference.ps1 (PakonX runs it)");
}
#endif

} // namespace pakon::usb
