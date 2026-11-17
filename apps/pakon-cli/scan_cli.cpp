// The `scan` and `eeprom` commands - see scan_cli.hpp for the safety
// rules. Everything before run_live() is argument handling and plan
// rendering: no usb:: entry point is reached outside the single
// SessionOpener call (tests/cli/scan_cli_test.cpp pins that).

#include "scan_cli.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <variant>
#include <span>
#include <string_view>
#include <system_error>

#include "interrupt.hpp"
#include "pakon/eeprom/eeprom.hpp"
#include "pakon/image/raw_writer.hpp"
#include "pakon/image/tiff.hpp"
#include "pakon/process/json.hpp"
#include "pakon/scan/f235_plan.hpp"
#include "pakon/scan/runner.hpp"
#include "pakon/scan/sidecar.hpp"
#include "pakon/stream/image_stream.hpp"

namespace pakon::cli {
namespace {

enum class Mode { unset, dry_run, live };

struct Options {
    Mode mode{Mode::unset};
    std::string config;
    scan::ScanMode scan_mode{};
    bool first_light{false};
    std::size_t film_rows{0};
    std::size_t first_light_lines{256};
    std::string out_prefix;
    bool stop_on_stdin{false};
    bool clear_base{false}; // --bw: black-and-white film (clear base between frames)
    scan::FilmKind film{scan::FilmKind::c41}; // --film (scan/film.hpp)
    scan::FilmGate film_gate{scan::FilmGate::automatic}; // --film-gate
    std::string hot_start; // --hot-start: the earlier strip's .scan.json
};

// A hot start reuses an earlier strip's corrections only this soon after
// it (the lamp and CCD drift with time) [INFERRED].
constexpr auto kHotStartWindow = std::chrono::minutes(10);

struct ParseOutcome {
    int code{2};
    Options options;
    std::string error;
};

std::optional<std::size_t> parse_count(const std::string& text) {
    std::size_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || ec != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

ParseOutcome parse_options(const std::vector<std::string>& args) {
    ParseOutcome outcome;
    Options& o = outcome.options;
    bool have_film_rows = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        const auto value = [&]() -> const std::string* {
            return i + 1 < args.size() ? &args[++i] : nullptr;
        };
        if (arg == "--dry-run" || arg == "--live-scan") {
            const Mode m = arg == "--dry-run" ? Mode::dry_run : Mode::live;
            if (o.mode != Mode::unset && o.mode != m) {
                outcome.error = "--dry-run and --live-scan are mutually exclusive";
                return outcome;
            }
            o.mode = m;
        } else if (arg == "--bw") {
            o.clear_base = true;
        } else if (arg == "--first-light") {
            o.first_light = true;
        } else if (arg == "--stop-on-stdin") {
            o.stop_on_stdin = true;
        } else if (arg == "--film") {
            const auto* v = value();
            const auto k = v ? scan::parse_film(*v) : std::nullopt;
            if (!k) {
                outcome.error = "--film wants c41, c41-unmasked, e6, bw-silver or bw-c41";
                return outcome;
            }
            o.film = *k;
        } else if (arg == "--film-gate") {
            const auto* v = value();
            if (v && *v == "auto") {
                o.film_gate = scan::FilmGate::automatic;
            } else if (v && *v == "brightness") {
                o.film_gate = scan::FilmGate::brightness;
            } else if (v && *v == "off") {
                o.film_gate = scan::FilmGate::off;
            } else {
                outcome.error = "--film-gate wants auto, brightness or off";
                return outcome;
            }
        } else if (arg == "--hot-start") {
            const auto* v = value();
            if (!v || v->empty()) {
                outcome.error = "--hot-start wants the earlier strip's .scan.json";
                return outcome;
            }
            o.hot_start = *v;
        } else if (arg == "--config" || arg == "--out-prefix" || arg == "--film-rows" ||
                   arg == "--first-light-lines") {
            const auto* v = value();
            if (!v) {
                outcome.error = std::format("{} requires a value", arg);
                return outcome;
            }
            if (arg == "--config") {
                o.config = *v;
            } else if (arg == "--out-prefix") {
                o.out_prefix = *v;
            } else {
                const auto n = parse_count(*v);
                if (!n || *n == 0) {
                    outcome.error = std::format("{}: '{}' is not a positive integer", arg, *v);
                    return outcome;
                }
                if (arg == "--film-rows") {
                    o.film_rows = *n;
                    have_film_rows = true;
                } else {
                    o.first_light_lines = *n;
                }
            }
        } else {
            outcome.error = std::format("unknown scan argument '{}'", arg);
            return outcome;
        }
    }
    if (o.mode == Mode::unset) {
        outcome.error = "choose the mode explicitly: --dry-run (print the plan, no device) or "
                        "--live-scan (execute on the scanner)";
        return outcome;
    }
    if (o.config.empty()) {
        outcome.error = "--config is required: base4 | base4-ir | base8 | base8-ir | base16 | "
                        "base16-ir";
        return outcome;
    }
    const auto m = scan::parse_mode(o.config);
    if (!m) {
        outcome.error = std::format(
            "unknown --config '{}'; expected base4, base4-ir, base8, base8-ir, base16 or "
            "base16-ir",
            o.config);
        return outcome;
    }
    o.scan_mode = *m;
    // Silver holds back infrared like light: the IR plane would only copy
    // the picture, so dust cannot be told from it.
    if (o.film == scan::FilmKind::bw_silver && o.scan_mode.ir) {
        outcome.error = std::format("--film bw-silver: silver film blocks infrared; choose a "
                                    "mode without IR, not '{}'", o.config);
        return outcome;
    }
    if (o.first_light == have_film_rows) {
        outcome.error = "choose exactly one window: --first-light (calibration window only, "
                        "motor never engaged) or --film-rows <N> (film pass, N = hard row cap)";
        return outcome;
    }
    if (o.out_prefix.empty()) {
        outcome.error = "--out-prefix is required (the output paths are part of the plan)";
        return outcome;
    }
    outcome.code = 0;
    return outcome;
}

std::string hex_of(const ppb::Frame& frame) {
    auto bytes = frame.serialize();
    std::string out;
    if (bytes) {
        for (const auto b : *bytes) {
            out += std::format("{:02x}", b);
        }
    }
    return out;
}

void print_frames(std::FILE* out, std::span<const ppb::Frame> frames) {
    std::string line;
    for (const auto& f : frames) {
        const std::string h = hex_of(f);
        if (!line.empty() && line.size() + h.size() + 1 > 92) {
            std::fprintf(out, "    %s\n", line.c_str());
            line.clear();
        }
        line += line.empty() ? h : " " + h;
    }
    if (!line.empty()) {
        std::fprintf(out, "    %s\n", line.c_str());
    }
}

struct Outputs {
    std::string raw;
    std::string tiff;
    std::string text;
    std::string json;
};

Outputs outputs_for(const Options& o) {
    const std::string kind = o.first_light ? "white" : "film";
    return {o.out_prefix + "." + kind + ".pakraw", o.out_prefix + "." + kind + ".tiff",
            o.out_prefix + ".scan-stats.txt", o.out_prefix + ".scan.json"};
}

void print_preflight(const Options& o, std::FILE* out) {
    const auto& a = protocol::kF135Plus;
    const auto& mc = scan::constants(o.scan_mode.base);
    const auto outputs = outputs_for(o);
    std::fprintf(out, "pakon-cli scan - preflight (dry run)\n");
    std::fprintf(out, "No device is opened and no USB traffic is sent.\n\n");
    std::fprintf(out, "configuration:  %s (integration %u, DX word 0x%04x, %s)\n",
                 std::string(scan::mode_name(o.scan_mode)).c_str(),
                 scan::integration(o.scan_mode), scan::dx_word(o.scan_mode),
                 o.scan_mode.ir ? "IR lane on" : "IR off");
    std::fprintf(out, "window:         %s\n",
                 o.first_light
                     ? std::format("first light - Corrections + {} white lines, lamp on, motor "
                                   "never engaged",
                                   o.first_light_lines)
                           .c_str()
                     : std::format("film pass - ends on film end, no film within 45 s, or {} "
                                   "rows (hard cap)",
                                   o.film_rows)
                           .c_str());
    if (!o.first_light) {
        std::fprintf(out, "film:           %s (%s on-times%s)\n",
                     std::string(scan::film_name(o.film)).c_str(),
                     scan::film_boost(o.film) ? "boosted orange-mask" : "open-gate",
                     o.clear_base || scan::clear_base(o.film) ? ", clear-base film end" : "");
        std::fprintf(out, "film gate:      %s\n",
                     o.film_gate == scan::FilmGate::off ? "off - no film end, the row cap ends the pass"
                     : o.film_gate == scan::FilmGate::brightness ? "brightness - the light detector alone"
                                                                 : "auto - light detector, DX sensor confirms");
        if (!o.hot_start.empty()) {
            std::fprintf(out, "hot start:      %s (same unit and mode, <= %d min old; one white "
                              "check, else full Corrections)\n",
                         o.hot_start.c_str(), static_cast<int>(kHotStartWindow.count()));
        }
    }
    std::fprintf(out, "session:        one warm connection - usb::open_first(cold_ok=false) -> "
                      "Scanner::connect\n");
    std::fprintf(out, "                -> identify (incl. PICM 0x97=01, 0x03=01 page select); no "
                      "cold boot, no\n                firmware reload, no second connection\n");
    std::fprintf(out, "model gate:     F-135+ only (PICL 0x%02x / PICM 0x%02x)\n", a.light, a.motor);
    std::fprintf(out, "EEPROM:         read-only: 0xA4 wValue 0x00A5 + 0xA9, wIndex 0x1234 "
                      "(no 0xA2, no write-select)\n");
    std::fprintf(out, "                -> Offset, MotorSpeed%s, MotorAdjust for Base %d\n",
                 o.scan_mode.ir ? " IR" : "",
                 o.scan_mode.base == scan::Base::b4 ? 4 : o.scan_mode.base == scan::Base::b8 ? 8 : 16);
    std::fprintf(out, "endpoints:      commands OUT 0x01 / IN 0x81; image IN 0x86 with %zu queued "
                      "reads of %zu B\n                into a %zu MiB ring (overflow = error)\n",
                 stream::kQueuedReads, stream::kTransferBytes, stream::kRingBytes >> 20);
    std::fprintf(out, "geometry:       calibration pixels %u..Offset+%u, film Offset..Offset+%u%s\n",
                 mc.cal_start, mc.width, mc.width, mc.resample ? " (3/4 resample)" : "");
    std::fprintf(out, "outputs:        %s\n                %s (preview)\n                %s\n"
                      "                %s (settings, calibration references)\n",
                 outputs.raw.c_str(), outputs.tiff.c_str(), outputs.text.c_str(),
                 outputs.json.c_str());
    const auto init = scan::ScanRunner::init_frames(a);
    std::fprintf(out, "\nphases (exact frames where fixed):\n");
    std::fprintf(out, "  1 init block (%zu frames, byte-identical to base4.jsonl 0.951-1.067):\n",
                 init.size());
    print_frames(out, init);
    std::fprintf(out, "  2 lamp warm-up: poll 030110 every 200 ms; on an event read 0103400102 / "
                      "0103440102,\n    ack 0x06 [00][status]; until lamp status 0x83 bit1 "
                      "(stable); limit 300 s\n");
    std::fprintf(out, "  3 Corrections (data-dependent, bounded): stream started, calibration "
                      "geometry,\n    acquire bit on + DX start; dark servo <= 8 rounds; LED "
                      "current servo; on-time servo;\n    every measurement = 32 averaged lines "
                      "after a FIFO reset; LED currents clamped\n    to the board ceilings\n");
    if (o.first_light) {
        std::fprintf(out, "  4 %zu white lines, acquire off\n", o.first_light_lines);
    } else {
        std::fprintf(out, "  4 film: geometry at Offset, lamp on (film on-times), motor rate from "
                          "EEPROM, go,\n    acquire + DX start; lines consumed while the window "
                          "streams; the HOST ends it\n    (acquire off) - never by waiting for "
                          "the device to go quiet\n");
    }
    std::fprintf(out, "  5 teardown (exactly once, every path incl. Ctrl+C):\n");
    std::vector<ppb::Frame> td;
    const auto idle = scan::control_word(scan::film_geometry(o.scan_mode, 30), false);
    for (const auto& f : scan::teardown_frames(a, idle)) {
        td.push_back(f.frame);
    }
    print_frames(out, td);
    std::fprintf(out, "    (the first frame's control word depends on the mode; shown for this "
                      "mode)\n");
    std::fprintf(out, "Ctrl+C:         observed at bounded points; the teardown always runs\n");
    std::fprintf(out, "dry run only - nothing was sent; --live-scan executes this plan.\n");
}

// RGB preview from the written .pakraw, transposed so that width =
// travel (lines) and height = CCD pixels, the way the OEM export orients
// it. Linear, unprocessed. Read row by row and decimated so that neither
// side exceeds kPreviewMax: a full roll does not have to fit in memory.
constexpr std::size_t kPreviewMax = 8000;

VoidResult write_preview(const std::string& tiff_path, const std::string& raw_path,
                         std::size_t pixels) {
    std::ifstream in(raw_path, std::ios::binary);
    std::array<std::uint8_t, image::kStreamRawHeaderBytes> h{};
    if (!in.read(reinterpret_cast<char*>(h.data()), static_cast<std::streamsize>(h.size()))) {
        return void_failure(ErrorKind::io_failure, std::format("cannot read {}", raw_path));
    }
    const auto u32 = [&](std::size_t o) {
        return static_cast<std::size_t>(h[o]) | (static_cast<std::size_t>(h[o + 1]) << 8) |
               (static_cast<std::size_t>(h[o + 2]) << 16) | (static_cast<std::size_t>(h[o + 3]) << 24);
    };
    const std::size_t spr = u32(0x0C);
    const std::size_t rows = u32(0x10);
    if (rows == 0 || pixels == 0 || spr < 3 * pixels) {
        return void_failure(ErrorKind::image_bad_geometry, "no lines to preview");
    }
    const std::size_t step = std::max<std::size_t>(1, (std::max(rows, pixels) + kPreviewMax - 1) / kPreviewMax);
    const std::size_t w = (rows + step - 1) / step;
    const std::size_t hgt = (pixels + step - 1) / step;
    std::vector<std::uint16_t> rgb(w * hgt * 3);
    std::vector<std::uint8_t> row(spr * 2);
    for (std::size_t r = 0; r < rows; ++r) {
        if (!in.read(reinterpret_cast<char*>(row.data()), static_cast<std::streamsize>(row.size()))) {
            return void_failure(ErrorKind::io_failure, std::format("{} ends at row {}", raw_path, r));
        }
        if (r % step != 0) {
            continue;
        }
        for (std::size_t p = 0; p < pixels; p += step) {
            for (std::size_t c = 0; c < 3; ++c) {
                const std::size_t s = 2 * (3 * p + c);
                // Oriented like the frame TIFFs for the normal head-first
                // load: travel axis reversed (process/positive.hpp, Feed).
                rgb[((p / step) * w + (w - 1 - r / step)) * 3 + c] =
                    static_cast<std::uint16_t>(row[s] | (row[s + 1] << 8));
            }
        }
    }
    return image::write_tiff16(tiff_path, static_cast<std::uint32_t>(w),
                               static_cast<std::uint32_t>(hgt), 3, rgb);
}

image::RawStreamMeta raw_meta(const scan::Geometry& g) {
    image::RawStreamMeta meta;
    meta.has_ir_lane = g.ir;
    if (g.ir) {
        meta.ir_region = std::pair<std::uint32_t, std::uint32_t>{
            static_cast<std::uint32_t>(3 * g.pixels()), static_cast<std::uint32_t>(g.pixels())};
    }
    return meta;
}

// Film rows straight to the .pakraw file (scan::IRowSink).
// Also reports the film rows on `out` as they arrive, flushed, so an app
// reading the pipe (PakonX) can show the film pass moving.
class FileRowSink final : public scan::IRowSink {
public:
    static constexpr std::size_t kProgressRows = 1000;

