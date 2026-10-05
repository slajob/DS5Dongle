#include "usbip.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace usbip {

namespace {

const char *kVhciDir = "/sys/devices/platform/vhci_hcd.0";

void set_error(std::string *why, const std::string &msg) {
    if (why) *why = msg;
}

bool write_sysfs(const std::string &path, const std::string &value, std::string *why) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        set_error(why, "open " + path + ": " + std::strerror(errno));
        return false;
    }
    const ssize_t n = ::write(fd, value.data(), value.size());
    const int err = errno;
    ::close(fd);
    if (n != static_cast<ssize_t>(value.size())) {
        set_error(why, "write " + path + " '" + value + "': " + std::strerror(err));
        return false;
    }
    return true;
}

void put_be32s(uint8_t *p, int32_t v) { put_be32(p, static_cast<uint32_t>(v)); }

} // namespace

uint32_t get_be32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 |
           static_cast<uint32_t>(p[2]) << 8 | static_cast<uint32_t>(p[3]);
}

void put_be32(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

StreamParser::Result StreamParser::next(Submit &submit, Unlink &unlink, std::string &error) {
    // Compact the buffer once everything before pos_ is consumed.
    if (pos_ > 0 && pos_ == buf_.size()) {
        buf_.clear();
        pos_ = 0;
    } else if (pos_ > 65536) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(pos_));
        pos_ = 0;
    }

    const size_t avail = buf_.size() - pos_;
    if (avail < kHeaderSize) return Result::kNeedMore;
    const uint8_t *h = buf_.data() + pos_;
    const uint32_t command = get_be32(h);

    if (command == kCmdUnlink) {
        unlink.seqnum = get_be32(h + 4);
        unlink.unlink_seqnum = get_be32(h + 20);
        pos_ += kHeaderSize;
        return Result::kUnlink;
    }
    if (command != kCmdSubmit) {
        error = "unexpected USB/IP command 0x" + std::to_string(command);
        return Result::kError;
    }

    const uint32_t direction = get_be32(h + 12);
    const uint32_t ep = get_be32(h + 16);
    const int32_t length = static_cast<int32_t>(get_be32(h + 24));
    const int32_t packets = static_cast<int32_t>(get_be32(h + 32));
    if (length < 0 || length > kMaxTransferLength) {
        error = "bad transfer_buffer_length " + std::to_string(length);
        return Result::kError;
    }
    const bool iso = is_iso_(ep, direction) && packets > 0 && packets <= kMaxIsoPackets;
    const size_t payload = direction == kDirOut ? static_cast<size_t>(length) : 0;
    const size_t iso_bytes = iso ? static_cast<size_t>(packets) * kIsoDescriptorSize : 0;
    if (avail < kHeaderSize + payload + iso_bytes) return Result::kNeedMore;

    submit = Submit{};
    submit.seqnum = get_be32(h + 4);
    submit.devid = get_be32(h + 8);
    submit.direction = direction;
    submit.ep = ep;
    submit.transfer_flags = get_be32(h + 20);
    submit.transfer_buffer_length = length;
    submit.start_frame = static_cast<int32_t>(get_be32(h + 28));
    submit.number_of_packets = iso ? packets : 0;
    submit.interval = static_cast<int32_t>(get_be32(h + 36));
    std::memcpy(submit.setup.data(), h + 40, 8);

    const uint8_t *p = h + kHeaderSize;
    if (payload) submit.data.assign(p, p + payload);
    p += payload;
    if (iso) {
        submit.iso.resize(static_cast<size_t>(packets));
        for (int32_t i = 0; i < packets; ++i, p += kIsoDescriptorSize) {
            auto &d = submit.iso[static_cast<size_t>(i)];
            d.offset = get_be32(p);
            d.length = get_be32(p + 4);
            d.actual_length = get_be32(p + 8);
            d.status = static_cast<int32_t>(get_be32(p + 12));
        }
    }
    pos_ += kHeaderSize + payload + iso_bytes;
    return Result::kSubmit;
}

