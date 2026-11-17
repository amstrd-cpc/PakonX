#include "frames_cli.hpp"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <thread>

#include "pakon/image/bmp.hpp"
#include "pakon/image/jpeg.hpp"
#include "pakon/image/tiff.hpp"
#include "pakon/process/frames.hpp"
#include "pakon/process/ice.hpp"
#include "pakon/process/filmid.hpp"
#include "pakon/process/dx.hpp"
#include "pakon/process/json.hpp"
#include "pakon/process/look.hpp"
#include "pakon/process/pakraw.hpp"
#include "pakon/process/positive.hpp"

namespace pakon::cli {

namespace {

namespace fs = std::filesystem;

std::string dx_json(const process::DxDecode& d) {
    std::string j = std::format(
        "\"dx\":{{\"records\":{},\"codes\":{},\"rejected\":{},\"product\":{},"
        "\"specifier\":{},\"votes\":{},\"frames\":[",
        d.records, d.codes, d.rejected, d.film ? d.film->product : -1,
        d.film ? d.film->specifier : -1, d.votes);
    for (std::size_t i = 0; i < d.frames.size(); ++i) {
        const auto& f = d.frames[i];
        j += std::format("{}{{\"code\":{},\"read_row\":{},\"mark_row\":{}}}", i ? "," : "",
                         f.code, f.read_row,
                         f.mark_row ? std::to_string(*f.mark_row) : std::string("null"));
    }
    return j + "]}";
}

std::string frames_json(const process::FrameSplit& s, const std::string& raw,
                        const process::DxDecode& dx, std::span<const std::optional<int>> numbers) {
    std::string j = std::format(
        "{{\"schema\":\"pakon-frames/1\",\"source\":\"{}\",\"pitch_seed\":{:.1f},\"pitch\":{:.1f},"
        "\"base\":{:.4f},\"gaps_on_grid\":{},\"bin\":{},\"threshold\":{},\"format\":\"{}\",\n{},\n\"gaps\":[",
        fs::path(raw).filename().string(), s.pitch_seed, s.pitch, s.base, s.gaps_on_grid, s.bin,
        s.threshold, s.half ? "half" : s.pano ? "pano" : "full", dx_json(dx));
    for (std::size_t i = 0; i < s.gaps.size(); ++i) {
        j += std::format("{}[{},{}]", i ? "," : "", s.gaps[i].first, s.gaps[i].last);
    }
    j += "],\n\"frames\":[\n";
    std::size_t picture = 0;
    for (std::size_t i = 0; i < s.frames.size(); ++i) {
        const auto& f = s.frames[i];
        const bool pic = f.kind == process::FrameKind::picture;
        picture += pic ? 1 : 0;
        j += std::format("{}  {{\"index\":{},\"kind\":\"{}\",\"first_line\":{},\"end_line\":{},"
                         "\"placement\":\"{}\",\"picture\":{},\"dx_frame\":{}}}",
                         i ? ",\n" : "", i + 1, process::to_string(f.kind), f.first, f.last,
                         process::to_string(f.placement),
                         pic ? std::to_string(picture) : std::string("null"),
                         i < numbers.size() && numbers[i] ? std::format("\"{}A\"", *numbers[i])
                                                          : std::string("null"));
    }
    return j + "\n]}\n";
}

// One frame, rows [first, last): RGB transposed so width = travel and
// oriented for viewing (process::picture_row / picture_col), plus the
// IR lane when present. Samples as scanned (linear negative).
VoidResult write_frame(process::PakrawReader& raw, const process::ScanInfo& info,
                       const process::Frame& f, process::Feed feed, const std::string& rgb_path,
                       const std::string& ir_path) {
    const std::size_t w = f.last - f.first;
    const std::size_t h = info.pixels;
    std::vector<std::uint16_t> rgb(w * h * 3);
    std::vector<std::uint16_t> ir(info.ir ? w * h : 0);
    std::vector<std::uint16_t> row(raw.samples_per_row());
    for (std::size_t r = 0; r < w; ++r) {
        if (auto rr = raw.read_row(f.first + r, row); !rr) {
            return rr;
        }
        const std::size_t x = process::picture_col(r, w, feed);
        for (std::size_t p = 0; p < h; ++p) {
            const std::size_t y = process::picture_row(p, h, feed);
            for (std::size_t c = 0; c < 3; ++c) {
                rgb[(y * w + x) * 3 + c] = row[3 * p + c];
            }
            if (info.ir) {
                ir[y * w + x] = row[3 * h + p];
            }
        }
    }
    if (auto t = image::write_tiff16(rgb_path, static_cast<std::uint32_t>(w),
                                     static_cast<std::uint32_t>(h), 3, rgb);
        !t) {
        return t;
    }
    if (info.ir) {
        return image::write_tiff16(ir_path, static_cast<std::uint32_t>(w),
                                   static_cast<std::uint32_t>(h), 1, ir);
    }
    return {};
}

struct LookOptions {
    std::string oem;      // OEM install copy: PakonIMAu.dll + dependencies
    std::string capture;  // TLB's captured descriptors (settings.bin, ...)
    std::string host = "oem_host.exe";
    std::string eeprom;   // EEPROM dump, for sidecars without unit.neg_matrix
    process::FilmDx dx;   // the film's DX product / specifier, -1/-1 = not read
    bool dx_given{false}; // --film-dx (else the scan's decoded DX code)
    std::size_t hosts{0}; // --look-hosts: oem_host processes at once, 0 = by the cores
    bool bw{false};       // --bw: black-and-white film (TLB's BnW_NORMAL)
    bool raw{false};      // --look-raw: keep each picture's 8-bit planar RGB too
};

std::vector<unsigned char> read_bytes(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

bool write_bytes(const fs::path& path, const void* data, std::size_t size) {
    std::ofstream f(path, std::ios::binary);
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(f);
}

// Runs the 32-bit helper and waits; its output goes to this console.
// Runs the oem_host command lines at once and waits for all; the first
// non-zero exit code (or -1 when one cannot start), else 0.
int run_hosts(const std::vector<std::vector<fs::path>>& argvs, std::FILE* err) {
#ifdef _WIN32
    std::vector<PROCESS_INFORMATION> procs;
    int rc = 0;
    for (const auto& argv : argvs) {
        std::wstring cmd;
        for (const auto& a : argv) {
            cmd += (cmd.empty() ? L"\"" : L" \"") + a.wstring() + L"\"";
        }
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                            &pi)) {
            std::fprintf(err, "error: frames: cannot start %s (error %lu); pass --oem-host\n",
                         argv.front().string().c_str(), GetLastError());
            rc = -1;
            break;
        }
        procs.push_back(pi);
    }
    for (auto& pi : procs) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        if (rc == 0 && code != 0) {
            rc = static_cast<int>(code);
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return rc;
#else
    (void)argvs;
    std::fprintf(err, "error: frames: --pakon-look needs Windows (the OEM DLL is 32-bit Win32)\n");
    return -1;
#endif
}

// Stage 2: TLB's density per picture, the Ansel thumbnails and an
// oem_host script; PakonIMAu.dll then balances, scales, colour-adjusts and
// saves <stem>.look/lookNN.jpg exactly as the OEM software does.
int run_look(const std::string& raw_path, const process::ScanInfo& info,
             const process::FrameSplit& split, const std::string& stem, const LookOptions& o,
             std::FILE* out, std::FILE* err) {
    if (info.reversal) {
        std::fprintf(err, "error: frames: --pakon-look develops negatives only; this roll was "
                          "scanned as an E-6 slide\n");
        return 2;
    }
    if (split.half || split.pano) {
        std::fprintf(err, "error: frames: --pakon-look frames 24x36 only; this roll is set to "
                          "%s frames\n", split.half ? "half" : "panoramic");
        return 2;
    }
    const auto geo = process::look_geometry(info.mode);
    if (!geo) {
        std::fprintf(err, "error: frames: %s\n", geo.error().message.c_str());
        return 1;
    }
    if (o.oem.empty() || o.capture.empty()) {
        std::fprintf(err, "error: frames: --pakon-look needs --oem DIR and --capture DIR\n");
        return 2;
    }
    process::NegMatrix m{};
    if (info.neg_matrix) {
        m = *info.neg_matrix;
    } else if (!o.eeprom.empty()) {
        const auto e = read_bytes(o.eeprom);
        if (e.size() < 0x26 + 120) {
            std::fprintf(err, "error: frames: %s is not an EEPROM dump\n", o.eeprom.c_str());
            return 1;
        }
        for (std::size_t k = 0; k < 30; ++k) {
            float f = 0;
            std::memcpy(&f, e.data() + 0x26 + 4 * k, 4);
            m[k] = f;
        }
    } else {
        std::fprintf(err, "error: frames: the sidecar has no unit.neg_matrix (scan predates it); "
                          "pass --eeprom <dump from pakon-cli eeprom --dump>\n");
        return 1;
    }
    auto settings = read_bytes(fs::path(o.capture) / "settings.bin");
    if (settings.size() < 0x74) {
        std::fprintf(err, "error: frames: no settings.bin in %s\n", o.capture.c_str());
        return 1;
    }
    // PIBegin settings +0x60/+0x68/+0x70: the NegMatrix constants as
    // integers (173/447/623 on serial 17373) [VM proxy run3, 2026-10-14].
    for (std::size_t c = 0; c < 3; ++c) {
        const auto v = static_cast<std::int32_t>(m[10 * c + 9]);
        std::memcpy(settings.data() + 0x60 + 8 * c, &v, 4);
    }
    const fs::path work = fs::absolute(fs::path(stem + ".look"));
    std::error_code ec;
    fs::create_directories(work, ec);
    if (!write_bytes(work / "settings.bin", settings.data(), settings.size())) {
        std::fprintf(err, "error: frames: cannot write in %s\n", work.string().c_str());
        return 1;
    }
    // TLB's densities per picture, on all cores: each worker reads the scan
    // through its own reader and takes every T-th picture.
    std::vector<const process::Frame*> pics;
    for (const auto& f : split.frames) {
        if (f.kind == process::FrameKind::picture) {
            pics.push_back(&f);
        }
    }
    std::vector<std::vector<std::uint16_t>> thumbs(pics.size());
    std::vector<std::size_t> firsts(pics.size());
    std::vector<std::string> errors(pics.size());
    const std::size_t workers =
        std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, std::max<std::size_t>(1, std::min<std::size_t>(pics.size(), 8)));
    const auto work_on = [&](std::size_t w) {
        auto reader = process::PakrawReader::open(raw_path);
        if (!reader) {
            errors[w] = reader.error().message;
            return;
        }
        const auto rows = static_cast<std::size_t>((*reader)->rows());
        for (std::size_t k = w; k < pics.size(); k += workers) {
            const auto& f = *pics[k];
            const std::size_t mid = (f.first + f.last) / 2;
            const std::size_t first =
                std::min(mid > geo->height / 2 ? mid - geo->height / 2 : 0,
                         rows > geo->height ? rows - geo->height : 0);
            auto planes = process::look_frame(**reader, info, m, *geo, first);
            if (!planes) {
                errors[k] = planes.error().message;
                return;
            }
            std::vector<std::uint16_t> dens;
            for (const auto& pl : planes->plane) {
                dens.insert(dens.end(), pl.begin(), pl.end());
            }
            thumbs[k] = process::look_thumbnail(*planes, geo->div);
            const std::size_t n = k + 1;
            if (!write_bytes(work / std::format("dens{:02}.raw", n), dens.data(), 2 * dens.size()) ||
                !write_bytes(work / std::format("thumb{:02}.raw", n), thumbs[k].data(),
                             2 * thumbs[k].size())) {
                errors[k] = "cannot write in " + work.string();
                return;
            }
            firsts[k] = first;
        }
    };
    {
        std::vector<std::jthread> pool;
        for (std::size_t w = 1; w < workers; ++w) {
            pool.emplace_back(work_on, w);
        }
        work_on(0);
    }
    for (const auto& e : errors) {
        if (!e.empty()) {
            std::fprintf(err, "error: frames: look: %s\n", e.c_str());
            return 1;
        }
    }
    for (std::size_t k = 0; k < pics.size(); ++k) {
        std::fprintf(out, "look: frame %2zu lines %zu..%zu\n", k + 1, firsts[k], firsts[k] + geo->height);
    }
    if (thumbs.empty()) {
        std::fprintf(err, "error: frames: no pictures for the look\n");
        return 1;
    }
    const auto dmin = process::look_dmin(thumbs);
    // Several hosts at once, each with the whole roll's Ansel analysis (the
    // same balance) and every H-th picture; each is a 32-bit process of its
    // own (~200 MB at most).
    const std::size_t hosts = std::min(
        thumbs.size(), o.hosts ? o.hosts : std::clamp<std::size_t>(std::thread::hardware_concurrency() / 2, 1, 6));
    std::vector<std::vector<fs::path>> runs;
    for (std::size_t h = 0; h < hosts; ++h) {
        std::vector<std::size_t> share;
        for (std::size_t n = h + 1; n <= thumbs.size(); n += hosts) {
            share.push_back(n);
        }
        const auto script = process::look_script(thumbs.size(), work.string(),
                                                 fs::absolute(o.capture).string(), *geo, dmin, o.dx,
                                                 hosts > 1 ? std::span<const std::size_t>(share)
                                                           : std::span<const std::size_t>(),
                                                 o.bw, o.raw);
        const auto name = hosts > 1 ? std::format("look{}.txt", h + 1) : std::string("look.txt");
        if (!write_bytes(work / name, script.data(), script.size())) {
            std::fprintf(err, "error: frames: cannot write in %s\n", work.string().c_str());
            return 1;
        }
        runs.push_back({fs::path(o.host), fs::path("script"), fs::absolute(o.oem), work / "settings.bin",
                        work / name});
    }
    std::fprintf(out, "look: %zux%zu frames -> %zux%zu JPEG; dmin %u/%u/%u; DX %d/%d%s; running %s\n",
                 geo->width, geo->height, geo->out_width, geo->out_height, dmin[0], dmin[1],
                 dmin[2], o.dx.product, o.dx.specifier,
                 o.dx.product < 0 ? " (unknown: pass --film-dx)" : o.dx_given ? " (--film-dx)" : " (scan)",
                 o.host.c_str());
    std::fprintf(out, "look: %zu host%s at once\n", hosts, hosts == 1 ? "" : "s");
    std::fflush(out);
    const int rc = run_hosts(runs, err);
    if (rc != 0) {
        std::fprintf(err, "error: frames: oem_host exited with %d (inputs kept in %s)\n", rc,
                     work.string().c_str());
        return 1;
    }
    for (std::size_t n = 1; n <= thumbs.size(); ++n) {
        fs::remove(work / std::format("dens{:02}.raw", n), ec);
        fs::remove(work / std::format("thumb{:02}.raw", n), ec);
        std::fprintf(out, "written: %s\n", (work / std::format("look{:02}.jpg", n)).string().c_str());
    }
    return 0;
}

// 8-bit screen previews of the stage-1 positive per picture:
// <stem>.frameNN.preview.bmp (long side <= 1600) and .thumb.bmp (<= 240).
int write_previews(process::PakrawReader& raw, const process::ScanInfo& info,
                   const process::FrameSplit& split, process::Feed feed, const std::string& stem,
                   bool bw, std::FILE* out, std::FILE* err) {
    auto pp = process::measure_positive(raw, info, split);
    if (!pp) {
        std::fprintf(err, "error: frames: preview: %s\n", pp.error().message.c_str());
        return 1;
    }
    std::size_t n = 0;
    for (const auto& f : split.frames) {
        if (f.kind != process::FrameKind::picture) {
            continue;
        }
        ++n;
        auto px = process::develop_frame(raw, info, *pp, f, feed);
        if (!px) {
            std::fprintf(err, "error: frames: preview: %s\n", px.error().message.c_str());
            return 1;
        }
        if (bw) {
            process::to_grey(*px);
        }
        const std::size_t w = f.last - f.first;
        for (const auto& [suffix, side] : {std::pair{"preview", std::size_t{1600}},
                                           std::pair{"thumb", std::size_t{240}}}) {
            const auto p = process::preview8(*px, w, info.pixels, side);
            const auto path = std::format("{}.frame{:02}.{}.bmp", stem, n, suffix);
            if (auto wr = image::write_bmp24(path, static_cast<std::uint32_t>(p.width),
                                             static_cast<std::uint32_t>(p.height), p.rgb);
                !wr) {
                std::fprintf(err, "error: frames: %s\n", wr.error().message.c_str());
                return 1;
            }
        }
        std::fprintf(out, "written: %s.frame%02zu.preview.bmp (+ .thumb.bmp)\n", stem.c_str(), n);
    }
    return 0;
}

// "1,3,5-7" -> {1, 3, 5, 6, 7}; empty on a malformed list.
std::vector<std::size_t> parse_pictures(const std::string& list) {
    std::vector<std::size_t> out;
    std::stringstream ss(list);
    std::string item;
    while (std::getline(ss, item, ',')) {
        unsigned long a = 0;
        unsigned long b = 0;
        char tail = 0;
        const int got = std::sscanf(item.c_str(), "%lu-%lu%c", &a, &b, &tail);
        if (got == 1) {
            b = a;
        } else if (got != 2) {
            return {};
        }
        if (a == 0 || b < a || b - a > 999) {
            return {};
        }
        for (unsigned long n = a; n <= b; ++n) {
            out.push_back(n);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

struct SaveOptions {
    bool jpeg{false};                  // else the 16-bit TIFF positive
    bool negative{false};              // --save negative: .neg.tiff, linear flat-fielded negative
    int quality{92};
    std::vector<std::size_t> pictures; // 1-based; empty = every picture
    bool dx_names{false};              // <stem>_19A.jpg (roll + film number) instead of .frameNN
    std::map<std::size_t, process::Adjust> adjust; // --adjust, by picture number
    // --name BASE [--start N] [--digits D]: <dir>/BASE<N + picture - 1>,
    // zero-padded to D digits, numbered by position on the roll.
    std::string name;
    std::size_t start{1};
    int digits{3};
    bool bw{false}; // --bw: grey pictures (black-and-white film)
};

// A file name base the user typed: printable ASCII, no path, nothing
// Windows refuses (the number follows it: a trailing space or dot is fine).
bool valid_name_base(const std::string& s) {
    if (s.empty() || s.size() > 80 || s.front() == '-') {
        return false;
    }
    return std::none_of(s.begin(), s.end(), [](char c) {
        // ASCII only: the writers open files by narrow (ANSI) paths.
        const auto u = static_cast<unsigned char>(c);
        return u < 0x20 || u > 0x7E || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos;
    });
}

// Finished stage-1 positives of the chosen pictures:
// <stem>.frameNN.jpg (8-bit, rounded from 16) or .frameNN.pos.tiff.
int write_saved(process::PakrawReader& raw, const process::ScanInfo& info,
                const process::FrameSplit& split, process::Feed feed, const std::string& stem,
                std::span<const std::optional<int>> numbers, const SaveOptions& o,
                std::FILE* out, std::FILE* err) {
    std::vector<const process::Frame*> pics;
    std::vector<std::string> names; // file stem per picture
    for (std::size_t i = 0; i < split.frames.size(); ++i) {
        if (split.frames[i].kind != process::FrameKind::picture) {
            continue;
        }
        pics.push_back(&split.frames[i]);
        names.push_back(std::format("{}.frame{:02}", stem, pics.size()));
        if (o.dx_names) {
            // Roll + film number, 1111_19A; no DX number read: the picture's
            // place on the roll, 1111_05.
            names.back() = i < numbers.size() && numbers[i]
                               ? std::format("{}_{}A", stem, *numbers[i])
                               : std::format("{}_{:02}", stem, pics.size());
        }
        if (!o.name.empty()) {
            const auto dir = fs::path(stem).parent_path();
            const auto file = std::format("{}{:0{}}", o.name, o.start + pics.size() - 1, o.digits);
            names.back() = (dir / fs::path(file)).string();
        }
    }
    // A DX number read twice would overwrite a picture: those are named by
    // their place on the roll instead.
    for (std::size_t a = 0; a < names.size(); ++a) {
        for (std::size_t b = a + 1; b < names.size(); ++b) {
            if (names[a] == names[b]) {
                const auto place = [&](std::size_t n) {
                    return o.dx_names ? std::format("{}_{:02}", stem, n)
                                      : std::format("{}.frame{:02}", stem, n);
                };
                names[a] = place(a + 1);
                names[b] = place(b + 1);
            }
        }
    }
    for (const auto n : o.pictures) {
        if (n > pics.size()) {
            std::fprintf(err, "error: frames: no picture %zu (the roll has %zu)\n", n, pics.size());
            return 1;
        }
    }
    auto pp = process::measure_positive(raw, info, split);
    if (!pp) {
        std::fprintf(err, "error: frames: save: %s\n", pp.error().message.c_str());
        return 1;
    }
    pp->negative = o.negative;
    for (std::size_t n = 1; n <= pics.size(); ++n) {
        if (!o.pictures.empty() && !std::binary_search(o.pictures.begin(), o.pictures.end(), n)) {
            continue;
        }
        const auto& f = *pics[n - 1];
        const auto adj = o.adjust.find(n);
        auto a = adj == o.adjust.end() ? process::Adjust{} : adj->second;
        if (o.negative) {
            a = process::Adjust{.rotate = a.rotate}; // the converter does the look
        }
        auto px = process::develop_frame(raw, info, *pp, f, feed, a);
        if (!px) {
            std::fprintf(err, "error: frames: save: %s\n", px.error().message.c_str());
            return 1;
        }
        if (o.bw && !o.negative) {
            process::to_grey(*px);
        }
        auto w = static_cast<std::uint32_t>(f.last - f.first);
        auto h = static_cast<std::uint32_t>(info.pixels);
        if (a.rotate != 0) {
            *px = process::rotate_rgb(*px, w, h, a.rotate);
        }
        const auto path = std::format("{}.{}", names[n - 1], o.jpeg ? "jpg" : o.negative ? "neg.tiff" : "pos.tiff");
        VoidResult wr;
        if (o.jpeg) {
            std::vector<std::uint8_t> rgb8(px->size());
            std::transform(px->begin(), px->end(), rgb8.begin(), [](std::uint16_t v) {
                return static_cast<std::uint8_t>((v * 255u + 32767u) / 65535u);
            });
            wr = image::write_jpeg(path, w, h, rgb8, o.quality);
        } else {
            wr = image::write_tiff16(path, w, h, 3, *px);
        }
        if (!wr) {
            std::fprintf(err, "error: frames: %s\n", wr.error().message.c_str());
            return 1;
        }
        std::fprintf(out, "written: %s\n", path.c_str());
    }
    return 0;
}

} // namespace

void print_frames_usage(std::FILE* out) {
    std::fprintf(out,
                 "  frames <prefix> [--out-dir DIR] [--detect-only] [--positive] [--tail-first]\n"
                 "             split a scanned film window into its pictures (offline:\n"
                 "             reads <prefix>.film.pakraw + <prefix>.scan.json, opens no\n"
                 "             device); writes <prefix>.frameNN.tiff (linear 16-bit raw\n"
                 "             negative), .frameNN.ir.tiff in IR modes, <prefix>.frames.json;\n"
                 "             --positive also writes .frameNN.pos.tiff (16-bit sRGB positive:\n"
                 "             flat field, film base removed, one density stretch per roll);\n"
                 "             --tail-first when the film went in tail first (pictures upside\n"
                 "             down otherwise; the default is the normal head-first load)\n"
                 "  frames <prefix> --preview [--tail-first] [--out-dir DIR]\n"
                 "             8-bit screen previews of the stage-1 positive instead of the\n"
                 "             raw TIFFs: <prefix>.frameNN.preview.bmp (long side <= 1600)\n"
                 "             and .thumb.bmp (<= 240), for the PakonX light table\n"
                 "  frames <prefix> --save jpeg|tiff|negative [--pictures LIST] [--quality Q]\n"
                 "                  [--dx-names] [--adjust FILE] [--tail-first] [--out-dir DIR]\n"
                 "             finished stage-1 positives instead of the raw TIFFs:\n"
                 "             <prefix>.frameNN.jpg (8-bit, 4:4:4, quality 1..100, default\n"
                 "             92) or .frameNN.pos.tiff (16-bit sRGB), no .frames.json;\n"
                 "             negative: .frameNN.neg.tiff, the flat-fielded linear 16-bit\n"
                 "             negative (mask kept, rows aligned) for Negative Lab Pro;\n"
                 "             --pictures picks pictures by number, e.g. 1,4-6 (default: all);\n"
                 "             --dx-names names them roll + film number, <prefix>_19A.jpg\n"
                 "             (no DX number read: the place on the roll, <prefix>_05.jpg);\n"
                 "             --adjust: per-picture exposure, colour balance, contrast and\n"
                 "             rotation (pakon-adjust/1 JSON, written by PakonX as\n"
                 "             <prefix>.adjust.json);\n"
                 "             --name BASE [--start N] [--digits D]: names BASE001.jpg,\n"
                 "             BASE002.jpg, ... by position on the roll (start 1, 3 digits)\n"
                 "  --look-raw  with --pakon-look: also <prefix>.look/lookNN.rgb, the 8-bit\n"
                 "             planar RGB the OEM compresses (for TIFF or another quality)\n"
                 "  --bw        with any of the above: black-and-white film (grey pictures;\n"
                 "             the OEM look as TLB develops B&W, iFilmColor 4)\n"
                 "  --format F  with any of the above: frame format auto (default, from the\n"
                 "             gaps), full (24x36), half (18x24) or pano (gap to gap)\n"
                 "  --ice S     with any of the above: infrared dust removal, off (default),\n"
                 "             light, normal or strong (IR scans; process/ice.hpp)\n"
                 "  --shift N   with any of the above: move every frame window N film lines\n"
                 "             (negative: towards the film start), to correct the framing\n"
                 "  frames <prefix> --pakon-look --oem DIR --capture DIR [--oem-host EXE]\n"
                 "                  [--eeprom DUMP] [--film-dx P/S] [--out-dir DIR]\n"
                 "             the OEM Pakon look instead of the raw TIFFs (Windows):\n"
                 "             TLB's density per picture, then the OEM's own PakonIMAu.dll\n"
                 "             (Ansel roll balance, scale, colour adjust) via the 32-bit\n"
                 "             tools/oem_host writes <prefix>.look/lookNN.jpg. --oem: copy of\n"
                 "             the OEM install (PakonIMAu.dll + dependencies, Config,\n"
                 "             anselinstalldir); --capture: TLB's captured descriptors;\n"
                 "             --eeprom: for scans whose sidecar lacks unit.neg_matrix;\n"
                 "             --film-dx P/S: the film's DX product/specifier when the scan has\n"
                 "             none (scans from 2026-10-22 on record and decode it; the OEM\n"
                 "             logs it in Logs\\DxCode.txt); Ansel balances by film type\n");
}

int run_frames_command(const std::vector<std::string>& args, std::FILE* out, std::FILE* err) {
    std::string prefix;
    std::string out_dir;
    bool detect_only = false;
    bool positive = false;
    bool preview = false;
    process::Feed feed = process::Feed::head_first;
    bool pakon_look = false;
    process::IceStrength ice = process::IceStrength::off; // --ice
    process::FrameFormat frame_format = process::FrameFormat::auto_detect; // --format
    long long shift = 0;
    LookOptions look;
    bool save = false;
    SaveOptions save_opt;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--out-dir" && i + 1 < args.size()) {
            out_dir = args[++i];
        } else if (args[i] == "--detect-only") {
            detect_only = true;
        } else if (args[i] == "--positive") {
            positive = true;
        } else if (args[i] == "--preview") {
            preview = true;
        } else if (args[i] == "--tail-first") {
            feed = process::Feed::tail_first;
        } else if (args[i] == "--save" && i + 1 < args.size()) {
            save = true;
            const auto& fmt = args[++i];
            if (fmt == "jpeg" || fmt == "jpg") {
                save_opt.jpeg = true;
            } else if (fmt == "negative") {
                save_opt.negative = true;
            } else if (fmt != "tiff") {
                std::fprintf(err, "error: frames: --save wants jpeg, tiff or negative\n");
                return 2;
            }
        } else if (args[i] == "--pictures" && i + 1 < args.size()) {
            save_opt.pictures = parse_pictures(args[++i]);
            if (save_opt.pictures.empty()) {
                std::fprintf(err, "error: frames: --pictures wants numbers from 1, e.g. 1,4-6\n");
                return 2;
            }
        } else if (args[i] == "--adjust" && i + 1 < args.size()) {
            const std::string path = args[++i];
            std::ifstream af(path, std::ios::binary);
            std::stringstream at;
            at << af.rdbuf();
            auto aj = process::parse_json(at.str());
            auto adj = aj ? process::parse_adjustments(*aj)
                          : failure<std::map<std::size_t, process::Adjust>>(aj.error().kind, aj.error().message);
            if (!af || !adj) {
                std::fprintf(err, "error: frames: --adjust %s: %s\n", path.c_str(),
                             af ? adj.error().message.c_str() : "cannot read");
                return 2;
            }
            save_opt.adjust = std::move(*adj);
        } else if (args[i] == "--dx-names") {
            save_opt.dx_names = true;
        } else if (args[i] == "--shift" && i + 1 < args.size()) {
            char tail = 0;
            if (std::sscanf(args[++i].c_str(), "%lld%c", &shift, &tail) != 1 || shift < -20000 ||
                shift > 20000) {
                std::fprintf(err, "error: frames: --shift wants -20000..20000 lines\n");
                return 2;
            }
        } else if (args[i] == "--name" && i + 1 < args.size()) {
            save_opt.name = args[++i];
            if (!valid_name_base(save_opt.name)) {
                std::fprintf(err, "error: frames: --name '%s': up to 80 ASCII characters, no path, none of <>:\"/\\|?*\n",
                             save_opt.name.c_str());
                return 2;
            }
        } else if ((args[i] == "--start" || args[i] == "--digits") && i + 1 < args.size()) {
            const bool start = args[i] == "--start";
            long long v = 0;
            char tail = 0;
            if (std::sscanf(args[++i].c_str(), "%lld%c", &v, &tail) != 1 ||
                (start ? (v < 0 || v > 999999) : (v < 1 || v > 6))) {
                std::fprintf(err, "error: frames: %s\n",
                             start ? "--start wants 0..999999" : "--digits wants 1..6");
                return 2;
            }
            if (start) {
                save_opt.start = static_cast<std::size_t>(v);
            } else {
                save_opt.digits = static_cast<int>(v);
            }
        } else if (args[i] == "--quality" && i + 1 < args.size()) {
            char tail = 0;
            if (std::sscanf(args[++i].c_str(), "%d%c", &save_opt.quality, &tail) != 1 ||
                save_opt.quality < 1 || save_opt.quality > 100) {
                std::fprintf(err, "error: frames: --quality wants 1..100\n");
                return 2;
            }
        } else if (args[i] == "--pakon-look") {
            pakon_look = true;
        } else if (args[i] == "--oem" && i + 1 < args.size()) {
            look.oem = args[++i];
        } else if (args[i] == "--capture" && i + 1 < args.size()) {
            look.capture = args[++i];
        } else if (args[i] == "--look-raw") {
            look.raw = true;
        } else if (args[i] == "--bw") {
            look.bw = save_opt.bw = true;
        } else if (args[i] == "--format" && i + 1 < args.size()) {
            const std::string v = args[++i];
            if (v == "full") {
                frame_format = process::FrameFormat::full;
            } else if (v == "half") {
                frame_format = process::FrameFormat::half;
            } else if (v == "auto") {
                frame_format = process::FrameFormat::auto_detect;
            } else if (v == "pano") {
                frame_format = process::FrameFormat::pano;
            } else {
                std::fprintf(err, "error: frames: --format wants full, half, pano or auto\n");
                return 2;
            }
        } else if (args[i] == "--ice" && i + 1 < args.size()) {
            const auto k = process::parse_ice(args[++i]);
            if (!k) {
                std::fprintf(err, "error: frames: --ice wants off, light, normal or strong\n");
                return 2;
            }
            ice = *k;
        } else if (args[i] == "--look-hosts" && i + 1 < args.size()) {
            look.hosts = static_cast<std::size_t>(std::clamp(std::atoi(args[++i].c_str()), 1, 8));
        } else if (args[i] == "--oem-host" && i + 1 < args.size()) {
            look.host = args[++i];
        } else if (args[i] == "--eeprom" && i + 1 < args.size()) {
            look.eeprom = args[++i];
        } else if (args[i] == "--film-dx" && i + 1 < args.size()) {
            look.dx_given = true;
            if (std::sscanf(args[++i].c_str(), "%d/%d", &look.dx.product, &look.dx.specifier) != 2) {
                std::fprintf(err, "error: frames: --film-dx wants PRODUCT/SPECIFIER, e.g. 39/1\n");
                return 2;
            }
        } else if (prefix.empty() && !args[i].starts_with("--")) {
            prefix = args[i];
        } else {
            std::fprintf(err, "error: frames: unexpected argument '%s'\n", args[i].c_str());
            print_frames_usage(err);
            return 2;
        }
    }
    if (prefix.empty()) {
        print_frames_usage(err);
        return 2;
    }
    const std::string raw_path = prefix + ".film.pakraw";
    const std::string side_path = prefix + ".scan.json";

    std::ifstream sf(side_path, std::ios::binary);
    if (!sf) {
        std::fprintf(err, "error: frames: cannot read %s\n", side_path.c_str());
        return 1;
    }
    std::stringstream text;
    text << sf.rdbuf();
    auto json = process::parse_json(text.str());
    if (!json) {
        std::fprintf(err, "error: frames: %s: %s\n", side_path.c_str(), json.error().message.c_str());
        return 1;
    }
    auto info = process::scan_info_from_sidecar(*json);
    if (!info) {
        std::fprintf(err, "error: frames: %s: %s\n", side_path.c_str(), info.error().message.c_str());
        return 1;
    }
    info->frame_format = frame_format;
    if (ice != process::IceStrength::off) {
        if (!info->ir || info->dark_ir.empty()) {
            std::fprintf(out, "ice: off - this scan has no infrared plane (scan in a -ir mode)\n");
        } else if ((*json)["film"].string() == std::optional<std::string>("bw-silver")) {
            std::fprintf(out, "ice: off - silver film blocks infrared, so dust cannot be told "
                              "from the picture\n");
        } else {
            info->ice = ice;
            std::fprintf(out, "ice: %s\n", std::string(process::ice_name(ice)).c_str());
        }
    }
    auto raw = process::PakrawReader::open(raw_path);
    if (!raw) {
        std::fprintf(err, "error: frames: %s\n", raw.error().message.c_str());
        return 1;
    }
    auto profile = process::line_profile(**raw, *info);
    if (!profile) {
        std::fprintf(err, "error: frames: %s\n", profile.error().message.c_str());
        return 1;
    }
    auto split = process::find_frames(*profile, *info);
    const auto dx = process::decode_dx(*json);
    // Which film: the DX code's ISO speed and the kind measured on the scan.
    const auto guess = process::detect_film(**raw, *info, split);
    const auto iso = dx.film ? process::dx_iso(dx.film->product, dx.film->specifier) : std::nullopt;
    std::fprintf(out, "film detected: %s%s (%s)\n", guess.kind.empty() ? "not sure" : guess.kind.c_str(),
                 iso ? std::format(", ISO {}", *iso).c_str() : "", guess.why.c_str());
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    for (const auto& f : split.frames) {
        spans.emplace_back(f.first, f.last);
    }
    // DX numbers belong to the frames as found; the shift only moves the
    // windows. Half frames share a DX number two by two: they go without.
    const auto numbers = split.half || split.pano ? std::vector<std::optional<int>>(spans.size())
                                    : process::dx_frame_numbers(dx, spans, split.pitch);
    {
        std::fprintf(out, "format: %s frames (%zu gaps on the full-frame grid, %zu half way, gaps every %.0f lines vs a %.0f-line pitch)\n",
                     split.half ? "half" : split.pano ? "pano" : "full", split.gaps_on_grid, split.gaps_half_way,
                     split.gap_pitch, split.pitch_seed);
    }
    if (shift != 0) {
        process::shift_frames(split, shift, static_cast<std::size_t>((*raw)->rows()));
        std::fprintf(out, "shift: every frame window moved %lld lines\n", shift);
    }
    std::fprintf(out,
                 "frames: %s, %llu lines, film %zu..%s; pitch seed %.0f, fitted %.1f lines; "
                 "base %.4f; %zu gaps (%zu on the grid); OEM framing: bins of %zu lines, "
                 "threshold %d\n",
                 info->mode.c_str(), static_cast<unsigned long long>((*raw)->rows()),
                 info->film_start,
                 info->film_end ? std::to_string(*info->film_end).c_str() : "end",
                 split.pitch_seed, split.pitch, split.base, split.gaps.size(),
                 split.gaps_on_grid, split.bin, split.threshold);
    if (dx.records > 0) {
        std::string codes;
        for (const auto& c : dx.frames) {
            codes += std::format(" {}@{}", c.code,
                                 c.mark_row ? std::to_string(*c.mark_row) : std::string("?"));
        }
        std::fprintf(out, "DX: %zu records, %zu codes (%zu rejected)%s%s\n", dx.records, dx.codes,
                     dx.rejected,
                     dx.film ? std::format("; film product {} specifier {} ({} votes)",
                                           dx.film->product, dx.film->specifier, dx.votes)
                                   .c_str()
                             : "; no film code",
                     codes.empty() ? "" : ("; codes at film rows:" + codes).c_str());
    }
    if (!look.dx_given && dx.film) {
        look.dx = *dx.film;
    }
    std::size_t picture = 0;
    for (std::size_t i = 0; i < split.frames.size(); ++i) {
        const auto& f = split.frames[i];
        const bool pic = f.kind == process::FrameKind::picture;
        std::fprintf(out, "  %s lines %zu..%zu (%zu)  [%s]%s\n",
                     pic ? std::format("frame {:2}:", ++picture).c_str()
                         : std::format("{:<9}", std::string(process::to_string(f.kind)) + ":").c_str(),
                     f.first, f.last, f.last - f.first,
                     std::string(process::to_string(f.placement)).c_str(),
                     numbers[i] ? std::format("  DX {}A", *numbers[i]).c_str() : "");
    }

    fs::path base = fs::path(prefix);
    if (!out_dir.empty()) {
        std::error_code ec;
        fs::create_directories(out_dir, ec);
        base = fs::path(out_dir) / fs::path(prefix).filename();
    }
    const std::string stem = base.string();
    int code = 0;
    if (save) {
        // Finished pictures only: the destination is the user's folder.
        return write_saved(**raw, *info, split, feed, stem, numbers, save_opt, out, err);
    }
    if (std::FILE* f = std::fopen((stem + ".frames.json").c_str(), "w")) {
        auto j = frames_json(split, raw_path, dx, numbers);
        // The film as measured (filmid.hpp), for PakonX's Roll settings.
        j.insert(1, std::format("\"film_detected\":\"{}\",\"film_why\":\"{}\",\"iso\":{},", guess.kind, guess.why,
                                iso ? std::to_string(*iso) : "null"));
        std::fwrite(j.data(), 1, j.size(), f);
        std::fclose(f);
        std::fprintf(out, "written: %s.frames.json\n", stem.c_str());
    } else {
        std::fprintf(err, "error: frames: cannot write %s.frames.json\n", stem.c_str());
        code = 1;
    }
    if (detect_only) {
        return code;
    }
    if (pakon_look) {
        const int rc = run_look(raw_path, *info, split, stem, look, out, err);
        return rc ? rc : code;
    }
    if (preview) {
        const int rc = write_previews(**raw, *info, split, feed, stem, look.bw, out, err);
        return rc ? rc : code;
    }
    std::optional<process::PositiveParams> pos;
    if (positive) {
        auto pp = process::measure_positive(**raw, *info, split);
        if (!pp) {
            std::fprintf(err, "error: frames: positive: %s\n", pp.error().message.c_str());
            return 1;
        }
        pos = *pp;
        std::fprintf(out,
                     "positive: base R %.4f G %.4f B %.4f (%zu gap lines); density black..white "
                     "R %.3f..%.3f G %.3f..%.3f B %.3f..%.3f\n",
                     pos->base[0], pos->base[1], pos->base[2], pos->base_lines, pos->d_low[0],
                     pos->d_high[0], pos->d_low[1], pos->d_high[1], pos->d_low[2], pos->d_high[2]);
    }
    std::size_t n = 0;
    for (std::size_t i = 0; i < split.frames.size(); ++i) {
        if (split.frames[i].kind != process::FrameKind::picture) {
            continue; // blank or fogged film: listed in frames.json only
        }
        ++n;
        const auto rgb = std::format("{}.frame{:02}.tiff", stem, n);
        const auto ir = std::format("{}.frame{:02}.ir.tiff", stem, n);
        if (auto w = write_frame(**raw, *info, split.frames[i], feed, rgb, ir); !w) {
            std::fprintf(err, "error: frames: %s: %s\n", rgb.c_str(), w.error().message.c_str());
            code = 1;
            continue;
        }
        std::fprintf(out, "written: %s%s\n", rgb.c_str(), info->ir ? " (+ .ir.tiff)" : "");
        if (pos) {
            const auto pos_path = std::format("{}.frame{:02}.pos.tiff", stem, n);
            const auto& f = split.frames[i];
            auto px = process::develop_frame(**raw, *info, *pos, f, feed);
            if (!px) {
                std::fprintf(err, "error: frames: %s: %s\n", pos_path.c_str(),
                             px.error().message.c_str());
                code = 1;
                continue;
            }
            if (auto t = image::write_tiff16(pos_path, static_cast<std::uint32_t>(f.last - f.first),
                                             static_cast<std::uint32_t>(info->pixels), 3, *px);
                !t) {
                std::fprintf(err, "error: frames: %s: %s\n", pos_path.c_str(),
                             t.error().message.c_str());
                code = 1;
                continue;
            }
            std::fprintf(out, "written: %s\n", pos_path.c_str());
        }
    }
    return code;
}

} // namespace pakon::cli
