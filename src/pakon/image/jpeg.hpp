#pragma once

// Minimal baseline JPEG writer for finished pictures (frames --save jpeg,
// the PakonX save buttons): JFIF, 8-bit YCbCr, 4:4:4 (no chroma
// subsampling: film grain and fine colour edges survive), the ITU-T T.81
// Annex K quantisation tables scaled by `quality` like libjpeg, and the
// Annex K Huffman tables. No dependency.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "pakon/errors/error.hpp"

namespace pakon::image {

// `rgb`: width × height × 3 samples, row-major, top row first, R G B.
// `quality` 1..100 (libjpeg's scale: 50 = the Annex K tables as printed,
// 100 = every step 1).
Result<std::vector<std::uint8_t>> encode_jpeg(std::uint32_t width, std::uint32_t height,
                                              std::span<const std::uint8_t> rgb, int quality);

VoidResult write_jpeg(const std::string& path, std::uint32_t width, std::uint32_t height,
                      std::span<const std::uint8_t> rgb, int quality);

} // namespace pakon::image
