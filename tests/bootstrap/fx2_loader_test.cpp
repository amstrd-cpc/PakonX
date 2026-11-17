// The portable firmware loader against the frozen reference
// (tools/pakon_boot_reference.ps1): request order and counts, the 0xA9
// gate, the request filter, MD5. No hardware. With the real firmware in
// apps/pakonx/src-tauri/firmware (not in git) the STEP-A gates run too.

#include "pakon/bootstrap/fx2_loader.hpp"
#include "pakon/usb/loader_control.hpp"
#include "support/test_harness.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace pakon;

namespace {

struct Req {
    std::uint8_t type, request;
    std::uint16_t value;
    std::vector<std::uint8_t> data;
};

class Recorder final : public usb::ILoaderControl {
public:
    std::vector<Req> log;
    std::vector<std::uint8_t> a9{0xC0, 0x05, 0x0F, 0x35, 0xF2, 0x07, 0xAA, 0x00};
    VoidResult vendor_out(std::uint8_t r, std::uint16_t v, std::span<const std::uint8_t> d) override {
        EXPECT(usb::loader_request_allowed(0x40, r, v, d));
        log.push_back({0x40, r, v, {d.begin(), d.end()}});
        return {};
    }
    Result<std::vector<std::uint8_t>> vendor_in(std::uint8_t r, std::uint16_t v, std::uint16_t n) override {
        EXPECT(usb::loader_request_allowed(0xC0, r, v, {}, n));
        log.push_back({0xC0, r, v, {}});
        return a9;
    }
};

bootstrap::FirmwareImages synthetic() {
    bootstrap::FirmwareImages img;
    for (int i = 0; i < 360; ++i) img.stage1.push_back({static_cast<std::uint16_t>(i * 12), {1, 2, 3}});
    img.pakon7 = {{0x2000, {9}}, {0x0100, {8}}, {0x478F, {7}}, {0x1B3F, {6}}};
    return img;
}

bool cpu_pair(const std::vector<Req>& l, std::size_t at, std::uint8_t v) {
    return l[at].request == 0xA0 && l[at].value == 0x7F92 && l[at].data == std::vector<std::uint8_t>{v} &&
           l[at + 1].request == 0xA0 && l[at + 1].value == 0xE600 && l[at + 1].data == std::vector<std::uint8_t>{v};
}

} // namespace

PAKON_TEST(md5_matches_the_rfc_vectors) {
    const auto md5 = [](const std::string& s) {
        return bootstrap::md5_hex({reinterpret_cast<const std::uint8_t*>(s.data()), s.size()});
    };
    EXPECT_EQ(md5(""), std::string("d41d8cd98f00b204e9800998ecf8427e"));
    EXPECT_EQ(md5("abc"), std::string("900150983cd24fb0d6963f7d28e17f72"));
    EXPECT_EQ(md5("12345678901234567890123456789012345678901234567890123456789012345678901234567890"),
              std::string("57edf4a22be3c955ac49da2e2107b67a"));
}

PAKON_TEST(boot_sends_the_reference_sequence) {
    Recorder usb;
    std::string log;
    EXPECT(bootstrap::boot(usb, synthetic(), log).has_value());
    const auto& l = usb.log;
    // halt, halt, 360 stage 1, run, A4, A9, 2 high (A3, file order), halt, 2 low (A0), halt, run
    EXPECT_EQ(l.size(), std::size_t(4 + 360 + 2 + 1 + 1 + 2 + 2 + 2 + 2 + 2));
    EXPECT(cpu_pair(l, 0, 1) && cpu_pair(l, 2, 1));
    EXPECT(l[4].request == 0xA0 && l[4].value == 0 && l[363].value == 359 * 12);
    std::size_t at = 364;
    EXPECT(cpu_pair(l, at, 0));
    EXPECT(l[at + 2].request == 0xA4 && l[at + 2].value == 0x00A1 && l[at + 2].data.empty());
    EXPECT(l[at + 3].type == 0xC0 && l[at + 3].request == 0xA9);
    EXPECT(l[at + 4].request == 0xA3 && l[at + 4].value == 0x2000 && l[at + 5].value == 0x478F);
    EXPECT(cpu_pair(l, at + 6, 1));
    EXPECT(l[at + 8].request == 0xA0 && l[at + 8].value == 0x0100 && l[at + 9].value == 0x1B3F);
    EXPECT(cpu_pair(l, at + 10, 1) && cpu_pair(l, at + 12, 0));
    EXPECT(log.find("STEP11 final run pair -> OK") != std::string::npos);
}

PAKON_TEST(a_wrong_personality_stops_before_any_pakon7_record) {
    Recorder usb;
    usb.a9[6] = 0xA6; // an F-235 board revision
    std::string log;
    EXPECT(!bootstrap::boot(usb, synthetic(), log).has_value());
    EXPECT(usb.log.back().request == 0xA9);
    EXPECT(log.find("gate closed") != std::string::npos && log.find("no retry") != std::string::npos);
}

PAKON_TEST(the_filter_allows_only_the_reference_requests) {
    const std::vector<std::uint8_t> one{1}, two{0, 1}, sixteen(16, 0), seventeen(17, 0);
    EXPECT(usb::loader_request_allowed(0x40, 0xA0, 0xE600, one));
    EXPECT(!usb::loader_request_allowed(0x40, 0xA0, 0xE600, two));
    EXPECT(!usb::loader_request_allowed(0x40, 0xA3, 0xE600, one));
    EXPECT(usb::loader_request_allowed(0x40, 0xA3, 0x478F, sixteen));
    EXPECT(!usb::loader_request_allowed(0x40, 0xA0, 0xBFF8, sixteen)); // runs past 0xC000
    EXPECT(!usb::loader_request_allowed(0x40, 0xA0, 0x0000, seventeen));
    EXPECT(!usb::loader_request_allowed(0x40, 0xA2, 0x0000, one));    // EEPROM write
    EXPECT(usb::loader_request_allowed(0x40, 0xA4, 0x00A1, {}));
    EXPECT(!usb::loader_request_allowed(0x40, 0xA4, 0x0000, {}));
    EXPECT(usb::loader_request_allowed(0xC0, 0xA9, 0, {}, 8));
    EXPECT(!usb::loader_request_allowed(0xC0, 0xA9, 0, {}, 64));
}

PAKON_TEST(the_real_firmware_passes_step_a_when_present) {
    const std::string dir = std::string(PAKON_SOURCE_DIR) + "/apps/pakonx/src-tauri/firmware/";
    std::ifstream l(dir + "F235Ldr.sys", std::ios::binary), h(dir + "Pakon7.hex", std::ios::binary);
    if (!l || !h) return; // not in git: checked where the files are
    const std::string ldr(std::istreambuf_iterator<char>(l), {});
    const std::string hex(std::istreambuf_iterator<char>(h), {});
    auto img = bootstrap::load_images({reinterpret_cast<const std::uint8_t*>(ldr.data()), ldr.size()}, hex);
    EXPECT(img.has_value());
    if (img) {
        EXPECT_EQ(img->stage1.size(), std::size_t(360));
        EXPECT_EQ(img->pakon7.size(), std::size_t(728));
    }
    auto bad = bootstrap::load_images({reinterpret_cast<const std::uint8_t*>(ldr.data()), ldr.size() - 1}, hex);
    EXPECT(!bad.has_value());
}

int main() { return pakon::test::run_all(); }
