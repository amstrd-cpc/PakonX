#include "pakon/image/jpeg.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <format>
#include <numbers>

namespace pakon::image {

namespace {

// Natural (row-major) index of each zig-zag position, T.81 Figure A.6.
constexpr std::array<std::uint8_t, 64> kZigzag = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// T.81 Annex K.1, natural order.
constexpr std::array<std::uint8_t, 64> kLumaQuant = {
    16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
    14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
    18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
constexpr std::array<std::uint8_t, 64> kChromaQuant = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

// T.81 Annex K.3: code counts per length 1..16, then the symbols.
constexpr std::array<std::uint8_t, 16> kDcLumaBits = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
constexpr std::array<std::uint8_t, 16> kDcChromaBits = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
constexpr std::array<std::uint8_t, 12> kDcValues = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
constexpr std::array<std::uint8_t, 16> kAcLumaBits = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
constexpr std::array<std::uint8_t, 162> kAcLumaValues = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61,
    0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52,
    0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25,
    0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64,
    0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
    0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
    0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3,
    0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8,
    0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};
constexpr std::array<std::uint8_t, 16> kAcChromaBits = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
constexpr std::array<std::uint8_t, 162> kAcChromaValues = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61,
    0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33,
    0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18,
    0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44,
    0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63,
    0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
    0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca,
    0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
    0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

// Code and length per symbol (T.81 Annex C).
struct Huffman {
    std::array<std::uint16_t, 256> code{};
    std::array<std::uint8_t, 256> size{};
};

Huffman build_huffman(std::span<const std::uint8_t, 16> bits, std::span<const std::uint8_t> values) {
    Huffman h;
    std::uint16_t code = 0;
    std::size_t k = 0;
    for (std::size_t len = 1; len <= 16; ++len) {
        for (std::size_t i = 0; i < bits[len - 1]; ++i) {
            h.code[values[k]] = code++;
            h.size[values[k]] = static_cast<std::uint8_t>(len);
            ++k;
        }
        code = static_cast<std::uint16_t>(code << 1);
    }
    return h;
}

// libjpeg's quality scaling of a base table, returned in zig-zag order.
std::array<std::uint8_t, 64> scaled_quant(const std::array<std::uint8_t, 64>& base, int quality) {
    const int scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;
    std::array<std::uint8_t, 64> zz{};
    for (std::size_t k = 0; k < 64; ++k) {
        const int q = (base[kZigzag[k]] * scale + 50) / 100;
        zz[k] = static_cast<std::uint8_t>(std::clamp(q, 1, 255));
    }
    return zz;
}

class BitWriter {
public:
    explicit BitWriter(std::vector<std::uint8_t>& out) : out_(out) {}

    void put(std::uint32_t bits, std::uint32_t count) {
        acc_ = (acc_ << count) | (bits & ((1u << count) - 1));
        n_ += count;
        while (n_ >= 8) {
            n_ -= 8;
            const auto byte = static_cast<std::uint8_t>(acc_ >> n_);
            out_.push_back(byte);
            if (byte == 0xFF) {
                out_.push_back(0x00); // byte stuffing, T.81 F.1.2.3
            }
        }
        acc_ &= (1u << n_) - 1;
    }

