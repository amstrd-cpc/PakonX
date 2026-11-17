#pragma once

// Film start / end detection for the film window.
//
// The OEM decides film start and end inside TLB's processing thread from
// the scanned lines (panel LEDs 0x0215 at the leading edge, 0x02D4 at the
// end; the acquire bit is cleared ~20 ms later; no film → window closed
// after ~45 s) — the algorithm itself is not recovered (docs/OEM_RE.md
// §10, [UNKNOWN]). This detector is this stack's own, [INFERRED] design:
// the open-gate level is measured on the leader (the gate is empty when
// the drive starts, as the OEM requires), film is "present" when the
// green level drops below 85 % of it for 16 consecutive lines, and the
// film has passed when the level is back above 95 % for 64 consecutive
// lines. The caller's row budget is always a hard cap on top.
//
// Clear-based (black-and-white) film is nearly as bright as the open gate
// between its frames, so "back above 95 %" alone ended the first B&W film
// pass after one frame (2026-10-23). The runner confirms an end with the
// DX sensor and calls resume() when the sensor still reports film.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace pakon::scan {

class FilmDetector {
public:
    enum class State { measuring_open_gate, waiting_for_film, in_film, ended };

    static constexpr std::size_t kReferenceLines = 32;
    static constexpr std::size_t kEnterLines = 16;
    static constexpr std::size_t kExitLines = 64;
    static constexpr double kEnterRatio = 0.85;
    static constexpr double kExitRatio = 0.95;

    // `pixels`: pixels per line; the level is the mean green sample over
    // the central 60 % of the line. `exit_lines`: how long the gate must
    // be bright again before the film counts as passed (kExitLines; longer
    // than a frame and its gap for clear-based film).
    explicit FilmDetector(std::size_t pixels, std::size_t exit_lines = kExitLines)
        : pixels_(pixels), exit_lines_(exit_lines) {}

    // Feed one line (interleaved RGB[, IR block]); returns the new state.
    State feed(std::span<const std::uint16_t> line) {
        const double level = green_level(line);
        ++lines_;
        switch (state_) {
        case State::measuring_open_gate:
            open_sum_ += level;
            if (lines_ >= kReferenceLines) {
                open_ = open_sum_ / static_cast<double>(kReferenceLines);
                state_ = State::waiting_for_film;
            }
            break;
        case State::waiting_for_film:
            run_ = level < kEnterRatio * open_ ? run_ + 1 : 0;
            if (run_ >= kEnterLines) {
                state_ = State::in_film;
                film_start_ = lines_ - kEnterLines;
                run_ = 0;
            }
            break;
        case State::in_film:
            run_ = level > kExitRatio * open_ ? run_ + 1 : 0;
            if (run_ >= exit_lines_) {
                state_ = State::ended;
                film_end_ = lines_ - exit_lines_;
            }
            break;
        case State::ended:
            break;
        }
        return state_;
    }

    // The light said "ended" but the film is still in the path (clear film
    // base): back to in_film, looking for the end again.
    void resume() {
        if (state_ == State::ended) {
            state_ = State::in_film;
            run_ = 0;
            film_end_.reset();
        }
    }

    State state() const { return state_; }
    double open_level() const { return open_; }
    std::optional<std::size_t> film_start() const { return film_start_; }
    std::optional<std::size_t> film_end() const { return film_end_; }
    std::size_t lines() const { return lines_; }

private:
    double green_level(std::span<const std::uint16_t> line) const {
        const std::size_t from = pixels_ / 5;
        const std::size_t to = pixels_ - pixels_ / 5;
        double s = 0;
        std::size_t n = 0;
        for (std::size_t p = from; p < to && 3 * p + 1 < line.size(); ++p) {
            s += line[3 * p + 1];
            ++n;
        }
        return n == 0 ? 0.0 : s / static_cast<double>(n);
    }

    std::size_t pixels_;
    std::size_t exit_lines_;
    State state_{State::measuring_open_gate};
    std::size_t lines_{0};
    std::size_t run_{0};
    double open_sum_{0};
    double open_{0};
    std::optional<std::size_t> film_start_;
    std::optional<std::size_t> film_end_;
};

// The film in the scanner, chosen by the operator before the pass. It
// changes the exposure and the film-end detection; development reads it
// back from the sidecar ("film").
//   c41           colour negative with an orange mask (the default);
//   c41_unmasked  colour negative without one (Harman Phoenix, Kodak
//                 Aerocolor, ...): clear base;
//   e6            colour slide: developed without inversion;
//   bw_silver     silver black-and-white: clear base, the silver blocks
//                 infrared, so an IR plane holds nothing usable;
//   bw_c41        chromogenic black-and-white (C-41 process).
enum class FilmKind { c41, c41_unmasked, e6, bw_silver, bw_c41 };

constexpr std::string_view film_name(FilmKind k) {
    switch (k) {
    case FilmKind::c41_unmasked: return "c41-unmasked";
    case FilmKind::e6: return "e6";
    case FilmKind::bw_silver: return "bw-silver";
    case FilmKind::bw_c41: return "bw-c41";
    case FilmKind::c41: break;
    }
    return "c41";
}

constexpr std::optional<FilmKind> parse_film(std::string_view s) {
    for (auto k : {FilmKind::c41, FilmKind::c41_unmasked, FilmKind::e6, FilmKind::bw_silver,
                   FilmKind::bw_c41}) {
        if (film_name(k) == s) {
            return k;
        }
    }
    return std::nullopt;
}

// kFilmBoost lifts the on-times over the orange mask's density; a film
// without the mask gets the open-gate on-times [INFERRED: the boost is
// ~10^D of the masked base, corrections.hpp]. bw_c41 keeps the boost:
// some chromogenic B&W stocks carry a mask.
constexpr bool film_boost(FilmKind k) { return k == FilmKind::c41 || k == FilmKind::bw_c41; }
// The gaps between frames are near open-gate bright (FilmDetector).
constexpr bool clear_base(FilmKind k) { return k != FilmKind::c41 && k != FilmKind::e6; }

} // namespace pakon::scan
