#include "pakon/process/look.hpp"

#include "pakon/process/ice.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>

namespace pakon::process {

namespace {

constexpr int kLutSize = 0x4000;

const std::vector<int>& lut() {
    static const std::vector<int> table = [] {
        std::vector<int> t(kLutSize);
        t[0] = kLutSize - 1;
        for (int i = 1; i < kLutSize; ++i) {
            t[static_cast<std::size_t>(i)] =
                static_cast<int>(-3500.0 * std::log10(i / static_cast<double>(kLutSize - 1)));
        }
        return t;
    }();
    return table;
}

int code_of(double t) {
    const double v = kLookOpenGate * t;
    if (!(v > 0)) {
        return 0;
    }
    return v >= kLutSize - 1 ? kLutSize - 1 : static_cast<int>(v);
}

std::string join(const std::string& dir, const std::string& name) {
    if (dir.empty()) {
        return name;
    }
    const char last = dir.back();
    return dir + (last == '/' || last == '\\' ? "" : "/") + name;
}

} // namespace

Result<LookGeometry> look_geometry(std::string_view mode) {
    if (mode.starts_with("base16")) {
        return LookGeometry{2000, 3000, 8, 8, 3000, 2000};
    }
    if (mode.starts_with("base8")) {
        return LookGeometry{1500, 2250, 6, 6, 2250, 1500};
    }
    if (mode.starts_with("base4")) {
        return LookGeometry{1000, 1500, 4, 4, 3000, 2000}; // upscaled 2x by PakonIMAu
    }
    return failure<LookGeometry>(ErrorKind::image_bad_input,
                                 std::format("no OEM look geometry for mode '{}'", mode));
}

int look_density_lut(int code) {
    return lut()[static_cast<std::size_t>(std::clamp(code, 0, kLutSize - 1))];
}

std::array<std::uint16_t, 3> look_pixel(const NegMatrix& m, double t_r, double t_g, double t_b) {
    const double r = look_density_lut(code_of(t_r));
    const double g = look_density_lut(code_of(t_g));
    const double b = look_density_lut(code_of(t_b));
    const std::array<double, 10> terms{r, g, b, r * r, g * g, b * b, r * g, r * b, g * b, 1.0};
    std::array<std::uint16_t, 3> out{};
    for (std::size_t c = 0; c < 3; ++c) {
        double v = 0;
        for (std::size_t k = 0; k < 10; ++k) {
            v += m[10 * c + k] * terms[k];
        }
        out[c] = v < 0 ? 0 : v > 65535 ? 65535 : static_cast<std::uint16_t>(v + 0.5);
    }
    return out;
}

Result<LookPlanes> look_frame(PakrawReader& raw, const ScanInfo& info, const NegMatrix& m,
                              const LookGeometry& g, std::size_t first) {
    const std::size_t width = g.width;
    const std::size_t height = g.height;
    const int shift = g.row_shift;
    const std::size_t channels = info.ir ? 4 : 3;
    if (raw.samples_per_row() != info.pixels * channels || info.pixels < width ||
        raw.rows() == 0) {
        return failure<LookPlanes>(ErrorKind::image_bad_input,
                                   std::format("the look needs >= {} film pixels per line, have {}",
                                               width, info.pixels));
    }
    for (std::size_t c = 0; c < 3; ++c) {
        if (info.dark[c].size() < width + info.reference_shift ||
            info.white[c].size() < width + info.reference_shift) {
            return failure<LookPlanes>(ErrorKind::image_bad_input,
                                       "sidecar lacks the R/G/B dark and white references");
        }
    }
    std::array<std::vector<double>, 3> dark, span;
    for (std::size_t c = 0; c < 3; ++c) {
        for (std::size_t p = 0; p < width; ++p) {
            const double d = info.dark[c][p + info.reference_shift];
            dark[c].push_back(d);
            span[c].push_back(std::max(1.0, info.white[c][p + info.reference_shift] - d));
        }
    }
    const auto last_row = static_cast<long long>(raw.rows()) - 1;
    // The frame and `shift` lines either side as one block, so infrared
    // dust removal (ice.hpp) sees the frame whole before it is read.
    const long long lo = std::clamp(static_cast<long long>(first) - shift, 0LL, last_row);
    const long long hi = std::clamp(static_cast<long long>(first + height) + shift, lo, last_row) + 1;
    const std::size_t spr = raw.samples_per_row();
    std::vector<std::uint16_t> block(static_cast<std::size_t>(hi - lo) * spr);
    for (long long r = lo; r < hi; ++r) {
        if (auto rr = raw.read_row(static_cast<std::uint64_t>(r),
                                   std::span<std::uint16_t>(block).subspan(static_cast<std::size_t>(r - lo) * spr, spr));
            !rr) {
            return rr.error();
        }
    }
    ice_correct(block, static_cast<std::size_t>(hi - lo), info, shift, info.ice);
    const auto row = [&](long long r) {
        r = std::clamp(r, lo, hi - 1);
        return block.data() + static_cast<std::size_t>(r - lo) * spr;
    };
    LookPlanes out;
    out.width = width;
    out.height = height;
    for (auto& p : out.plane) {
        p.assign(width * height, 0);
    }
    for (std::size_t y = 0; y < height; ++y) {
        const auto line = static_cast<long long>(first + y);
        const std::uint16_t* R = row(line - shift);
        const std::uint16_t* G = row(line);
        const std::uint16_t* B = row(line + shift);
        for (std::size_t x = 0; x < width; ++x) {
            const auto p = static_cast<std::size_t>(
                std::clamp(static_cast<long long>(x) + kLookPixelOffset, 0LL,
                           static_cast<long long>(width) - 1));
            const auto v = look_pixel(m, (R[3 * p] - dark[0][p]) / span[0][p],
                                      (G[3 * p + 1] - dark[1][p]) / span[1][p],
                                      (B[3 * p + 2] - dark[2][p]) / span[2][p]);
            for (std::size_t c = 0; c < 3; ++c) {
                out.plane[c][y * width + x] = v[c];
            }
        }
    }
    return out;
}

std::vector<std::uint16_t> look_thumbnail(const LookPlanes& p, std::size_t div) {
    std::vector<std::uint16_t> t(3 * kThumbWidth * kThumbHeight);
    if (div == 0 || p.width < kThumbWidth * div || p.height < kThumbHeight * div) {
        return t;
    }
    const std::size_t n = div * div;
    for (std::size_t c = 0; c < 3; ++c) {
        const auto& P = p.plane[c];
        for (std::size_t y = 0; y < kThumbHeight; ++y) {
            for (std::size_t x = 0; x < kThumbWidth; ++x) {
                std::size_t s = 0;
                for (std::size_t dy = 0; dy < div; ++dy) {
                    const std::size_t b = (div * y + dy) * p.width + div * x;
                    for (std::size_t dx = 0; dx < div; ++dx) {
                        s += P[b + dx];
                    }
                }
                t[c * kThumbWidth * kThumbHeight + (kThumbHeight - 1 - y) * kThumbWidth +
                  (kThumbWidth - 1 - x)] = static_cast<std::uint16_t>((s + n / 2) / n);
            }
        }
    }
    return t;
}

std::array<std::uint16_t, 3> look_dmin(const std::vector<std::vector<std::uint16_t>>& thumbs) {
    std::array<std::uint16_t, 3> out{};
    const std::size_t n = kThumbWidth * kThumbHeight;
    for (std::size_t c = 0; c < 3; ++c) {
        std::vector<std::uint16_t> v;
        for (const auto& t : thumbs) {
            for (std::size_t i = c * n; i < (c + 1) * n && i < t.size(); i += 7) {
                v.push_back(t[i]);
            }
        }
        if (v.empty()) {
            continue;
        }
        std::sort(v.begin(), v.end());
        out[c] = v[v.size() / 100];
    }
    return out;
}

std::string look_script(std::size_t frames, const std::string& work, const std::string& capture,
                        const LookGeometry& g, const std::array<std::uint16_t, 3>& dmin,
                        const FilmDx& dx, std::span<const std::size_t> only, bool bw, bool raw) {
    const auto q = [](const std::string& s) { return "\"" + s + "\""; };
    const auto cap = [&](const std::string& f) { return q(join(capture, f)); };
    const auto hex = [](int v) { return std::format("{:x}", static_cast<std::uint32_t>(v)); };
    const std::size_t big = std::max(g.width * g.height, g.out_width * g.out_height) * 3 * 2;
    const std::size_t out = g.out_width * g.out_height * 3;
    std::string s = "# generated by pakon-cli frames --pakon-look\n";
    s += "buf roll blob " + cap("roll.bin") + " 4096\n";
    s += std::format("poke roll 0x18 {0:x}\npoke roll 0x1c {0:x}\n", g.div);
    if (bw) {
        s += "poke roll 0x20 4\n"; // iFilmColor BnW_NORMAL
    }
    s += "call PIAnselStartNewRoll @roll 1\n";
    for (const int k : {9, 11, 14, 15}) {
        s += std::format("buf s43a{} blob {}\n", k, cap(std::format("s43/a{}.bin", k)));
    }
    for (std::size_t n = 1; n <= frames; ++n) {
        s += std::format("buf scene{0} blob {1} 4096\n"
                         "buf img{0} blob {2}\n"
                         "buf desc{0} blob {3}\n"
                         "pokeref desc{0} 0 img{0}\n"
                         "poke desc{0} 0xc {4:x}\n"
                         "poke desc{0} 0x54 {5:x}\n"
                         "poke desc{0} 0x58 {6:x}\n"
                         "poke desc{0} 0x5c {7:x}\n"
                         "poke desc{0} 0x60 {8}\n"
                         "poke desc{0} 0x64 {9}\n"
                         "call PIAnselAddScene @roll @scene{0} @desc{0}\n",
                         n, cap("scene_4.bin"), q(join(work, std::format("thumb{:02}.raw", n))),
                         cap("adddesc_4.bin"), kThumbHeight, dmin[0], dmin[1], dmin[2],
                         hex(dx.product), hex(dx.specifier));
    }
    s += "call PIAnselEndRoll @roll 1\ncall PIAnselAnalyzeRoll @roll\n";
    for (std::size_t n = 1; n <= frames; ++n) {
        if (!only.empty() && std::find(only.begin(), only.end(), n) == only.end()) {
            continue;
        }
        s += std::format(
            "buf in{0} blob {1}\n"
            "buf big{0} zero {2}\n"
            "buf bal{0} blob {3}\n"
            "pokeref bal{0} 0 in{0}\n"
            "pokeref bal{0} 4 big{0}\n"
            "poke bal{0} 8 {4:x}\n"
            "poke bal{0} 0xc {5:x}\n"
            "{11}"
            "call PIAnselColorSceneBalancePlanar @roll @scene{0} @bal{0}\n"
            "call PIScaleAndRotatePlanar @big{0} 1 {4:x} {5:x} {6:x} {7:x} 3 10e 2 @s43a9 2 "
            "@s43a11 @scene{0} @scene{0} @s43a14 @s43a15\n"
            "buf out{0} zero {8}\n"
            "buf ca{0} blob {9}\n"
            "pokeref ca{0} 0 big{0}\n"
            "pokeref ca{0} 4 out{0}\n"
            "poke ca{0} 8 {6:x}\n"
            "poke ca{0} 0xc {7:x}\n"
            "{12}"
            "call PIColorAdjustPlanar @ca{0}\n"
            "buf path{0} wstr {10}\n"
            "call PISaveFilePlanar_8 @out{0} {6:x} {7:x} 3 @path{0} 0 5a 12c 0\n"
            "{13}"
            "buf in{0} zero 4\nbuf big{0} zero 4\nbuf out{0} zero 4\n",
            n, q(join(work, std::format("dens{:02}.raw", n))), big, cap("baldesc_42.bin"), g.width,
            g.height, g.out_width, g.out_height, out, cap("coloradjust44_a0.bin"),
            q(join(work, std::format("look{:02}.jpg", n))),
            bw ? std::format("poke bal{} 0x48 4\n", n) : std::string(),
            bw ? std::format("poke ca{} 0x4c 1\n", n) : std::string(),
            raw ? std::format("save out{} {} {}\n", n, out, q(join(work, std::format("look{:02}.rgb", n))))
                : std::string());
    }
    return s;
}

} // namespace pakon::process
