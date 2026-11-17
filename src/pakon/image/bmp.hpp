#pragma once

// Minimal 24-bit BMP writer for 8-bit previews (the PakonX app shows them
// directly; WebView2 and every browser decode BMP). Uncompressed,
// bottom-up rows padded to 4 bytes. No dependency.

#include <cstdint>
#include <span>
#include <string>

#include "pakon/errors/error.hpp"

namespace pakon::image {

// `rgb`: width × height × 3 samples, row-major, top row first, R G B.
VoidResult write_bmp24(const std::string& path, std::uint32_t width, std::uint32_t height,
                       std::span<const std::uint8_t> rgb);

} // namespace pakon::image