std::vector<uint8_t> encode_ret_submit(uint32_t seqnum, int32_t status, const std::vector<uint8_t> &in_data,
                                       int32_t actual_length, int32_t start_frame,
                                       const std::vector<IsoPacket> *iso) {
    const size_t iso_count = iso ? iso->size() : 0;
    std::vector<uint8_t> out(kHeaderSize + in_data.size() + iso_count * kIsoDescriptorSize, 0);
    uint8_t *h = out.data();
    put_be32(h, kRetSubmit);
    put_be32(h + 4, seqnum);
    // devid, direction, ep are zero in replies.
    put_be32s(h + 20, status);
    put_be32s(h + 24, actual_length);
    put_be32s(h + 28, start_frame);
    put_be32s(h + 32, static_cast<int32_t>(iso_count));
    // error_count and padding stay zero.
    if (!in_data.empty()) std::memcpy(h + kHeaderSize, in_data.data(), in_data.size());
    uint8_t *p = h + kHeaderSize + in_data.size();
    for (size_t i = 0; i < iso_count; ++i, p += kIsoDescriptorSize) {
        const auto &d = (*iso)[i];
        put_be32(p, d.offset);
        put_be32(p + 4, d.length);
        put_be32(p + 8, d.actual_length);
        put_be32s(p + 12, d.status);
    }
    return out;
}

std::vector<uint8_t> encode_ret_unlink(uint32_t seqnum, int32_t status) {
    std::vector<uint8_t> out(kHeaderSize, 0);
    put_be32(out.data(), kRetUnlink);
    put_be32(out.data() + 4, seqnum);
    put_be32s(out.data() + 20, status);
    return out;
}

bool vhci_available(std::string *why) {
    struct stat st{};
    const std::string attach = std::string(kVhciDir) + "/attach";
    if (::stat(attach.c_str(), &st) != 0) {
        set_error(why, "vhci-hcd is not loaded (" + attach + " missing). Run: modprobe vhci-hcd");
        return false;
    }
    return true;
}

int vhci_find_free_port(std::string *why) {
    // Format (Linux >= 4.13):
    //   hub port sta spd dev      sockfd local_busid
    //   hs  0000 004 000 00000000 000000 0-0
    std::ifstream status(std::string(kVhciDir) + "/status");
    if (!status) {
        set_error(why, "cannot read vhci status");
        return -1;
    }
    std::string line;
    std::getline(status, line); // header
    while (std::getline(status, line)) {
        std::istringstream ss(line);
        std::string hub;
        unsigned port = 0, sta = 0;
        if (!(ss >> hub >> port >> sta)) continue;
        constexpr unsigned kVdevStNull = 4;
        if (hub == "hs" && sta == kVdevStNull) return static_cast<int>(port);
    }
    set_error(why, "no free vhci high-speed port");
    return -1;
}

bool vhci_attach(int port, int sockfd, uint32_t devid, int speed, std::string *why) {
    const std::string value = std::to_string(port) + " " + std::to_string(sockfd) + " " +
                              std::to_string(devid) + " " + std::to_string(speed);
    return write_sysfs(std::string(kVhciDir) + "/attach", value, why);
}

bool vhci_detach(int port, std::string *why) {
    return write_sysfs(std::string(kVhciDir) + "/detach", std::to_string(port), why);
}

bool make_loopback_pair(int &kernel_end, int &our_end, std::string *why) {
    kernel_end = our_end = -1;
    const int listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) {
        set_error(why, std::string("socket: ") + std::strerror(errno));
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    socklen_t addr_len = sizeof(addr);
    if (::bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
        ::listen(listener, 1) != 0 ||
        ::getsockname(listener, reinterpret_cast<sockaddr *>(&addr), &addr_len) != 0) {
        set_error(why, std::string("loopback listen: ") + std::strerror(errno));
        ::close(listener);
        return false;
    }
    const int client = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (client < 0 || ::connect(client, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        set_error(why, std::string("loopback connect: ") + std::strerror(errno));
        if (client >= 0) ::close(client);
        ::close(listener);
        return false;
    }
    const int server = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    ::close(listener);
    if (server < 0) {
        set_error(why, std::string("loopback accept: ") + std::strerror(errno));
        ::close(client);
        return false;
    }
    const int one = 1;
    ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    ::setsockopt(server, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    kernel_end = client;
    our_end = server;
    return true;
}

} // namespace usbip