    FileRowSink(image::StreamRawWriter& writer, std::FILE* out) : writer_(writer), out_(out) {}
    VoidResult append(std::span<const std::uint16_t> row) override {
        auto r = writer_.append(row);
        if (r && ++rows_ % kProgressRows == 0) {
            std::fprintf(out_, "progress: film rows %zu\n", rows_);
            std::fflush(out_);
        }
        return r;
    }

private:
    image::StreamRawWriter& writer_;
    std::FILE* out_;
    std::size_t rows_{0};
};

void print_unit(std::FILE* out, const eeprom::UnitCalibration& u, scan::ScanMode m) {
    const auto& b = u.base[scan::base_index(m.base)];
    std::fprintf(out, "EEPROM:  serial %u, type %u, hw %u (section A %s, section B %s)\n", u.serial,
                 u.scanner_type, u.hardware_version, std::string(to_string(u.section_a)).c_str(),
                 std::string(to_string(u.section_b)).c_str());
    std::fprintf(out, "         %s: Offset %u, MotorSpeed %u, MotorSpeed IR %u, adjust %u/%u/%u/%u"
                      " -> rate %u\n",
                 std::string(scan::mode_name(m)).c_str(), b.offset, b.motor_speed,
                 b.motor_speed_ir, b.adjust[0], b.adjust[1], b.adjust[2], b.adjust[3],
                 eeprom::motor_rate(b, m.ir));
    for (const auto& n : u.notes) {
        std::fprintf(out, "         note: %s\n", n.c_str());
    }
}

void print_stats(std::FILE* out, const scan::ScanResult& r, std::size_t pixels) {
    const auto& s = r.stream;
    const auto& img = r.kind == scan::ScanKind::first_light ? r.white : r.film;
    const std::size_t rows = r.kind == scan::ScanKind::film ? r.film_rows : img.rows();
    std::fprintf(out, "scan-stats:\n");
    std::fprintf(out, "  rows %zu, samples/row %u, pixels/row %zu, bytes %zu\n", rows,
                 img.geometry.samples_per_row, pixels,
                 rows * img.geometry.samples_per_row * 2);
    if (r.kind == scan::ScanKind::film) {
        std::fprintf(out, "  line period %.3f ms, film end: %s", r.line_period_ms,
                     std::string(scan::to_string(r.film_end)).c_str());
        if (r.film_start_line) {
            std::fprintf(out, " (film lines %zu..%s)", *r.film_start_line,
                         r.film_end_line ? std::to_string(*r.film_end_line).c_str() : "?");
        }
        std::fprintf(out, "\n  motor rate %u\n", r.motor_rate);
        if (r.film_end_vetoes > 0) {
            std::fprintf(out, "  film end vetoed %zu time%s: bright film base, the DX sensor still saw film\n",
                         r.film_end_vetoes, r.film_end_vetoes == 1 ? "" : "s");
        }
        if (r.run_out.ran) {
            std::fprintf(out, "  run-out: motor ran %.1f s after the row cap, %s\n",
                         r.run_out.seconds,
                         r.run_out.cleared ? "DX sensor clear"
                                           : "LIMIT REACHED - film may still be in the path");
        }
    }
    std::fprintf(out, "  stream: in %llu B, out %llu B, discarded %llu B, transfers %llu, "
                      "ring high-water %zu B,\n          overflows %llu (%llu B), transfer "
                      "errors %llu, line resyncs %zu\n",
                 static_cast<unsigned long long>(s.bytes_in),
                 static_cast<unsigned long long>(s.bytes_out),
                 static_cast<unsigned long long>(s.bytes_discarded),
                 static_cast<unsigned long long>(s.transfers), s.high_water,
                 static_cast<unsigned long long>(s.overflow_events),
                 static_cast<unsigned long long>(s.overflow_bytes),
                 static_cast<unsigned long long>(s.transfer_errors), r.resyncs);
    const auto& c = r.corrections;
    std::fprintf(out, "  corrections: A/D offsets %d %d %d, gains %u %u %u, LED R%u G%u B%u IR%u, "
                      "on-time %.3f %.3f %.3f %.3f\n",
                 c.offsets[0], c.offsets[1], c.offsets[2], c.gains[0], c.gains[1], c.gains[2],
                 c.currents.r, c.currents.g, c.currents.b, c.currents.ir, c.duties.r, c.duties.g,
                 c.duties.b, c.duties.ir);
    std::fprintf(out, "               rounds: dark %zu, current %zu, on-time %zu\n", c.dark_rounds,
                 c.current_rounds, c.duty_rounds);
    for (const auto& p : r.phases) {
        std::fprintf(out, "  phase %-18s %8.3f s\n", p.name.c_str(), p.seconds);
    }
    for (const auto& e : r.events) {
        std::fprintf(out, "  event: %s\n", e.c_str());
    }
}

void print_teardown(std::FILE* out, const scan::TeardownReport& t) {
    if (!t.ran) {
        std::fprintf(out, "teardown: not reached\n");
        return;
    }
    std::fprintf(out, "teardown: %s\n", t.all_ok() ? "all steps acknowledged" : "STEP FAILURES");
    for (const auto& s : t.steps) {
        if (!s.ok) {
            std::fprintf(out, "  FAILED %s: %s\n", s.what.c_str(), s.error.c_str());
        }
    }
}

std::vector<double> json_numbers(const process::Json& j) {
    std::vector<double> v;
    if (const auto* a = j.array()) {
        for (const auto& x : *a) {
            v.push_back(x.number().value_or(0.0));
        }
    }
    return v;
}

scan::LineAverage json_reference(const process::Json& j) {
    scan::LineAverage a;
    a.lines = static_cast<std::size_t>(j["lines"].number().value_or(0));
    a.pixels = static_cast<std::size_t>(j["pixels"].number().value_or(0));
    if (const auto* planes = j["planes"].array()) {
        for (const auto& p : *planes) {
            a.planes.push_back(json_numbers(p));
        }
    }
    return a;
}

// The earlier strip's corrections for a hot start, when it is the same
// unit and mode and recent; otherwise why not (the scan then calibrates).
std::variant<scan::CorrectionsResult, std::string> hot_start_from(const std::string& path,
                                                                  const Options& o,
                                                                  std::uint32_t serial) {
    std::error_code ec;
    const auto when = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return std::format("{}: not found", path);
    }
    if (std::filesystem::file_time_type::clock::now() - when > kHotStartWindow) {
        return std::format("{}: older than {} min", path, kHotStartWindow.count());
    }
    std::ifstream f(path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    auto j = process::parse_json(text);
    if (!j) {
        return std::format("{}: {}", path, j.error().message);
    }
    const auto& s = *j;
    if (s["mode"].string() != std::optional<std::string>(std::string(scan::mode_name(o.scan_mode)))) {
        return "another scan mode";
    }
    if (s["unit"]["serial"].number() != std::optional<double>(static_cast<double>(serial))) {
        return "another scanner";
    }
    const auto& c = s["corrections"];
    const auto off = json_numbers(c["ad_offsets"]);
    const auto gain = json_numbers(c["ad_gains"]);
    if (off.size() != 3 || gain.size() != 3) {
        return "no corrections in it";
    }
    scan::CorrectionsResult r;
    for (std::size_t k = 0; k < 3; ++k) {
        r.offsets[k] = static_cast<int>(off[k]);
        r.gains[k] = static_cast<std::uint16_t>(gain[k]);
    }
    const auto u8 = [&](const char* k) {
        return static_cast<std::uint8_t>(c["led_currents"][k].number().value_or(0));
    };
    r.currents = {u8("r"), u8("g"), u8("b"), u8("ir")};
    const auto d = [&](const char* k) { return c["on_time"][k].number().value_or(0); };
    r.duties = {d("r"), d("g"), d("b"), d("ir")};
    r.dark = json_reference(c["dark"]);
    r.white = json_reference(c["white"]);
    return r;
}

int run_live(const Options& o, const ScanCliDeps& deps, std::FILE* out, std::FILE* err) {
    if (!deps.open_session) {
        std::fprintf(err, "error: scan: no session opener wired\n");
        return 1;
    }
    const auto outputs = outputs_for(o);
    if (o.stop_on_stdin && !watch_stdin_for_stop()) {
        std::fprintf(err, "error: scan: --stop-on-stdin: cannot watch stdin\n");
        return 1;
    }
    // Film rows stream to disk: the whole row cap must fit there. Checked
    // before the device is even opened (the pixel width does not depend
    // on the EEPROM Offset).
    if (!o.first_light) {
        const std::uint64_t need =
            image::kStreamRawHeaderBytes +
            std::uint64_t{o.film_rows} * scan::film_geometry(o.scan_mode, 0).samples_per_line() * 2;
        std::error_code ec;
        const auto dir = std::filesystem::absolute(outputs.raw, ec).parent_path();
        const auto space = std::filesystem::space(dir, ec);
        if (!ec && space.available < need) {
            std::fprintf(err,
                         "error: scan: %zu rows need %llu MB on %s, %llu MB free - lower "
                         "--film-rows or free space\n",
                         o.film_rows, static_cast<unsigned long long>(need >> 20),
                         dir.string().c_str(),
                         static_cast<unsigned long long>(space.available >> 20));
            return 1;
        }
    }
    auto opened = deps.open_session(); // the single device entry point
    if (!opened) {
        std::fprintf(err, "error: scan: open session: %s: %s\n",
                     std::string(to_string(opened.error().kind)).c_str(),
                     opened.error().message.c_str());
        return 1;
    }
    struct DisconnectGuard {
        scanner::Scanner* s;
        ~DisconnectGuard() {
            if (s) {
                s->disconnect();
            }
        }
    } guard{opened->scanner.get()};
    if (opened->identity.model != scanner::Model::f135_plus) {
        std::fprintf(err, "error: scan: requires an F-135+ (0x40/0x44); identify reported %s\n",
                     std::string(scanner::to_string(opened->identity.model)).c_str());
        return 1;
    }
    auto& transport = opened->client->transport();
    auto image = eeprom::read_sections(transport);
    if (!image) {
        std::fprintf(err, "error: scan: EEPROM read: %s\n", image.error().message.c_str());
        return 1;
    }
    const auto unit = eeprom::parse(*image);
    print_unit(out, unit, o.scan_mode);
    if (unit.uses_fallback()) {
        std::fprintf(err, "WARNING: this unit's EEPROM could not supply every field - the scan "
                          "uses FALLBACK values from capture unit 16402 for them (see notes)\n");
    }
    auto pipe = transport.open_bulk_in(protocol::scan::kImageEndpoint, stream::kQueuedReads);
    if (!pipe) {
        std::fprintf(err, "error: scan: image pipe: %s\n", pipe.error().message.c_str());
        return 1;
    }
    stream::ImageStream stream(std::move(*pipe));
    scan::RunnerConfig cfg;
    cfg.mode = o.scan_mode;
    cfg.kind = o.first_light ? scan::ScanKind::first_light : scan::ScanKind::film;
    cfg.addresses = opened->identity.addresses;
    cfg.unit = unit;
    cfg.first_light_lines = o.first_light_lines;
    cfg.film_row_budget = o.film_rows;
    cfg.cancel = &interrupt_token();
    cfg.clear_base = o.clear_base || scan::clear_base(o.film);
    cfg.film = o.film;
    cfg.film_gate = o.film_gate;
    if (!o.hot_start.empty()) {
        auto hot = hot_start_from(o.hot_start, o, unit.serial);
        if (auto* why = std::get_if<std::string>(&hot)) {
            std::fprintf(out, "hot start: not used (%s); calibrating in full\n", why->c_str());
        } else {
            cfg.hot_start = std::get<scan::CorrectionsResult>(std::move(hot));
        }
    }
    const auto offset = unit.base[scan::base_index(o.scan_mode.base)].offset;
    const auto geom = o.first_light ? scan::calibration_geometry(o.scan_mode, offset, o.scan_mode.ir)
                                    : scan::film_geometry(o.scan_mode, offset);
    // Film rows go straight to the .pakraw file; opened before any motion.
    std::unique_ptr<image::StreamRawWriter> film_file;
    std::optional<FileRowSink> film_sink;
    if (!o.first_light) {
        auto w = image::StreamRawWriter::open(
            outputs.raw, static_cast<std::uint32_t>(geom.samples_per_line()), raw_meta(geom));
        if (!w) {
            std::fprintf(err, "error: scan: %s\n", w.error().message.c_str());
            return 1;
        }
        film_file = std::move(*w);
        film_sink.emplace(*film_file, out);
        cfg.film_sink = &*film_sink;
    }
    auto runner = scan::ScanRunner::create(cfg);
    if (!runner) {
        std::fprintf(err, "error: scan: %s\n", runner.error().message.c_str());
        return 1;
    }
    scan::PpbCommandChannel commands(*opened->client);
    std::fprintf(out, "live scan: %s, %s\n", std::string(scan::mode_name(o.scan_mode)).c_str(),
                 o.first_light ? "first light" : std::format("film, cap {} rows", o.film_rows).c_str());
    std::fflush(out);
    if (!install_interrupt_handlers()) {
        std::fprintf(err, "warning: scan: interrupt handlers unavailable\n");
    }
    auto result = (*runner)->run(commands, stream);
    restore_interrupt_handlers();

    const auto& r = result ? *result : (*runner)->partial();
    if (!result) {
        std::fprintf(err, "error: scan: %s: %s\n",
                     std::string(to_string(result.error().kind)).c_str(),
                     result.error().message.c_str());
    }
    print_stats(out, r, geom.pixels());
    print_teardown(result && r.teardown.all_ok() ? out : err, r.teardown);

    // Save whatever was captured, even after a failure (for analysis).
    int code = result && r.teardown.all_ok() ? 0 : 1;
    std::size_t rows = 0;
    bool raw_ok = true;
    if (o.first_light) {
        rows = r.white.rows();
        if (rows > 0) {
            if (auto w = image::write_stream_raw(outputs.raw, r.white, raw_meta(geom)); !w) {
                std::fprintf(err, "error: write %s: %s\n", outputs.raw.c_str(),
                             w.error().message.c_str());
                raw_ok = false;
            }
        }
    } else {
        rows = static_cast<std::size_t>(film_file->rows());
        if (auto w = film_file->finish(); !w) {
            std::fprintf(err, "error: write %s: %s\n", outputs.raw.c_str(), w.error().message.c_str());
            raw_ok = false;
        }
    }
    if (!raw_ok) {
        code = 1;
    } else if (rows > 0) {
        std::fprintf(out, "written: %s\n", outputs.raw.c_str());
        if (auto w = write_preview(outputs.tiff, outputs.raw, geom.pixels()); !w) {
            std::fprintf(err, "error: write %s: %s\n", outputs.tiff.c_str(), w.error().message.c_str());
            code = 1;
        } else {
            std::fprintf(out, "written: %s (linear RGB preview, unprocessed)\n", outputs.tiff.c_str());
        }
    }
    // Everything later processing needs besides the pixels (scan/sidecar.hpp).
    if (std::FILE* f = std::fopen(outputs.json.c_str(), "w")) {
        const auto json = scan::sidecar_json(r);
        std::fwrite(json.data(), 1, json.size(), f);
        std::fclose(f);
        std::fprintf(out, "written: %s\n", outputs.json.c_str());
    }
    if (std::FILE* f = std::fopen(outputs.text.c_str(), "w")) {
        print_unit(f, unit, o.scan_mode);
        print_stats(f, r, geom.pixels());
        print_teardown(f, r.teardown);
        for (const auto& line : r.corrections.log) {
            std::fprintf(f, "corrections: %s\n", line.c_str());
        }
        std::fclose(f);
        std::fprintf(out, "written: %s\n", outputs.text.c_str());
    }
    return code;
}

void print_usage_line(std::FILE* err) {
    std::fprintf(err, "usage: pakon-cli scan (--dry-run | --live-scan) --config <mode>\n"
                      "                     (--first-light [--first-light-lines N] | --film-rows N)"
                      " --out-prefix <path> [--film K] [--film-gate G] [--hot-start J] [--stop-on-stdin]\n");
}

} // namespace

int run_scan_command(const std::vector<std::string>& args, const ScanCliDeps& deps,
                     std::FILE* out, std::FILE* err) {
    const auto parsed = parse_options(args);
    if (parsed.code != 0) {
        std::fprintf(err, "error: scan: %s\n", parsed.error.c_str());
        print_usage_line(err);
        return parsed.code;
    }
    if (parsed.options.mode == Mode::dry_run) {
        print_preflight(parsed.options, out);
        return 0;
    }
    return run_live(parsed.options, deps, out, err);
}

int run_eeprom_command(const std::vector<std::string>& args, const ScanCliDeps& deps,
                       std::FILE* out, std::FILE* err) {
    std::string dump;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--out" && i + 1 < args.size()) {
            dump = args[++i];
        } else {
            std::fprintf(err, "error: eeprom: unknown argument '%s'\nusage: pakon-cli eeprom "
                              "[--out <file>]\n",
                         args[i].c_str());
            return 2;
        }
    }
    if (!deps.open_session) {
        std::fprintf(err, "error: eeprom: no session opener wired\n");
        return 1;
    }
    auto opened = deps.open_session();
    if (!opened) {
        std::fprintf(err, "error: eeprom: open session: %s\n", opened.error().message.c_str());
        return 1;
    }
    struct DisconnectGuard {
        scanner::Scanner* s;
        ~DisconnectGuard() {
            if (s) {
                s->disconnect();
            }
        }
    } guard{opened->scanner.get()};
    auto image = eeprom::read_sections(opened->client->transport());
    if (!image) {
        std::fprintf(err, "error: eeprom: %s\n", image.error().message.c_str());
        return 1;
    }
    const auto unit = eeprom::parse(*image);
    for (const auto m : {scan::ScanMode{scan::Base::b4, false}, scan::ScanMode{scan::Base::b4, true},
                         scan::ScanMode{scan::Base::b8, false}, scan::ScanMode{scan::Base::b8, true},
                         scan::ScanMode{scan::Base::b16, false},
                         scan::ScanMode{scan::Base::b16, true}}) {
        const auto& b = unit.base[scan::base_index(m.base)];
        std::fprintf(out, "%-10s Offset %3u  rate %5u\n", std::string(scan::mode_name(m)).c_str(),
                     b.offset, eeprom::motor_rate(b, m.ir));
    }
    print_unit(out, unit, {scan::Base::b4, false});
    if (unit.section_a != eeprom::CopySource::fallback) {
        for (std::size_t r = 0; r < 3; ++r) {
            std::fprintf(out, "%s", r ? "         " : "NegMatrix");
            for (std::size_t k = 0; k < 10; ++k) {
                std::fprintf(out, " %.6g", static_cast<double>(unit.neg_matrix[10 * r + k]));
            }
            std::fprintf(out, "\n");
        }
    }
    if (unit.uses_fallback()) {
        std::fprintf(err, "WARNING: EEPROM sections unreadable - FALLBACK values shown\n");
    }
    if (!dump.empty()) {
        if (std::FILE* f = std::fopen(dump.c_str(), "wb")) {
            std::fwrite(image->data(), 1, image->size(), f);
            std::fclose(f);
            std::fprintf(out, "raw sections written to %s (0xFF = not read)\n", dump.c_str());
        }
    }
    return 0;
}

