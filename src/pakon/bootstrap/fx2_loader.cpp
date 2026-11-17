#include "pakon/bootstrap/fx2_loader.hpp"

#include <array>
#include <cstring>
#include <format>

namespace pakon::bootstrap {

namespace {

// RFC 1321, for the two pinned digests only.
struct Md5 {
    std::array<std::uint32_t, 4> h{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    void block(const std::uint8_t* p) {
        static constexpr std::uint32_t k[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
            0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
            0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
            0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
            0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
            0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        static constexpr int r[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                      5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                      4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                      6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        std::uint32_t m[16];
        for (int i = 0; i < 16; ++i) {
            m[i] = p[4 * i] | (p[4 * i + 1] << 8) | (p[4 * i + 2] << 16) | (std::uint32_t(p[4 * i + 3]) << 24);
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t f;
            int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            const std::uint32_t t = d;
            d = c;
            c = b;
            const std::uint32_t x = a + f + k[i] + m[g];
            b = b + ((x << r[i]) | (x >> (32 - r[i])));
            a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }
};

constexpr std::uint16_t kLowTop = 0x1B3F; // phase split: above = 0xA3 while running

} // namespace

std::string md5_hex(std::span<const std::uint8_t> data) {
    Md5 md;
    std::size_t i = 0;
    for (; i + 64 <= data.size(); i += 64) {
        md.block(data.data() + i);
    }
    std::array<std::uint8_t, 128> tail{};
    const std::size_t rest = data.size() - i;
    if (rest) std::memcpy(tail.data(), data.data() + i, rest);
    tail[rest] = 0x80;
    const std::size_t len = rest < 56 ? 64 : 128;
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    for (int b = 0; b < 8; ++b) tail[len - 8 + b] = static_cast<std::uint8_t>(bits >> (8 * b));
    md.block(tail.data());
    if (len == 128) md.block(tail.data() + 64);
    std::string out;
    for (auto w : md.h) {
        for (int b = 0; b < 4; ++b) out += std::format("{:02x}", (w >> (8 * b)) & 0xFF);
    }
    return out;
}

Result<FirmwareImages> load_images(std::span<const std::uint8_t> ldr, const std::string& hex) {
    const auto fail = [](std::string m) { return failure<FirmwareImages>(ErrorKind::unknown, "STEP-A  " + m); };
    const std::span<const std::uint8_t> hex_bytes(reinterpret_cast<const std::uint8_t*>(hex.data()), hex.size());
    if (const auto h1 = md5_hex(ldr); h1 != kLoaderMd5) return fail("F235Ldr.sys MD5 MISMATCH: " + h1);
    if (const auto h2 = md5_hex(hex_bytes); h2 != kPakon7Md5) return fail("Pakon7.hex MD5 MISMATCH: " + h2);
    FirmwareImages img;
    // Stage 1: F235Ldr .data 0x1700..0x35EF, 360 records of 0x16, then the terminator.
    if (ldr.size() < 0x3606) return fail("stage1 sanity failed");
    std::size_t pay = 0;
    for (std::size_t i = 0; i < 360; ++i) {
        const auto* r = ldr.data() + 0x1700 + i * 22;
        const std::size_t ln = r[0];
        if (r[1] != 0 || r[4] != 0 || ln < 1 || ln > 16) return fail(std::format("stage1 record {} invalid", i));
        const std::uint16_t a = static_cast<std::uint16_t>(r[2] | (r[3] << 8));
        if (a > kLowTop) return fail(std::format("stage1 record {} addr > 0x1B3F", i));
        img.stage1.push_back({a, std::vector<std::uint8_t>(r + 5, r + 5 + ln)});
        pay += ln;
    }
    if (pay != 4476 || ldr[0x35F0] != 0 || ldr[0x35F0 + 4] == 0) return fail("stage1 sanity failed");
    // Pakon7.hex (Intel HEX).
    std::size_t fpay = 0, hi = 0, hipay = 0, lo = 0, lop = 0;
    unsigned max_a = 0;
    std::size_t at = 0;
    while (at < hex.size()) {
        std::size_t end = hex.find('\n', at);
        if (end == std::string::npos) end = hex.size();
        std::string line = hex.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(0, 1);
        if (line.empty()) continue;
        if (line[0] != ':') return fail("Pakon7: line does not start with ':'");
        std::vector<std::uint8_t> nb((line.size() - 1) / 2);
        for (std::size_t k = 0; k < nb.size(); ++k) {
            unsigned v = 0;
            for (int d = 0; d < 2; ++d) {
                const char c = line[1 + 2 * k + d];
                const int x = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (x < 0) return fail("Pakon7: bad hex digit");
                v = v * 16 + static_cast<unsigned>(x);
            }
            nb[k] = static_cast<std::uint8_t>(v);
        }
        if (nb.size() < 4) return fail("Pakon7: short record");
        const std::size_t cnt = nb[0];
        const unsigned a = (nb[1] << 8) | nb[2];
        const auto typ = nb[3];
        if (typ == 0) {
            if (cnt < 1 || cnt > 16 || nb.size() < 4 + cnt) return fail(std::format("HEX record len {} out of range", cnt));
            if (a >= 0xC000) return fail(std::format("unsafe addr 0x{:x} (>= 0xC000 incl. CPUCS)", a));
            img.pakon7.push_back({static_cast<std::uint16_t>(a), std::vector<std::uint8_t>(nb.begin() + 4, nb.begin() + 4 + cnt)});
            fpay += cnt;
            max_a = std::max(max_a, a);
            if (a > kLowTop) { ++hi; hipay += cnt; } else { ++lo; lop += cnt; }
        } else if (typ != 1) {
            return fail(std::format("unexpected HEX record type {}", typ));
        }
    }
    if (img.pakon7.size() != 728 || fpay != 10326 || hi != 709 || hipay != 10128 || lo != 19 || lop != 198 || max_a != 0x478F) {
        return fail(std::format("Pakon7 shape mismatch: n={} pay={} hi={}/{} lo={}/{} max=0x{:x}", img.pakon7.size(), fpay, hi, hipay, lo, lop, max_a));
    }
    return img;
}

VoidResult boot(usb::ILoaderControl& usb, const FirmwareImages& img, std::string& log) {
    const auto line = [&](const std::string& s) { log += s + "\n"; };
    const auto stop = [&](const std::string& why) {
        line("BOOT SEQUENCE STOPPED - see failing step above; no retry");
        return void_failure(ErrorKind::usb_io_failed, why);
    };
    const auto cpu = [&](std::uint8_t v) -> VoidResult {
        const std::uint8_t d[1] = {v};
        if (auto r = usb.vendor_out(0xA0, 0x7F92, d); !r) return r;
        return usb.vendor_out(0xA0, 0xE600, d);
    };
    const auto phase = [&](std::uint8_t req, bool high, std::size_t& sent) -> VoidResult {
        for (const auto& r : img.pakon7) {
            if ((r.address > kLowTop) == high) {
                if (auto w = usb.vendor_out(req, r.address, r.data); !w) return w;
                ++sent;
            }
        }
        return {};
    };
    if (auto r = cpu(1); !r) { line("STEP1  CPUCS halt pair -> " + r.error().message); return stop(r.error().message); }
    line("STEP1  CPUCS halt pair (7F92 then E600, data 01) -> OK");
    if (auto r = cpu(1); !r) { line("STEP2  re-halt pair -> " + r.error().message); return stop(r.error().message); }
    line("STEP2  in-downloader re-halt pair -> OK");
    for (std::size_t i = 0; i < img.stage1.size(); ++i) {
        const auto& s = img.stage1[i];
        if (auto r = usb.vendor_out(0xA0, s.address, s.data); !r) {
            line(std::format("STEP3  FAIL at record {} (addr 0x{:04X}, len {}) -> {}", i, s.address, s.data.size(), r.error().message));
            return stop(r.error().message);
        }
    }
    line("STEP3  360 x 0xA0 stage-1 write -> OK (4476 bytes)");
    if (auto r = cpu(0); !r) { line("STEP4  CPUCS run pair -> " + r.error().message); return stop(r.error().message); }
    line("STEP4  CPUCS run pair (data 00) -> OK");
    if (auto r = usb.vendor_out(0xA4, 0x00A1, {}); !r) { line("STEP5  40 A4 00A1 0 0 -> " + r.error().message); return stop(r.error().message); }
    line("STEP5  40 A4 00A1 0 0 -> OK");
    auto id = usb.vendor_in(0xA9, 0, 8);
    if (!id) {
        line("STEP6  0xA9 -> " + id.error().message + " -> gate closed");
        line("STEP7-10 skipped: 0xA9 gate not passed - no file download, no retry");
        return stop(id.error().message);
    }
    std::string hx;
    for (std::size_t i = 0; i < id->size(); ++i) hx += std::format("{}{:02X}", i ? "-" : "", (*id)[i]);
    const auto& b = *id;
    const bool good = b.size() == 8 && b[0] == 0xC0 && b[1] == 0x05 && b[2] == 0x0F && b[3] == 0x35 && b[4] == 0xF2 && b[5] == 0x07 && b[6] == 0xAA;
    if (!good) {
        line("STEP6  0xA9 UNEXPECTED: " + hx + " -> gate closed");
        line("STEP7-10 skipped: 0xA9 gate not passed - no file download, no retry");
        return stop("0xA9 gate closed");
    }
    line("STEP6  0xA9 verified: " + hx + "  (tag C0, 0F05:F235 REV AA07 -> F235_AA07 -> Pakon7.hex)");
    std::size_t sent = 0;
    if (auto r = phase(0xA3, true, sent); !r) { line(std::format("STEP7  phase1 FAIL after {} records -> {}", sent, r.error().message)); return stop(r.error().message); }
    line(std::format("STEP7  phase1: {} x 0xA3 (addr > 0x1B3F, CPU running) -> OK", sent));
    if (auto r = cpu(1); !r) { line("STEP8  halt pair -> " + r.error().message); return stop(r.error().message); }
    line("STEP8  CPUCS halt pair before phase2 -> OK");
    sent = 0;
    if (auto r = phase(0xA0, false, sent); !r) { line(std::format("STEP9  phase2 FAIL after {} records -> {}", sent, r.error().message)); return stop(r.error().message); }
    line(std::format("STEP9  phase2: {} x 0xA0 (addr <= 0x1B3F, halted) -> OK", sent));
    if (auto r = cpu(1); !r) { line("STEP10 final halt -> " + r.error().message); return stop(r.error().message); }
    line("STEP10 final halt pair -> OK");
    if (auto r = cpu(0); !r) { line("STEP11 final run -> " + r.error().message); return stop(r.error().message); }
    line("STEP11 final run pair -> OK (Pakon7 executing; renumeration expected now)");
    return {};
}

} // namespace pakon::bootstrap
