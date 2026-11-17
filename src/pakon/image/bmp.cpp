#include "pakon/image/bmp.hpp"

#include <cstdio>
#include <format>
#include <vector>

namespace pakon::image {

namespace {

void put16(std::vector<std::uint8_t>& b, std::size_t at, std::uint16_t v) {
    b[at] = static_cast<std::uint8_t>(v);
    b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}

void put32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    for (std::size_t i = 0; i < 4; ++i) {
        b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

} // namespace

VoidResult write_bmp24(const std::string& path, std::uint32_t width, std::uint32_t height,
                       std::span<const std::uint8_t> rgb) {
    if (width == 0 || height == 0 ||
        rgb.size() != static_cast<std::size_t>(width) * height * 3) {
        return void_failure(ErrorKind::image_bad_geometry,
                            std::format("BMP: {} samples for {}x{} RGB", rgb.size(), width, height));
    }
    const std::size_t stride = (static_cast<std::size_t>(width) * 3 + 3) & ~std::size_t{3};
    const std::size_t data = stride * height;
    std::vector<std::uint8_t> out(54 + data, 0);
    out[0] = 'B';
    out[1] = 'M';
    put32(out, 2, static_cast<std::uint32_t>(out.size()));
    put32(out, 10, 54);     // pixel data offset
    put32(out, 14, 40);     // BITMAPINFOHEADER
    put32(out, 18, width);
    put32(out, 22, height); // positive: bottom-up rows
    put16(out, 26, 1);      // planes
    put16(out, 28, 24);     // bits per pixel
    put32(out, 34, static_cast<std::uint32_t>(data));
    put32(out, 38, 11811);  // 300 dpi in pixels per metre
    put32(out, 42, 11811);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t* src = rgb.data() + static_cast<std::size_t>(height - 1 - y) * width * 3;
        std::uint8_t* dst = out.data() + 54 + y * stride;
        for (std::uint32_t x = 0; x < width; ++x) {
            dst[3 * x] = src[3 * x + 2]; // B G R
            dst[3 * x + 1] = src[3 * x + 1];
            dst[3 * x + 2] = src[3 * x];
        }
    }
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return void_failure(ErrorKind::io_failure, std::format("cannot create {}", path));
    }
    const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    const bool closed = std::fclose(f) == 0;
    if (!ok || !closed) {
        return void_failure(ErrorKind::io_failure, std::format("write failed: {}", path));
    }
    return {};
}

} // namespace pakon::image
