// The virtual wired DualSense, as seen by the host through USB/IP.
//
// This class plays the role TinyUSB plays in the firmware: it answers the
// standard USB requests (descriptors, configuration, alternate settings),
// queues the URBs of every endpoint and completes them. Isochronous URBs are
// completed at the rate a real full-speed bus would (one packet per 1 ms
// frame), so the host's audio clock behaves exactly as with the Pico.
// Everything DualSense specific is delegated to a UsbDeviceHandler.

#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "usbip.h"

struct SetupPacket {
    uint8_t bmRequestType = 0;
    uint8_t bRequest = 0;
    uint16_t wValue = 0;
    uint16_t wIndex = 0;
    uint16_t wLength = 0;

    bool is_in() const { return bmRequestType & 0x80; }
    uint8_t type() const { return (bmRequestType >> 5) & 0x03; } // 0 std, 1 class, 2 vendor
    uint8_t recipient() const { return bmRequestType & 0x1F; }  // 0 dev, 1 itf, 2 ep
};

struct ControlReply {
    enum class Kind { kData, kStall, kPending };
    Kind kind = Kind::kStall;
    std::vector<uint8_t> data; // IN data (truncated to wLength by the device)

    static ControlReply ok(std::vector<uint8_t> d = {}) { return {Kind::kData, std::move(d)}; }
    static ControlReply stall() { return {Kind::kStall, {}}; }
    static ControlReply pending() { return {Kind::kPending, {}}; }
};

class UsbDeviceHandler {
public:
    virtual ~UsbDeviceHandler() = default;
    // Class / vendor requests on ep0. kPending means the handler will call
    // VirtualUsbDevice::complete_control(seqnum, ...) later.
    virtual ControlReply on_class_request(const SetupPacket &setup, const std::vector<uint8_t> &out_data,
                                          uint32_t seqnum) = 0;
    virtual void on_configured(bool configured) = 0;
    virtual void on_set_interface(uint8_t itf, uint8_t alt) = 0;
    // Interrupt OUT (HID output report, e.g. 0x02).
    virtual void on_hid_output(const uint8_t *data, size_t len) = 0;
    // One 1 ms isochronous packet of the speaker/haptics stream (4ch s16le).
    virtual void on_audio_out_packet(const uint8_t *data, size_t len) = 0;
    // Fill one 1 ms isochronous packet of the mic stream; returns bytes used.
    virtual size_t on_audio_in_packet(uint8_t *data, size_t max_len) = 0;
};

class VirtualUsbDevice {
public:
    VirtualUsbDevice(UsbDeviceHandler &handler, bool edge, std::string serial);
    ~VirtualUsbDevice();

    VirtualUsbDevice(const VirtualUsbDevice &) = delete;
    VirtualUsbDevice &operator=(const VirtualUsbDevice &) = delete;

    // Takes ownership of a connected, non-blocking stream socket.
    void set_socket(int fd);
    void close_socket();
    int fd() const { return fd_; }
    bool wants_write() const { return !out_.empty(); }

    // Return false when the host side went away or sent garbage.
    bool on_readable();
    bool on_writable();
    void tick(int64_t now_us);

    // New HID input report (64 bytes incl. report id 0x01).
    void send_input_report(const uint8_t *report, size_t len);
    void complete_control(uint32_t seqnum, int32_t status, const std::vector<uint8_t> &data);

    bool configured() const { return configured_; }
    uint8_t alt_setting(uint8_t itf) const { return itf < 4 ? alt_[itf] : 0; }
    const std::string &last_error() const { return error_; }

    struct Stats {
        uint64_t urbs = 0;
        uint64_t audio_out_packets = 0;
        uint64_t audio_in_packets = 0;
        uint64_t input_reports = 0;
        uint64_t output_reports = 0;
        uint64_t unlinks = 0;
    };
    const Stats &stats() const { return stats_; }

private:
    struct IsoUrb {
        usbip::Submit submit;
        size_t next_packet = 0;
    };

    void handle_submit(usbip::Submit &submit);
    void handle_unlink(const usbip::Unlink &unlink);
    void handle_control(usbip::Submit &submit);
    ControlReply standard_request(const SetupPacket &setup);
    void reply_submit(const usbip::Submit &submit, int32_t status, const std::vector<uint8_t> &in_data,
                      int32_t actual_length, const std::vector<usbip::IsoPacket> *iso = nullptr);
    void queue_bytes(const std::vector<uint8_t> &bytes);
    bool flush();
    void complete_iso_out(IsoUrb &urb);
    void complete_iso_in(IsoUrb &urb);
    void tick_iso_out(int64_t now_us);
    void tick_iso_in(int64_t now_us);

    UsbDeviceHandler &handler_;
    bool edge_;
    std::string serial_;
    int fd_ = -1;
    usbip::StreamParser parser_;
    std::vector<uint8_t> out_;
    std::string error_;

    bool configured_ = false;
    uint8_t alt_[4] = {0, 0, 0, 0};

    std::deque<usbip::Submit> hid_in_;     // pending interrupt IN URBs
    std::vector<uint8_t> latest_report_;
    bool report_dirty_ = false;

    std::deque<IsoUrb> iso_out_;
    std::deque<IsoUrb> iso_in_;
    int64_t out_next_us_ = 0;
    int64_t in_next_us_ = 0;
    int64_t now_us_ = 0;

    std::map<uint32_t, uint16_t> pending_control_; // seqnum -> wLength
    Stats stats_;
};