    // Pads the last byte with 1 bits.
    void flush() {
        if (n_ > 0) {
            put(0x7F, 8 - n_);
        }
    }

private:
    std::vector<std::uint8_t>& out_;
    std::uint32_t acc_{0};
    std::uint32_t n_{0};
};

// Bit length of |v| (the JPEG magnitude category) and its amplitude bits.
std::pair<std::uint32_t, std::uint32_t> category(int v) {
    const auto mag = static_cast<std::uint32_t>(v < 0 ? -v : v);
    std::uint32_t n = 0;
    while ((mag >> n) != 0) {
        ++n;
    }
    const auto bits = v < 0 ? static_cast<std::uint32_t>(v + (1 << n) - 1) : mag;
    return {n, bits};
}

// c[u][x] = C(u)/2 cos((2x+1)uπ/16): F = c · f · cᵀ is the T.81 A.3.3 FDCT.
std::array<std::array<double, 8>, 8> dct_matrix() {
    std::array<std::array<double, 8>, 8> c{};
    for (std::size_t u = 0; u < 8; ++u) {
        const double cu = u == 0 ? std::numbers::sqrt2 / 2.0 : 1.0;
        for (std::size_t x = 0; x < 8; ++x) {
            c[u][x] = cu / 2.0 *
                      std::cos(static_cast<double>(2 * x + 1) * static_cast<double>(u) *
                               std::numbers::pi / 16.0);
        }
    }
    return c;
}

void put_marker(std::vector<std::uint8_t>& o, std::uint8_t m) {
    o.push_back(0xFF);
    o.push_back(m);
}

void put_u16(std::vector<std::uint8_t>& o, std::size_t v) {
    o.push_back(static_cast<std::uint8_t>(v >> 8));
    o.push_back(static_cast<std::uint8_t>(v));
}

void put_dht(std::vector<std::uint8_t>& o, std::uint8_t cls_id, std::span<const std::uint8_t, 16> bits,
             std::span<const std::uint8_t> values) {
    put_marker(o, 0xC4);
    put_u16(o, 2 + 1 + 16 + values.size());
    o.push_back(cls_id);
    o.insert(o.end(), bits.begin(), bits.end());
    o.insert(o.end(), values.begin(), values.end());
}

} // namespace

Result<std::vector<std::uint8_t>> encode_jpeg(std::uint32_t width, std::uint32_t height,
                                              std::span<const std::uint8_t> rgb, int quality) {
    if (width == 0 || height == 0 || width > 65535 || height > 65535 ||
        rgb.size() != static_cast<std::size_t>(width) * height * 3) {
        return failure<std::vector<std::uint8_t>>(
            ErrorKind::image_bad_geometry,
            std::format("JPEG: {} samples for {}x{} RGB", rgb.size(), width, height));
    }
    quality = std::clamp(quality, 1, 100);
    const std::array<std::array<std::uint8_t, 64>, 2> quant = {scaled_quant(kLumaQuant, quality),
                                                               scaled_quant(kChromaQuant, quality)};
    const std::array<Huffman, 2> dc = {build_huffman(kDcLumaBits, kDcValues),
                                       build_huffman(kDcChromaBits, kDcValues)};
    const std::array<Huffman, 2> ac = {build_huffman(kAcLumaBits, kAcLumaValues),
                                       build_huffman(kAcChromaBits, kAcChromaValues)};
    const auto c = dct_matrix();

    std::vector<std::uint8_t> o;
    o.reserve(rgb.size() / 4 + 1024);
    put_marker(o, 0xD8); // SOI
    put_marker(o, 0xE0); // APP0 JFIF 1.01, 300 dpi like the BMP previews
    put_u16(o, 16);
    o.insert(o.end(), {'J', 'F', 'I', 'F', 0, 1, 1, 1});
    put_u16(o, 300);
    put_u16(o, 300);
    o.insert(o.end(), {0, 0});
    for (std::uint8_t t = 0; t < 2; ++t) {
        put_marker(o, 0xDB); // DQT, 8-bit precision
        put_u16(o, 2 + 1 + 64);
        o.push_back(t);
        o.insert(o.end(), quant[t].begin(), quant[t].end());
    }
    put_marker(o, 0xC0); // SOF0: baseline, 8 bit, 3 components, 1x1 sampling
    put_u16(o, 2 + 6 + 3 * 3);
    o.push_back(8);
    put_u16(o, height);
    put_u16(o, width);
    o.push_back(3);
    for (std::uint8_t id = 1; id <= 3; ++id) {
        o.insert(o.end(), {id, 0x11, static_cast<std::uint8_t>(id == 1 ? 0 : 1)});
    }
    put_dht(o, 0x00, kDcLumaBits, kDcValues);
    put_dht(o, 0x10, kAcLumaBits, kAcLumaValues);
    put_dht(o, 0x01, kDcChromaBits, kDcValues);
    put_dht(o, 0x11, kAcChromaBits, kAcChromaValues);
    put_marker(o, 0xDA); // SOS: all three components, full spectral range
    put_u16(o, 2 + 1 + 3 * 2 + 3);
    o.push_back(3);
    o.insert(o.end(), {1, 0x00, 2, 0x11, 3, 0x11});
    o.insert(o.end(), {0, 63, 0});

    BitWriter bw(o);
    std::array<int, 3> prev_dc{};
    std::array<std::array<double, 64>, 3> block{};
    for (std::uint32_t by = 0; by < height; by += 8) {
        for (std::uint32_t bx = 0; bx < width; bx += 8) {
            // Level-shifted YCbCr (JFIF), edge pixels repeated past the border.
            for (std::uint32_t y = 0; y < 8; ++y) {
                const std::size_t sy = std::min(by + y, height - 1);
                for (std::uint32_t x = 0; x < 8; ++x) {
                    const std::size_t sx = std::min(bx + x, width - 1);
                    const auto* p = rgb.data() + (sy * width + sx) * 3;
                    const double r = p[0], g = p[1], b = p[2];
                    block[0][y * 8 + x] = 0.299 * r + 0.587 * g + 0.114 * b - 128.0;
                    block[1][y * 8 + x] = -0.168736 * r - 0.331264 * g + 0.5 * b;
                    block[2][y * 8 + x] = 0.5 * r - 0.418688 * g - 0.081312 * b;
                }
            }
            for (std::size_t comp = 0; comp < 3; ++comp) {
                const std::size_t t = comp == 0 ? 0 : 1;
                // Separable FDCT: rows, then columns.
                std::array<double, 64> tmp{};
                for (std::size_t y = 0; y < 8; ++y) {
                    for (std::size_t u = 0; u < 8; ++u) {
                        double s = 0;
                        for (std::size_t x = 0; x < 8; ++x) {
                            s += c[u][x] * block[comp][y * 8 + x];
                        }
                        tmp[y * 8 + u] = s;
                    }
                }
                std::array<int, 64> zz{};
                for (std::size_t k = 0; k < 64; ++k) {
                    const std::size_t v = kZigzag[k] / 8;
                    const std::size_t u = kZigzag[k] % 8;
                    double s = 0;
                    for (std::size_t y = 0; y < 8; ++y) {
                        s += c[v][y] * tmp[y * 8 + u];
                    }
                    zz[k] = static_cast<int>(std::lround(s / quant[t][k]));
                }
                const auto [dn, dbits] = category(zz[0] - prev_dc[comp]);
                prev_dc[comp] = zz[0];
                bw.put(dc[t].code[dn], dc[t].size[dn]);
                bw.put(dbits, dn);
                std::uint32_t run = 0;
                for (std::size_t k = 1; k < 64; ++k) {
                    if (zz[k] == 0) {
                        ++run;
                        continue;
                    }
                    while (run > 15) {
                        bw.put(ac[t].code[0xF0], ac[t].size[0xF0]); // ZRL
                        run -= 16;
                    }
                    const auto [an, abits] = category(zz[k]);
                    const auto sym = (run << 4) | an;
                    bw.put(ac[t].code[sym], ac[t].size[sym]);
                    bw.put(abits, an);
                    run = 0;
                }
                if (run > 0) {
                    bw.put(ac[t].code[0x00], ac[t].size[0x00]); // EOB
                }
            }
        }
    }
    bw.flush();
    put_marker(o, 0xD9); // EOI
    return o;
}

VoidResult write_jpeg(const std::string& path, std::uint32_t width, std::uint32_t height,
                      std::span<const std::uint8_t> rgb, int quality) {
    auto bytes = encode_jpeg(width, height, rgb, quality);
    if (!bytes) {
        return void_failure(bytes.error().kind, bytes.error().message);
    }
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return void_failure(ErrorKind::io_failure, std::format("cannot create {}", path));
    }
    const bool ok = std::fwrite(bytes->data(), 1, bytes->size(), f) == bytes->size();
    const bool closed = std::fclose(f) == 0;
    if (!ok || !closed) {
        return void_failure(ErrorKind::io_failure, std::format("write failed: {}", path));
    }
    return {};
}

} // namespace pakon::image
