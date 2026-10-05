// Minimal USB/IP device-side ("stub") protocol and vhci-hcd attach helpers.
//
// The kernel's vhci-hcd driver turns a connected stream socket into a USB
// port: every URB the host submits to the virtual device arrives as a
// USBIP_CMD_SUBMIT on the socket and is completed by our USBIP_RET_SUBMIT.
// This lets the daemon present a real USB device (with snd-usb-audio and
// usbhid binding to it) without any custom kernel module.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace usbip {

constexpr uint32_t kCmdSubmit = 0x00000001;
constexpr uint32_t kCmdUnlink = 0x00000002;
constexpr uint32_t kRetSubmit = 0x00000003;
constexpr uint32_t kRetUnlink = 0x00000004;

constexpr uint32_t kDirOut = 0;
constexpr uint32_t kDirIn = 1;

constexpr size_t kHeaderSize = 48;
constexpr size_t kIsoDescriptorSize = 16;
constexpr int32_t kMaxIsoPackets = 1024;
constexpr int32_t kMaxTransferLength = 1 << 20;

// Linux USB speeds (enum usb_device_speed).
constexpr int kSpeedFull = 2;

struct IsoPacket {
    uint32_t offset = 0;
    uint32_t length = 0;
    uint32_t actual_length = 0;
    int32_t status = 0;
};

struct Submit {
    uint32_t seqnum = 0;
    uint32_t devid = 0;
    uint32_t direction = 0;
    uint32_t ep = 0;
    uint32_t transfer_flags = 0;
    int32_t transfer_buffer_length = 0;
    int32_t start_frame = 0;
    int32_t number_of_packets = 0;
    int32_t interval = 0;
    std::array<uint8_t, 8> setup{};
    std::vector<uint8_t> data;   // OUT payload (whole transfer buffer)
    std::vector<IsoPacket> iso;  // iso descriptors (iso endpoints only)
};

struct Unlink {
    uint32_t seqnum = 0;
    uint32_t unlink_seqnum = 0;
};

// Incremental parser for the host->device byte stream.
class StreamParser {
public:
    // is_iso(ep, direction) tells the parser whether an endpoint is
    // isochronous, i.e. whether iso descriptors follow the payload.
    using IsoPredicate = bool (*)(uint32_t ep, uint32_t direction);

    explicit StreamParser(IsoPredicate is_iso) : is_iso_(is_iso) {}

    void append(const uint8_t *data, size_t len) { buf_.insert(buf_.end(), data, data + len); }

    enum class Result { kNeedMore, kSubmit, kUnlink, kError };

    // Extracts the next complete message, if any.
    Result next(Submit &submit, Unlink &unlink, std::string &error);

private:
    IsoPredicate is_iso_;
    std::vector<uint8_t> buf_;
    size_t pos_ = 0;
};

std::vector<uint8_t> encode_ret_submit(uint32_t seqnum, int32_t status, const std::vector<uint8_t> &in_data,
                                       int32_t actual_length, int32_t start_frame,
                                       const std::vector<IsoPacket> *iso);
std::vector<uint8_t> encode_ret_unlink(uint32_t seqnum, int32_t status);

// Helpers used by the test client and the parser.
uint32_t get_be32(const uint8_t *p);
void put_be32(uint8_t *p, uint32_t v);

// ----------------------------------------------------------------------------
// vhci-hcd attach/detach.
// ----------------------------------------------------------------------------
struct VhciPort {
    int port = -1;
};

// True when /sys/devices/platform/vhci_hcd.0 exists.
bool vhci_available(std::string *why);
// Finds a free high-speed-hub port (full speed devices live on the USB2 hub).
int vhci_find_free_port(std::string *why);
// Hands sockfd to the kernel. On success the kernel owns the socket.
bool vhci_attach(int port, int sockfd, uint32_t devid, int speed, std::string *why);
bool vhci_detach(int port, std::string *why);

// Creates a connected TCP loopback pair (exactly what usbip(8) gives the
// kernel). Returns false on failure.
bool make_loopback_pair(int &kernel_end, int &our_end, std::string *why);

} // namespace usbip
