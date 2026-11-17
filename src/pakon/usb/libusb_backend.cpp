// libusb backend for IUsbTransport (Linux, macOS): the same endpoints and
// guards as win_usb_transport.cpp, over libusb-1.0. The image pipe keeps
// N asynchronous bulk-IN transfers queued, like WinUSB's overlapped reads.
//
// Permissions: Linux needs the udev rule PakonX installs
// (60-pakon.rules: 0f05:f235 / 0f05:f135 for the logged-in user); macOS
// needs none (a vendor-class device has no kernel driver to detach).
#ifndef _WIN32

#include <libusb.h>

#include <chrono>
#include <deque>
#include <format>
#include <mutex>

#include "pakon/eeprom/eeprom.hpp"
#include "pakon/logging/logger.hpp"
#include "pakon/protocol/scan_commands.hpp"
#include "pakon/usb/loader_control.hpp"
#include "pakon/usb/transport.hpp"

namespace pakon::usb {

namespace {

libusb_context* context() {
    static libusb_context* ctx = [] {
        libusb_context* c = nullptr;
        if (libusb_init(&c) != 0) {
            return static_cast<libusb_context*>(nullptr);
        }
        return c;
    }();
    return ctx;
}

ErrorKind kind_of(int rc) {
    switch (rc) {
    case LIBUSB_ERROR_TIMEOUT: return ErrorKind::usb_timeout;
    case LIBUSB_ERROR_ACCESS: return ErrorKind::usb_access_denied;
    case LIBUSB_ERROR_NO_DEVICE:
    case LIBUSB_ERROR_NOT_FOUND: return ErrorKind::usb_device_not_found;
    default: return ErrorKind::usb_io_failed;
    }
}

std::string text(int rc) { return std::format("{} ({})", libusb_error_name(rc), rc); }

// "usb:BUS-PORT.PORT" names the device the way it is reopened.
std::string path_of(libusb_device* d) {
    std::uint8_t ports[8];
    const int n = libusb_get_port_numbers(d, ports, 8);
    std::string p = std::format("usb:{}-", libusb_get_bus_number(d));
    for (int i = 0; i < n; ++i) p += std::format("{}{}", i ? "." : "", ports[i]);
    return p;
}

DeviceInfo describe(libusb_device* d, const libusb_device_descriptor& dd) {
    DeviceInfo info;
    info.vendor_id = dd.idVendor;
    info.product_id = dd.idProduct;
    info.bcd_device = dd.bcdDevice;
    info.device_path = path_of(d);
    // The Windows hardware-ID form: identity.hpp recognises boards by it.
    info.hardware_id = std::format("USB\\VID_{:04X}&PID_{:04X}&REV_{:04X}", dd.idVendor, dd.idProduct, dd.bcdDevice);
    libusb_config_descriptor* cfg = nullptr;
    if (libusb_get_active_config_descriptor(d, &cfg) == 0 || libusb_get_config_descriptor(d, 0, &cfg) == 0) {
        for (int i = 0; i < cfg->bNumInterfaces; ++i) {
            for (int a = 0; a < cfg->interface[i].num_altsetting; ++a) {
                const auto& alt = cfg->interface[i].altsetting[a];
                InterfaceInfo iface{alt.bInterfaceNumber, alt.bAlternateSetting, alt.bInterfaceClass,
                                    alt.bInterfaceSubClass, alt.bInterfaceProtocol, {}};
                for (int e = 0; e < alt.bNumEndpoints; ++e) {
                    const auto& ep = alt.endpoint[e];
                    iface.endpoints.push_back({ep.bEndpointAddress, ep.bmAttributes, ep.wMaxPacketSize});
                }
                info.interfaces.push_back(std::move(iface));
            }
        }
        libusb_free_config_descriptor(cfg);
    }
    libusb_device_handle* h = nullptr;
    const int rc = libusb_open(d, &h);
    if (rc == 0) {
        const auto str = [&](std::uint8_t idx) -> std::optional<std::string> {
            unsigned char buf[256];
            if (idx == 0) return std::nullopt;
            const int n = libusb_get_string_descriptor_ascii(h, idx, buf, sizeof buf);
            if (n <= 0) return std::nullopt;
            return std::string(reinterpret_cast<char*>(buf), static_cast<std::size_t>(n));
        };
        info.serial_number = str(dd.iSerialNumber);
        info.manufacturer = str(dd.iManufacturer);
        info.product = str(dd.iProduct);
        libusb_close(h);
    } else if (rc == LIBUSB_ERROR_ACCESS) {
        info.interface_note = "permission denied: install PakonX's udev rule (60-pakon.rules) and replug";
    }
    return info;
}

// Same last line of defence as the WinUSB backend: vendor OUT only for
// allow-listed EEPROM read-selects; no IN with the write codes.
VoidResult guard(eeprom::Direction direction, std::uint8_t request, std::uint16_t value, std::uint16_t index) {
    const bool ok = direction == eeprom::Direction::out
                        ? eeprom::is_allowed({direction, request, value, index})
                        : request != eeprom::kRequestWrite && request != eeprom::kRequestSelect;
    if (!ok) {
        return void_failure(ErrorKind::usb_not_supported,
                            std::format("refused vendor request 0x{:02x}/0x{:04x}/0x{:04x}: EEPROM writes are never sent",
                                        request, value, index));
    }
    return {};
}

// Opens the device at `path`, configuration 1, interface 0 claimed.
Result<libusb_device_handle*> open_path(const std::string& path) {
    if (!context()) return failure<libusb_device_handle*>(ErrorKind::usb_open_failed, "libusb_init failed");
    libusb_device** list = nullptr;
    const auto n = libusb_get_device_list(context(), &list);
    libusb_device_handle* h = nullptr;
    int rc = LIBUSB_ERROR_NOT_FOUND;
    for (ssize_t i = 0; i < n; ++i) {
        if (path_of(list[i]) == path) {
            rc = libusb_open(list[i], &h);
            break;
        }
    }
    libusb_free_device_list(list, 1);
    if (rc != 0) {
        return failure<libusb_device_handle*>(kind_of(rc), std::format("open {}: {}{}", path, text(rc),
            rc == LIBUSB_ERROR_ACCESS ? " - install PakonX's udev rule (60-pakon.rules) and replug" : ""));
    }
    libusb_set_auto_detach_kernel_driver(h, 1);
    int config = 0;
    if (libusb_get_configuration(h, &config) == 0 && config == 0) {
        libusb_set_configuration(h, 1);
    }
    if (rc = libusb_claim_interface(h, 0); rc != 0) {
        libusb_close(h);
        return failure<libusb_device_handle*>(kind_of(rc), std::format("claim interface 0 on {}: {}", path, text(rc)));
    }
    return h;
}

class LibusbBulkInPipe final : public IBulkInPipe {
public:
    LibusbBulkInPipe(libusb_device_handle* h, std::uint8_t endpoint, std::size_t slots)
        : h_(h), endpoint_(endpoint), slots_(slots) {
        for (auto& s : slots_) {
            s.owner = this;
            s.t = libusb_alloc_transfer(0);
        }
    }
    ~LibusbBulkInPipe() override {
        abort();
        // Retire every transfer before the caller's buffers go away.
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (pending() && std::chrono::steady_clock::now() < until) {
            timeval tv{0, 50000};
            libusb_handle_events_timeout_completed(context(), &tv, nullptr);
        }
        for (auto& s : slots_) libusb_free_transfer(s.t);
    }
    VoidResult submit(std::size_t slot, std::span<std::uint8_t> buffer) override {
        if (slot >= slots_.size() || slots_[slot].pending || !slots_[slot].t) {
            return void_failure(ErrorKind::usb_io_failed, std::format("read slot {} invalid or busy", slot));
        }
        auto& s = slots_[slot];
        s.done = false;
        libusb_fill_bulk_transfer(s.t, h_, endpoint_, buffer.data(), static_cast<int>(buffer.size()), &LibusbBulkInPipe::on_done, &s, 0);
        if (const int rc = libusb_submit_transfer(s.t); rc != 0) {
            return void_failure(kind_of(rc), std::format("bulk 0x{:02x} submit failed: {}", endpoint_, text(rc)));
        }
        s.pending = true;
        queue_.push_back(slot);
        return {};
    }
    Result<std::optional<PipeCompletion>> wait(std::chrono::milliseconds timeout) override {
        if (queue_.empty()) return std::optional<PipeCompletion>{};
        auto& s = slots_[queue_.front()];
        const auto until = std::chrono::steady_clock::now() + timeout;
        while (!s.done) {
            const auto left = std::chrono::duration_cast<std::chrono::microseconds>(until - std::chrono::steady_clock::now());
            if (left.count() <= 0) return std::optional<PipeCompletion>{};
            // tv_usec is int on macOS, long on Linux: no brace narrowing.
            timeval tv{};
            tv.tv_sec = static_cast<decltype(tv.tv_sec)>(left.count() / 1000000);
            tv.tv_usec = static_cast<decltype(tv.tv_usec)>(left.count() % 1000000);
            int completed = 0;
            libusb_handle_events_timeout_completed(context(), &tv, &completed);
        }
        PipeCompletion c;
        c.slot = queue_.front();
        queue_.pop_front();
        s.pending = false;
        c.bytes = static_cast<std::size_t>(s.t->actual_length);
        if (s.t->status == LIBUSB_TRANSFER_CANCELLED) {
            c.aborted = true;
        } else if (s.t->status != LIBUSB_TRANSFER_COMPLETED) {
            c.failed = true;
            c.error = std::format("bulk 0x{:02x}: transfer status {}", endpoint_, static_cast<int>(s.t->status));
        }
        return std::optional<PipeCompletion>{c};
    }
    void abort() override {
        for (auto& s : slots_) {
            if (s.pending && !s.done) libusb_cancel_transfer(s.t);
        }
    }

private:
    struct Slot {
        LibusbBulkInPipe* owner{nullptr};
        libusb_transfer* t{nullptr};
        bool pending{false};
        bool done{false};
    };
    static void LIBUSB_CALL on_done(libusb_transfer* t) { static_cast<Slot*>(t->user_data)->done = true; }
    bool pending() const {
        for (const auto& s : slots_) if (s.pending && !s.done) return true;
        return false;
    }
    libusb_device_handle* h_;
    std::uint8_t endpoint_;
    std::vector<Slot> slots_;
    std::deque<std::size_t> queue_;
};

class LibusbTransport final : public IUsbTransport {
public:
    LibusbTransport(DeviceInfo info, libusb_device_handle* h) : info_(std::move(info)), h_(h) {}
    ~LibusbTransport() override {
        libusb_release_interface(h_, 0);
        libusb_close(h_);
    }
    Result<std::vector<std::uint8_t>> command_exchange(std::span<const std::uint8_t> frame) override {
        if (frame.empty()) return failure<std::vector<std::uint8_t>>(ErrorKind::ppb_invalid_frame, "empty command frame");
        log::Logger::instance().hex(log::Level::trace, "TX", kCommandOutEndpoint, frame);
        int written = 0;
        if (const int rc = libusb_bulk_transfer(h_, kCommandOutEndpoint, const_cast<unsigned char*>(frame.data()),
                                                static_cast<int>(frame.size()), &written, kPipeTimeoutMs);
            rc != 0) {
            return failure<std::vector<std::uint8_t>>(kind_of(rc), std::format("command write 0x{:02x} failed: {}", kCommandOutEndpoint, text(rc)));
        }
        std::vector<std::uint8_t> reply;
        unsigned char chunk[1024];
        for (;;) {
            int read = 0;
            if (const int rc = libusb_bulk_transfer(h_, kCommandInEndpoint, chunk, sizeof chunk, &read, kPipeTimeoutMs); rc != 0) {
                return failure<std::vector<std::uint8_t>>(kind_of(rc), std::format("command read 0x{:02x} failed after {} reply bytes: {}",
                                                                                   kCommandInEndpoint, reply.size(), text(rc)));
            }
            if (read == 0) break;
            reply.insert(reply.end(), chunk, chunk + read);
            if (reply.size() >= 2 && reply.size() >= 2u + reply[1]) break;
        }
        log::Logger::instance().hex(log::Level::trace, "RX", kCommandInEndpoint, reply);
        return reply;
    }
    Result<std::vector<std::uint8_t>> bulk_read(std::uint8_t endpoint, std::size_t max_length) override {
        std::vector<std::uint8_t> buffer(max_length);
        int read = 0;
        if (const int rc = libusb_bulk_transfer(h_, endpoint, buffer.data(), static_cast<int>(max_length), &read, kPipeTimeoutMs); rc != 0) {
            return failure<std::vector<std::uint8_t>>(kind_of(rc), std::format("bulk read 0x{:02x} failed: {}", endpoint, text(rc)));
        }
        buffer.resize(static_cast<std::size_t>(read));
        log::Logger::instance().log(log::Level::trace, "RX ep=0x{:02x} {} bytes", endpoint, read);
        return buffer;
    }
    Result<std::unique_ptr<IBulkInPipe>> open_bulk_in(std::uint8_t endpoint, std::size_t max_slots) override {
        return std::unique_ptr<IBulkInPipe>(std::make_unique<LibusbBulkInPipe>(h_, endpoint, max_slots));
    }
    Result<std::vector<std::uint8_t>> control_read(std::uint8_t request, std::uint16_t value, std::uint16_t index,
                                                   std::uint16_t length) override {
        if (auto g = guard(eeprom::Direction::in, request, value, index); !g) return g.error();
        std::vector<std::uint8_t> buffer(length);
        const int n = libusb_control_transfer(h_, 0xC0, request, value, index, buffer.data(), length, kPipeTimeoutMs);
        if (n < 0) {
            return failure<std::vector<std::uint8_t>>(kind_of(n), std::format("control read 0x{:02x}/0x{:04x}/0x{:04x} failed: {}", request, value, index, text(n)));
        }
        buffer.resize(static_cast<std::size_t>(n));
        return buffer;
    }
    Result<void> control_write(std::uint8_t request, std::uint16_t value, std::uint16_t index) override {
        if (auto g = guard(eeprom::Direction::out, request, value, index); !g) return g;
        const int n = libusb_control_transfer(h_, 0x40, request, value, index, nullptr, 0, kPipeTimeoutMs);
        if (n < 0) {
            return void_failure(kind_of(n), std::format("control write 0x{:02x}/0x{:04x}/0x{:04x} failed: {}", request, value, index, text(n)));
        }
        return {};
    }
    const DeviceInfo& device_info() const override { return info_; }

private:
    DeviceInfo info_;
    libusb_device_handle* h_;
};

class LibusbLoader final : public ILoaderControl {
public:
    explicit LibusbLoader(libusb_device_handle* h) : h_(h) {}
    ~LibusbLoader() override {
        libusb_release_interface(h_, 0);
        libusb_close(h_);
    }
    VoidResult vendor_out(std::uint8_t request, std::uint16_t value, std::span<const std::uint8_t> data) override {
        if (!loader_request_allowed(0x40, request, value, data)) {
            return void_failure(ErrorKind::usb_not_supported, std::format("loader refused 0x40 0x{:02x} 0x{:04x} ({} bytes)", request, value, data.size()));
        }
        const int n = libusb_control_transfer(h_, 0x40, request, value, 0, const_cast<unsigned char*>(data.data()),
                                              static_cast<std::uint16_t>(data.size()), kPipeTimeoutMs);
        if (n < 0) return void_failure(kind_of(n), "FAIL " + text(n));
        if (static_cast<std::size_t>(n) != data.size()) return void_failure(ErrorKind::usb_short_transfer, std::format("FAIL short write {}/{}", n, data.size()));
        return {};
    }
    Result<std::vector<std::uint8_t>> vendor_in(std::uint8_t request, std::uint16_t value, std::uint16_t length) override {
        if (!loader_request_allowed(0xC0, request, value, {}, length)) {
            return failure<std::vector<std::uint8_t>>(ErrorKind::usb_not_supported, std::format("loader refused 0xC0 0x{:02x}", request));
        }
        std::vector<std::uint8_t> b(length);
        const int n = libusb_control_transfer(h_, 0xC0, request, value, 0, b.data(), length, kPipeTimeoutMs);
        if (n < 0) return failure<std::vector<std::uint8_t>>(kind_of(n), "FAIL " + text(n));
        b.resize(static_cast<std::size_t>(n));
        return b;
    }

private:
    libusb_device_handle* h_;
};

} // namespace

std::vector<DeviceInfo> enumerate() {
    std::vector<DeviceInfo> out;
    if (!context()) return out;
    libusb_device** list = nullptr;
    const auto n = libusb_get_device_list(context(), &list);
    for (ssize_t i = 0; i < n; ++i) {
        libusb_device_descriptor dd{};
        if (libusb_get_device_descriptor(list[i], &dd) == 0 && dd.idVendor == kVendorId) {
            out.push_back(describe(list[i], dd));
        }
    }
    if (n >= 0) libusb_free_device_list(list, 1);
    return out;
}

Result<std::unique_ptr<IUsbTransport>> open_first(bool cold_ok) {
    const auto devices = enumerate_pakon();
    const DeviceInfo* chosen = nullptr;
    for (const auto& d : devices) {
        if (!d.is_cold() && d.product_id == kWarmPidF135) { chosen = &d; break; }
        if (cold_ok && d.is_cold() && !chosen) chosen = &d;
    }
    if (!chosen) {
        return failure<std::unique_ptr<IUsbTransport>>(
            ErrorKind::usb_device_not_found,
            devices.empty() ? "no Pakon F-X35 device detected"
                            : "Pakon device in cold/bootstrap state (0f05:f235): firmware is not loaded (pakon-cli boot)");
    }
    auto h = open_path(chosen->device_path);
    if (!h) return h.error();
    return std::unique_ptr<IUsbTransport>(std::make_unique<LibusbTransport>(*chosen, *h));
}

Result<std::unique_ptr<ILoaderControl>> open_cold_loader() {
    for (const auto& d : enumerate_pakon()) {
        if (d.is_cold() && d.bcd_device == 0xAA07) {
            auto h = open_path(d.device_path);
            if (!h) return h.error();
            return std::unique_ptr<ILoaderControl>(std::make_unique<LibusbLoader>(*h));
        }
    }
    return failure<std::unique_ptr<ILoaderControl>>(ErrorKind::usb_device_not_found,
                                                    "no cold F-135 board (0f05:f235 REV AA07) attached");
}

} // namespace pakon::usb

#endif // !_WIN32