// film-drive: PSI's Advance / Reverse Film for film stuck in the
// transport (scan/runner.hpp drive_film). Like scan, nothing touches the
// device without the explicit --live; usage errors exit 2 first.
int run_drive_command(const std::vector<std::string>& args, const ScanCliDeps& deps,
                      std::FILE* out, std::FILE* err) {
    int dir = 0; // +1 forward, -1 reverse
    bool live = false;
    bool stop_on_stdin = false;
    long seconds = 300;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "--forward" || a == "--reverse") {
            const int d = a == "--forward" ? 1 : -1;
            if (dir != 0 && dir != d) {
                std::fprintf(err, "error: film-drive: --forward and --reverse are mutually exclusive\n");
                return 2;
            }
            dir = d;
        } else if (a == "--live") {
            live = true;
        } else if (a == "--stop-on-stdin") {
            stop_on_stdin = true;
        } else if (a == "--seconds" && i + 1 < args.size()) {
            const auto n = parse_count(args[++i]);
            if (!n || *n == 0 || *n > 300) {
                std::fprintf(err, "error: film-drive: --seconds wants 1..300 (the OEM's limit)\n");
                return 2;
            }
            seconds = static_cast<long>(*n);
        } else {
            std::fprintf(err, "error: film-drive: unknown argument '%s'\n", a.c_str());
            return 2;
        }
    }
    if (dir == 0) {
        std::fprintf(err, "usage: pakon-cli film-drive (--forward | --reverse) [--seconds N] "
                          "[--stop-on-stdin] --live\n");
        return 2;
    }
    std::fprintf(out, "film-drive: %s at motor rate %u (0xA5), %s, up to %ld s; Stop or the "
                      "limit ends it with the teardown (0xA2 stop)\n",
                 dir > 0 ? "forward" : "reverse", scan::kDriveFilmRate,
                 dir > 0 ? "0xA0" : "0xA1", seconds);
    if (!live) {
        std::fprintf(out, "dry run only - nothing was sent; --live drives the film.\n");
        return 0;
    }
    if (!deps.open_session) {
        std::fprintf(err, "error: film-drive: no session opener wired\n");
        return 1;
    }
    if (stop_on_stdin && !watch_stdin_for_stop()) {
        std::fprintf(err, "error: film-drive: --stop-on-stdin: cannot watch stdin\n");
        return 1;
    }
    auto opened = deps.open_session();
    if (!opened) {
        std::fprintf(err, "error: film-drive: open session: %s\n", opened.error().message.c_str());
        return 1;
    }
    struct DisconnectGuard {
        scanner::Scanner* s;
        ~DisconnectGuard() {
            if (s) {
                s->disconnect();
            }
        }
    } guard{opened->scanner.get()};
    if (opened->identity.model != scanner::Model::f135_plus) {
        std::fprintf(err, "error: film-drive: requires an F-135+ (0x40/0x44)\n");
        return 1;
    }
    scan::PpbCommandChannel commands(*opened->client);
    if (!install_interrupt_handlers()) {
        std::fprintf(err, "warning: film-drive: interrupt handlers unavailable\n");
    }
    auto r = scan::drive_film(commands, opened->identity.addresses, dir < 0,
                              std::chrono::seconds(seconds), &interrupt_token());
    restore_interrupt_handlers();
    if (!r.error.empty()) {
        std::fprintf(err, "error: film-drive: %s\n", r.error.c_str());
    } else {
        std::fprintf(out, "film-drive: %s after %.1f s\n", r.stopped ? "stopped" : "limit reached",
                     r.seconds);
    }
    print_teardown(r.teardown.all_ok() ? out : err, r.teardown);
    return r.error.empty() && r.teardown.all_ok() ? 0 : 1;
}

