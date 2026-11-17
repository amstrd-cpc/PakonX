// Offline tests for the baseline JPEG writer: the marker layout (JFIF,
// two DQT, SOF0 4:4:4, four DHT, SOS, EOI), libjpeg quality scaling of
// the Annex K tables, a clean entropy segment (every 0xFF stuffed), and
// geometry refusals. Decoding is checked by hand with a real decoder.

#include <cstdint>
#include <vector>

#include "pakon/image/jpeg.hpp"
#include "support/test_harness.hpp"

namespace {

using namespace pakon;
using namespace pakon::image;

std::uint16_t be16(const std::vector<std::uint8_t>& b, std::size_t at) {
    return static_cast<std::uint16_t>((b[at] << 8) | b[at + 1]);
}

struct Segment {
    std::uint8_t marker;
    std::size_t at; // offset of the length field
};

// Marker segments up to SOS; `scan` = first byte of entropy data.
std::vector<Segment> segments(const std::vector<std::uint8_t>& b, std::size_t& scan) {
    std::vector<Segment> out;
    std::size_t i = 2;
    while (i + 4 <= b.size() && b[i] == 0xFF) {
        const std::uint8_t m = b[i + 1];
        out.push_back({m, i + 2});
        i += 2 + be16(b, i + 2);
        if (m == 0xDA) {
            break;
        }
    }
    scan = i;
    return out;
}

// Odd sizes exercise the edge-repeated border blocks.
std::vector<std::uint8_t> gradient(std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(w) * h * 3);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            auto* p = rgb.data() + (static_cast<std::size_t>(y) * w + x) * 3;
            p[0] = static_cast<std::uint8_t>(x * 255 / (w - 1));
            p[1] = static_cast<std::uint8_t>(y * 255 / (h - 1));
            p[2] = static_cast<std::uint8_t>(((x ^ y) & 1) ? 255 : 0);
        }
    }
    return rgb;
}

} // namespace

PAKON_TEST(jpeg_marker_layout_is_baseline_jfif_444) {
    auto j = encode_jpeg(37, 21, gradient(37, 21), 92);
    EXPECT(j.has_value());
    if (!j) {
        return;
    }
    const auto& b = *j;
    EXPECT_EQ(b[0], std::uint8_t{0xFF});
    EXPECT_EQ(b[1], std::uint8_t{0xD8});
    EXPECT_EQ(b[b.size() - 2], std::uint8_t{0xFF});
    EXPECT_EQ(b[b.size() - 1], std::uint8_t{0xD9});
    std::size_t scan = 0;
    const auto seg = segments(b, scan);
    std::vector<std::uint8_t> order;
    for (const auto& s : seg) {
        order.push_back(s.marker);
    }
    EXPECT_EQ(order, (std::vector<std::uint8_t>{0xE0, 0xDB, 0xDB, 0xC0, 0xC4, 0xC4, 0xC4, 0xC4, 0xDA}));
    if (order.size() != 9) {
        return;
    }
    EXPECT_EQ(b[seg[0].at + 2], std::uint8_t{'J'});
    const std::size_t sof = seg[3].at;
    EXPECT_EQ(b[sof + 2], std::uint8_t{8});         // precision
    EXPECT_EQ(be16(b, sof + 3), std::uint16_t{21}); // height
    EXPECT_EQ(be16(b, sof + 5), std::uint16_t{37}); // width
    EXPECT_EQ(b[sof + 7], std::uint8_t{3});
    for (std::size_t c = 0; c < 3; ++c) {
        EXPECT_EQ(b[sof + 9 + 3 * c], std::uint8_t{0x11}); // no subsampling
    }
    // Entropy data: every 0xFF is stuffed (followed by 0x00) up to EOI.
    for (std::size_t i = scan; i + 2 < b.size(); ++i) {
        if (b[i] == 0xFF) {
            EXPECT_EQ(b[i + 1], std::uint8_t{0x00});
            ++i;
        }
    }
}

PAKON_TEST(jpeg_quality_scales_the_annex_k_tables_like_libjpeg) {
    const auto grey = std::vector<std::uint8_t>(8 * 8 * 3, 128);
    auto q50 = encode_jpeg(8, 8, grey, 50);
    auto q100 = encode_jpeg(8, 8, grey, 100);
    auto q10 = encode_jpeg(8, 8, grey, 10);
    EXPECT(q50 && q100 && q10);
    if (!q50 || !q100 || !q10) {
        return;
    }
    std::size_t scan = 0;
    const auto s50 = segments(*q50, scan);
    const auto s100 = segments(*q100, scan);
    const auto s10 = segments(*q10, scan);
    // DQT luma: table id, then zig-zag entries 0, 1, 2 = natural 0, 1, 8.
    const std::size_t d = s50[1].at + 2;
    EXPECT_EQ((*q50)[d], std::uint8_t{0});
    EXPECT_EQ((*q50)[d + 1], std::uint8_t{16});
    EXPECT_EQ((*q50)[d + 2], std::uint8_t{11});
    EXPECT_EQ((*q50)[d + 3], std::uint8_t{12});
    EXPECT_EQ((*q50)[d + 64], std::uint8_t{99});
    for (std::size_t k = 0; k < 64; ++k) {
        EXPECT_EQ((*q100)[s100[1].at + 3 + k], std::uint8_t{1});
        EXPECT_EQ((*q100)[s100[2].at + 3 + k], std::uint8_t{1});
    }
    // Quality 10: scale 500 %, 16 -> 80, the 99s clamp at 255.
    EXPECT_EQ((*q10)[s10[1].at + 3], std::uint8_t{80});
    EXPECT_EQ((*q10)[s10[2].at + 3 + 63], std::uint8_t{255});
}

PAKON_TEST(jpeg_flat_grey_codes_to_almost_nothing) {
    const auto grey = std::vector<std::uint8_t>(64 * 64 * 3, 128);
    auto j = encode_jpeg(64, 64, grey, 92);
    EXPECT(j.has_value());
    std::size_t scan = 0;
    if (j) {
        segments(*j, scan);
        // 64 blocks x 3 components, each DC 0 + EOB: 10 bits a block.
        EXPECT(j->size() - scan < 128);
    }
}

PAKON_TEST(jpeg_refuses_bad_geometry) {
    EXPECT(!encode_jpeg(0, 8, {}, 90).has_value());
    EXPECT(!encode_jpeg(4, 4, std::vector<std::uint8_t>(47), 90).has_value());
    EXPECT(!encode_jpeg(70000, 1, std::vector<std::uint8_t>(210000), 90).has_value());
}

int main() { return pakon::test::run_all(); }
