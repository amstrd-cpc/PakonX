#pragma once

// ScanRunner: the OEM scan state machine (docs/OEM_RE.md §4, §7-§10) on
// the concurrent image stream.
//
//   init      the OEM init block, byte-identical to the capture
//             (base4.jsonl 6.601-6.696)
//   warm-up   service polls every 200 ms until the lamp reports stable
//             (bLampTemperatureStable: ≤ 300 s)
//   corrections  stream started BEFORE the acquire bit; the recovered
//             closed loop (scan/corrections.hpp) inside one window
//   film      geometry at this unit's EEPROM Offset, Corrections
//             offsets/gains, lamp on, motor rate from the EEPROM, go,
//             acquire + DX start; lines consumed while service polls run;
//             the window ends on film end, no-film timeout or the row
//             budget (hard cap) — always by the HOST clearing the
//             acquire bit, never by waiting for the device to go quiet
//   run-out   after the row cap only: lamp off, motor on, until the DX
//             sensor reads clear (film is never left in the path)
//   teardown  ScanDevice::teardown(), exactly once, on every path
//             (success, error, Ctrl+C)
//
// first-light runs init, warm-up and Corrections with the lamp on and the
// motor never engaged, keeps dark + white lines, then tears down.

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pakon/eeprom/eeprom.hpp"
#include "pakon/image/format.hpp"
#include "pakon/scan/cancel.hpp"
#include "pakon/scan/corrections.hpp"
#include "pakon/scan/device.hpp"
#include "pakon/scan/film.hpp"
#include "pakon/scan/modes.hpp"
#include "pakon/scan/transport.hpp"
#include "pakon/stream/image_stream.hpp"

