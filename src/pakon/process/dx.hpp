#pragma once

// The film's DX edge code from the DX reader's records (processing, never
// on the scan path). The scanner's DX board decodes the barcode itself;
// the scan keeps every PICL 0x90 record verbatim in the sidecar
// (dx.records, with the film row at which it was read).
//
// Record [DIS TLB@10009790]: [u16 BE line counter][entry count][entries
// of 5 bytes]; entry = [flags << 4 | type][p0][p1][p2][p3]. The first
// entry's high nibble carries the film-sensor flags (0x20 / 0x10). Types
// 5..8 are edge events with a u16 BE position in p1..p2 (in scan lines:
// the counter advances with the line clock); types 3 and 4 are decoded
// DX data [DIS TLB@10013cd0, 10013f50, 35 mm layout]:
//   type 3: frame = (p1 >> 2) + (p2 & 1) * 64 (> 121: minus 128),
//           half frame "A" = p0 & 1, specifier = (p2 >> 1) & 0xf,
//           product = (p2 >> 6) + (p3 & 0x1f) * 4;
//           valid when p2 bit 5 is clear and the bit count of p1, p2 and
//           p3 & 0x1f is even;
//   type 4: product = (p1 >> 6) + (p2 & 0x1f) * 4, specifier =
//           (p1 >> 1) & 0xf, no frame; valid when p2 bit 5 is clear and
//           the bit count of p1 and p2 is even.
// A decoded code belongs to the latest type-5 mark before it in reading
// order (one mark per code on the film, half a frame pitch apart); the
// record counter is the film row + a constant (strip8dx: 34 on every
// record). The codes count half frames: a picture spans codes 2N and
// 2N + 1 and the OEM names it "NA" (strip, Base 8: DxCode frames 39 ..
// 49 -> 19A .. 24A). The DX sensor reads the film ahead of the gate:
// the codes land on their pictures shifted by kDxGateShift pitches
// (strip8dx: 860 / 827 / 768 rows of a 2337-row pitch, 2026-10-22).
//
// TLB keeps the first four distinct (product, specifier) pairs with a
// vote count each and takes the most voted. The product / specifier go
// to Ansel (look.hpp). Checked on this unit's captures (2026-10-13): the
// OEM's Base 16 strip decodes to 39/1 (3 votes, 1 parity reject) = its
// DxCode.txt "Product = 39, Specifier = 1"; our Base 4 strip to 95/7
// (10 votes) with frame codes 25A, 26A, ... 34A.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "pakon/process/json.hpp"

namespace pakon::process {

// The film's DX code as Ansel takes it; -1/-1 when not read.
struct FilmDx {
    int product{-1};
    int specifier{-1};
};

inline constexpr double kDxGateShift = 0.35; // frame pitches, sensor ahead of the gate

struct DxFrameCode {
    std::size_t read_row{0}; // film rows streamed when the record was read
    std::optional<std::size_t> mark_row; // film row of its type-5 mark
    int code{0};             // half frames: the picture between 2N and 2N + 1 is "NA"
    bool flag{false};        // p0 bit 0 (set on every code seen so far)
};

struct DxDecode {
    std::size_t records{0};
    std::size_t codes{0};    // type 3/4 entries seen
    std::size_t rejected{0}; // failed the parity / fixed-bit check
    std::optional<FilmDx> film;
    int votes{0};            // votes for the chosen product / specifier
    std::vector<DxFrameCode> frames;
    // Reading state across records.
    std::optional<std::size_t> last_mark_row;
};

// One record (30 bytes, as read); `film_row` tags its frame codes.
// `candidates` carries TLB's four vote slots across records.
void decode_dx_record(std::span<const std::uint8_t> record, std::size_t film_row, DxDecode& out,
                      std::vector<std::pair<FilmDx, int>>& candidates);

// All records of a sidecar (dx.records); empty when the scan has none.
DxDecode decode_dx(const Json& sidecar);

// The DX frame number N of each picture (lines [first, last)), from the
// codes with a mark: the codes' rows shifted by kDxGateShift x pitch are
// fitted with a fixed slope of two codes per pitch; nullopt without codes.
std::vector<std::optional<int>> dx_frame_numbers(const DxDecode& dx,
                                                 std::span<const std::pair<std::size_t, std::size_t>> pictures,
                                                 double pitch);

} // namespace pakon::process