// f235-plan [--ir]: the frames of one F-235 film pass and its teardown, as
// decoded from TLA.dll (docs/F235.md). Dry run only: there is no live F-235
// path (no unit captured; the PPB allow-list refuses its addresses).
int run_f235_plan_command(const std::vector<std::string>& args, std::FILE* out, std::FILE* err) {
    bool ir = false;
    for (const auto& a : args) {
        if (a == "--ir") {
            ir = true;
        } else {
            std::fprintf(err, "error: f235-plan: unknown argument '%s' (only --ir)\n", a.c_str());
            return 2;
        }
    }
    // Example inputs: the middle of each decoded range, NOT calibration.
    scan::F235Settings s;
    s.lamp_level = (0x2CEC + 0x3A34) / 2;
    s.lamp_ir = ir ? 0x5DC / 2 : 0;
    s.pixel_start = 0;
    s.pixel_count = 0x800;
    s.integration = 0x800;
    s.ir_window = ir ? 0x400 : 0;
    s.gains[0] = s.gains[1] = s.gains[2] = 0x20;
    for (auto& e : s.exposures) e = 0x800;
    s.drive_rate = 1000;
    auto plan = scan::f235_scan_plan(s);
    if (!plan) {
        std::fprintf(err, "error: %s\n", plan.error().message.c_str());
        return 1;
    }
    std::fprintf(out, "F-235 film pass plan (UNVERIFIED, from TLA.dll 3.1.0.28; example inputs = range midpoints,\n"
                      "not calibration). Dry run: nothing is sent, and no live F-235 path exists.\n");
    const auto show = [&](const scan::F235Step& st) {
        auto bytes = st.frame.serialize();
        std::string hex;
        if (bytes) {
            for (auto b : *bytes) hex += std::format("{:02x} ", b);
        }
        std::fprintf(out, "  %-44s %s\n", st.what.c_str(), hex.c_str());
    };
    for (const auto& st : *plan) show(st);
    std::fprintf(out, "teardown:\n");
    for (const auto& st : scan::f235_teardown(ir)) show(st);
    return 0;
}