namespace pakon::scan {

enum class ScanKind { first_light, film };

// Where film rows go when they are not held in memory (the CLI streams
// them to the .pakraw file). A failing append ends the scan with the
// teardown, like any other error.
class IRowSink {
public:
    virtual ~IRowSink() = default;
    virtual VoidResult append(std::span<const std::uint16_t> row) = 0;
};

enum class FilmGate { automatic, brightness, off };

struct RunnerConfig {
    ScanMode mode{};
    ScanKind kind{ScanKind::first_light};
    protocol::ControllerAddresses addresses{protocol::kF135Plus};
    eeprom::UnitCalibration unit{};          // from eeprom::read_sections + parse
    std::size_t first_light_lines{256};      // white lines kept by first-light
    std::size_t film_row_budget{0};          // film: hard cap, required (> 0)
    // Film rows go here instead of ScanResult::film (memory then stays
    // flat however long the film is). Null: kept in memory, reserved
    // before the motor moves.
    IRowSink* film_sink{nullptr};
    std::chrono::milliseconds poll_interval{200};       // OEM idle poll
    std::chrono::milliseconds warmup_timeout{300000};   // bLampTemperatureStable
    std::chrono::milliseconds no_film_timeout{45000};   // capture: ~45 s
    std::chrono::milliseconds line_timeout{std::chrono::seconds(5)};
    double settle_scale{1.0};                // LED settle waits (tests: 0)
    // Row cap reached with film still in the path: the motor runs on
    // (acquire and lamp off) until the DX sensor reads clear for
    // run_out_clear, at most run_out_limit. 0 = from the motor rate:
    // 2 frames / 45 frames (a 36-exposure roll and its leader).
    std::chrono::milliseconds run_out_clear{0};
    std::chrono::milliseconds run_out_limit{0};
    const CancelToken* cancel{nullptr};
    // Clear-based (black-and-white) film: the film end needs the gate bright
    // for longer than a frame and its gap (1.2 frame pitches), on top of the
    // DX sensor check, so a clear gap or one thin frame never ends the pass.
    bool clear_base{false};
    // The film (film.hpp): without an orange mask the film window runs at
    // the open-gate on-times instead of the boosted ones.
    FilmKind film{FilmKind::c41};
    // How the film end is decided (the film gate):
    //   automatic   light detector, the DX sensor confirms (vetoes) the end;
    //   brightness  light detector alone (a DX sensor that misreports film);
    //   off         no film end: the pass runs to the row cap.
    FilmGate film_gate{FilmGate::automatic};
    // Hot start (corrections.hpp reuse_corrections): an earlier run's
    // corrections, tried before calibrating in full.
    std::optional<CorrectionsResult> hot_start;
};

struct PhaseTiming {
    std::string name;
    double seconds{0};
};

enum class FilmEnd { none, film_passed, no_film_timeout, row_budget };
std::string_view to_string(FilmEnd end);

struct ScanResult {
    ScanMode mode{};
    ScanKind kind{};
    FilmKind film_kind{FilmKind::c41};
    eeprom::UnitCalibration unit{};
    std::uint16_t motor_rate{0};
    Geometry film_geometry{};
    CorrectionsResult corrections{};
    Duties film_duties{};   // on-time fractions used for the film window
    image::RawImage white;  // first-light: white lines at the final settings
    image::RawImage film;   // film window lines, as streamed (empty with a film_sink)
    std::size_t film_rows{0}; // film window lines, in memory or sent to the sink
    FilmEnd film_end{FilmEnd::none};
    std::optional<std::size_t> film_start_line;
    std::optional<std::size_t> film_end_line;
    std::vector<PhaseTiming> phases;
    stream::StreamStats stream{};
    std::size_t resyncs{0};
    double line_period_ms{0};
    std::vector<std::string> events; // service events seen
    // Film ends the light detector reported while the DX sensor still saw
    // film (clear film base between frames): the pass went on.
    std::size_t film_end_vetoes{0};
    bool hot_started{false}; // corrections reused from an earlier run
    // Every DX record (PICL 0x90) read during the film window and the
    // run-out, verbatim, with the film row count when it was read: the
    // film's DX code and frame numbers are decoded offline
    // (process/dx.hpp, OEM_RE.md §10.1).
    struct DxRecord {
        std::size_t film_row{0};
        bool run_out{false};
        std::vector<std::uint8_t> bytes;
    };
    std::vector<DxRecord> dx_records;
    struct RunOut {
        bool ran{false};
        bool cleared{false}; // false: ended at the limit, film may remain
        double seconds{0};
    } run_out;
    TeardownReport teardown;
};

// Advance / Reverse Film (PSI's menu, for film stuck in the transport):
// IScanPictures::AdvanceFilm(300000 ms, +-1000) [DIS PSI.exe@0x4461f1],
// which TLB runs as rate 0xA5 = 1000 (the 0x44 board's slowest; clamped
// [1000, 0x7FFE]), 0xA0 forward / 0xA1 reverse, wait, 0xA2 stop
// [DIS TLB@0x1000b6d0, @0x1000a440]. Lamp and acquire stay off. Ends at
// the cancel token (Stop) or `limit`, always through the teardown.
// PakonX drives at the Base 4 scan rate instead of 1000 (operator's
// choice, 2026-11-17): 25676, the 0xA5 rate of a live Base 4 film pass
// [CAP river1.scan.json motor_rate]. shortcut: one unit's rate, not read
// from this unit's EEPROM; read eeprom::motor_rate if units differ much.
inline constexpr std::uint16_t kDriveFilmRate = 25676;
inline constexpr std::chrono::milliseconds kDriveFilmLimit{300000};
struct DriveFilmResult {
    double seconds{0};
    bool stopped{false}; // by the token, not the limit
    std::string error;   // a failed step; the teardown ran anyway
    TeardownReport teardown;
};
DriveFilmResult drive_film(ICommandChannel& commands,
                                   const protocol::ControllerAddresses& addresses, bool reverse,
                                   std::chrono::milliseconds limit, const CancelToken* stop);

class ScanRunner {
public:
    static Result<std::unique_ptr<ScanRunner>> create(RunnerConfig config);

    // Run the whole session. On failure the teardown has already run;
    // teardown_report() and partial() describe what happened.
    Result<ScanResult> run(ICommandChannel& commands, stream::IImageStream& stream);

    const ScanResult& partial() const { return result_; }
    const TeardownReport& teardown_report() const { return result_.teardown; }

    // The exact init block (OEM_RE.md §4.1 steps 6-9), for the dry run
    // and the capture-fidelity test.
    static std::vector<ppb::Frame> init_frames(const protocol::ControllerAddresses& a);

private:
    explicit ScanRunner(RunnerConfig config) : config_(std::move(config)) {}
    VoidResult run_phases(ScanDevice& device, stream::IImageStream& stream);
    VoidResult run_out(ScanDevice& device);

    RunnerConfig config_;
    ScanResult result_;
};

} // namespace pakon::scan
