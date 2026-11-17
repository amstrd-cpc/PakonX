#pragma once

// F-235 PPB command frames, decoded from the OEM TLA.dll 3.1.0.28 (Ghidra
// 12.1.4; pakon-re/TLA.dll.c; docs/F235.md). UNVERIFIED: no F-235 has been
// captured. Every value carries its [DIS TLA@…] source. None of these
// addresses is in protocol::kKnownControllerAddresses, so the PPB client
// refuses to send any of them: this header builds frames for plans,
// dry runs and the simulator only.

#include <array>
#include <cstdint>

#include "pakon/ppb/packet.hpp"

namespace pakon::protocol::f235 {

// Bus addresses [DIS TLA, every PutCommand / PutRegister* call site].
inline constexpr std::uint8_t kUsbBoard = 0x10;   // data flow
inline constexpr std::uint8_t kCcd = 0xF0;        // FPGA + A/D, via PutRegisterCcd
inline constexpr std::uint8_t kTransport = 0xF4;  // drive, rollers, filter wheel, focus
inline constexpr std::uint8_t kLamp = 0xF6;
inline constexpr std::uint8_t kHostPic = 0xFE;

// PutRegisterCcd(reg, sub, value): [02 len F0 03 reg sub lo hi]; reg is 0x82
// (FPGA) or 0x84 (A/D) only [DIS TLA@0x1000e510].
inline ppb::Frame ccd(std::uint8_t reg, std::uint8_t sub, std::uint16_t value) {
    const std::array<std::uint8_t, 3> p{sub, static_cast<std::uint8_t>(value & 0xFF),
                                        static_cast<std::uint8_t>(value >> 8)};
    return ppb::make_write(kCcd, reg, p);
}
inline constexpr std::uint8_t kCcdFpga = 0x82;
inline constexpr std::uint8_t kCcdAd = 0x84;

// FPGA control (0x82 sub 0) bits [DIS TLA@0x1002f880 and callers].
inline constexpr std::uint16_t kCtrlAcquire = 0x001;
inline constexpr std::uint16_t kCtrlBinning = 0x002;
inline constexpr std::uint16_t kCtrlExposures = 0x008;
inline constexpr std::uint16_t kCtrlFirstSettings = 0x060;
inline constexpr std::uint16_t kCtrlIr = 0x100;

inline ppb::Frame word(std::uint8_t addr, std::uint8_t reg, std::uint16_t v) {
    const std::array<std::uint8_t, 2> p{static_cast<std::uint8_t>(v & 0xFF), static_cast<std::uint8_t>(v >> 8)};
    return ppb::make_write(addr, reg, p);
}
inline ppb::Frame byte(std::uint8_t addr, std::uint8_t reg, std::uint8_t v) {
    const std::array<std::uint8_t, 1> p{v};
    return ppb::make_write(addr, reg, p);
}

// Data flow start [DIS TLA@0x10033dd0 bBeforeScan, 0x10022400].
inline ppb::Frame data_flow_start() { return ppb::make_cmd(kUsbBoard, 0x84); }
// Lamp: level word 0x80 (0x2CEC..0x3A34), off command 0x81, IR word 0x90
// (<= 0x5DC) [DIS TLA@0x10033c70, 0x10032850].
inline constexpr std::uint16_t kLampMin = 0x2CEC;
inline constexpr std::uint16_t kLampMax = 0x3A34;
inline constexpr std::uint16_t kLampIrMax = 0x5DC;
inline ppb::Frame lamp_level(std::uint16_t level) { return word(kLamp, 0x80, level); }
inline ppb::Frame lamp_off() { return ppb::make_cmd(kLamp, 0x81); }
inline ppb::Frame lamp_ir(std::uint16_t level) { return word(kLamp, 0x90, level); }
// Filter wheel: 0xE3 (film colour 1 / 8), 0xE4 (2 / 4), 0xE5 (0x1000 / 0x2000)
// [DIS TLA@0x10033250].
inline ppb::Frame filter_wheel(std::uint8_t position) { return ppb::make_cmd(kTransport, position); }
// Pressure rollers: byte 0xEE = 0..3 [DIS TLA@0x1000f5a0].
inline ppb::Frame rollers(std::uint8_t position) { return byte(kTransport, 0xEE, position); }
// Film drive: rate word 0x85, go 0x80 forward / 0x81 reverse, stop 0x82;
// rate clamped [60, 2200] (hardware < 1000) or [200, 3700] [DIS TLA@0x100100d0,
// 0x1000e4d0].
inline ppb::Frame drive_rate(std::uint16_t rate) { return word(kTransport, 0x85, rate); }
inline ppb::Frame drive_go(bool reverse) { return ppb::make_cmd(kTransport, reverse ? 0x81 : 0x80); }
inline ppb::Frame drive_stop() { return ppb::make_cmd(kTransport, 0x82); }
// Every motor [DIS TLA@0x100015e0 bStopMotors].
inline ppb::Frame stop_motors() { return ppb::make_cmd(kHostPic, 0x82); }

} // namespace pakon::protocol::f235