void print_scan_usage(std::FILE* out) {
    std::fprintf(out,
                 "scan - capture a scan (explicit opt-in; nothing runs by default)\n"
                 "  pakon-cli scan --dry-run ...    print the exact plan, open nothing\n"
                 "  pakon-cli scan --live-scan ...  run it over ONE warm session (F-135+)\n"
                 "  --config <mode>         base4 | base4-ir | base8 | base8-ir | base16 | base16-ir\n"
                 "  --first-light           calibration window only: Corrections + white lines,\n"
                 "                          lamp on, motor never engaged\n"
                 "  --first-light-lines <N> white lines to keep (default 256)\n"
                 "  --film-rows <N>         film pass; N = hard row cap (film end and a 45 s\n"
                 "                          no-film timeout end it earlier)\n"
                 "  --out-prefix <path>     writes <path>.{white|film}.pakraw, .tiff preview,\n"
                 "                          <path>.scan-stats.txt, <path>.scan.json\n"
                 "  --bw                    black-and-white film: clear base between frames does\n"
                 "                          not end the pass (the end waits 1.2 frame pitches)\n"
                 "  --film <kind>           c41 (default) | c41-unmasked | e6 | bw-silver | bw-c41:\n"
                 "                          films without an orange mask scan at the open-gate\n"
                 "                          on-times; clear-base films get the --bw film end;\n"
                 "                          bw-silver refuses IR modes\n"
                 "  --film-gate <g>         auto (default: light + DX sensor) | brightness |\n"
                 "                          off (no film end: the row cap ends the pass)\n"
                 "  --hot-start <scan.json> reuse that strip's Corrections when it is the same\n"
                 "                          unit and mode and at most 10 min old (batch scans)\n"
                 "  --stop-on-stdin         a \"stop\" line on stdin, or stdin closing, stops the\n"
                 "                          live run like Ctrl+C (for an app driving the scan)\n"
                 "eeprom [--out <file>] - read-only EEPROM dump + parsed per-unit values\n"
                 "Ctrl+C during a live run stops at the next bounded point; the teardown\n"
                 "(lamp off, acquire off, motor stop, rate=0 -> go -> idle) always runs.\n"
                 "exit codes: 0 ok, 1 runtime failure, 2 usage error (no USB traffic)\n");
}

} // namespace pakon::cli
