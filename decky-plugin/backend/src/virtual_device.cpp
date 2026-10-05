#include "virtual_device.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

#include "log.h"
#include "usb_descriptors.h"

namespace {

constexpr int32_t kEpipe = -32;       // -EPIPE: stall
constexpr int32_t kEconnreset = -104; // -ECONNRESET: unlinked
constexpr int64_t kFrameUs = 1000;    // full speed frame
constexpr int64_t kMaxCatchUpUs = 4000;

bool endpoint_is_iso(uint32_t ep, uint32_t direction) {
    return (ep == (usbdesc::kEpAudioOut & 0x0F) && direction == usbip::kDirOut) ||
           (ep == (usbdesc::kEpAudioIn & 0x0F) && direction == usbip::kDirIn);
}

SetupPacket parse_setup(const std::array<uint8_t, 8> &raw) {
    SetupPacket s;
    s.bmRequestType = raw[0];
    s.bRequest = raw[1];
    s.wValue = static_cast<uint16_t>(raw[2] | raw[3] << 8);
    s.wIndex = static_cast<uint16_t>(raw[4] | raw[5] << 8);
    s.wLength = static_cast<uint16_t>(raw[6] | raw[7] << 8);
    return s;
}

} // namespace

VirtualUsbDevice::VirtualUsbDevice(UsbDeviceHandler &handler, bool edge, std::string serial)
    : handler_(handler), edge_(edge), serial_(std::move(serial)), parser_(&endpoint_is_iso) {}

VirtualUsbDevice::~VirtualUsbDevice() { close_socket(); }

void VirtualUsbDevice::set_socket(int fd) {
    close_socket();
    fd_ = fd;
    parser_ = usbip::StreamParser(&endpoint_is_iso);
    out_.clear();
    error_.clear();
    configured_ = false;
    std::fill(std::begin(alt_), std::end(alt_), 0);
    hid_in_.clear();
    iso_out_.clear();
    iso_in_.clear();
    pending_control_.clear();
    report_dirty_ = false;
}

void VirtualUsbDevice::close_socket() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    out_.clear();
}

bool VirtualUsbDevice::on_readable() {
    uint8_t buf[65536];
    while (true) {
        const ssize_t n = ::recv(fd_, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) {
            parser_.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) {
            error_ = "USB/IP connection closed by host";
            return false;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        error_ = std::string("USB/IP recv: ") + std::strerror(errno);
        return false;
    }

    usbip::Submit submit;
    usbip::Unlink unlink;
    while (true) {
        std::string error;
        const auto result = parser_.next(submit, unlink, error);
        if (result == usbip::StreamParser::Result::kNeedMore) break;
        if (result == usbip::StreamParser::Result::kError) {
            error_ = error;
            return false;
        }
        if (result == usbip::StreamParser::Result::kSubmit) {
            handle_submit(submit);
        } else {
            handle_unlink(unlink);
        }
    }
    return flush();
}

bool VirtualUsbDevice::on_writable() { return flush(); }

void VirtualUsbDevice::queue_bytes(const std::vector<uint8_t> &bytes) {
    out_.insert(out_.end(), bytes.begin(), bytes.end());
}

bool VirtualUsbDevice::flush() {
    size_t sent = 0;
    while (sent < out_.size() && fd_ >= 0) {
        const ssize_t n = ::send(fd_, out_.data() + sent, out_.size() - sent, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        error_ = std::string("USB/IP send: ") + std::strerror(errno);
        out_.clear();
        return false;
    }
    out_.erase(out_.begin(), out_.begin() + static_cast<long>(sent));
    return true;
}

void VirtualUsbDevice::reply_submit(const usbip::Submit &submit, int32_t status, const std::vector<uint8_t> &in_data,
                                    int32_t actual_length, const std::vector<usbip::IsoPacket> *iso) {
    queue_bytes(usbip::encode_ret_submit(submit.seqnum, status, in_data, actual_length, submit.start_frame, iso));
}

void VirtualUsbDevice::handle_submit(usbip::Submit &submit) {
    ++stats_.urbs;
    const uint8_t ep = static_cast<uint8_t>(submit.ep & 0x0F);
    const bool in = submit.direction == usbip::kDirIn;

    if (ep == 0) {
        handle_control(submit);
        return;
    }
    if (ep == (usbdesc::kEpHidIn & 0x0F) && in) {
        if (report_dirty_) {
            const size_t n = std::min(latest_report_.size(), static_cast<size_t>(submit.transfer_buffer_length));
            std::vector<uint8_t> data(latest_report_.begin(), latest_report_.begin() + static_cast<long>(n));
            reply_submit(submit, 0, data, static_cast<int32_t>(n));
            report_dirty_ = false;
            ++stats_.input_reports;
        } else {
            hid_in_.push_back(std::move(submit));
        }
        return;
    }
    if (ep == usbdesc::kEpHidOut && !in) {
        ++stats_.output_reports;
        handler_.on_hid_output(submit.data.data(), submit.data.size());
        reply_submit(submit, 0, {}, static_cast<int32_t>(submit.data.size()));
        return;
    }
    if (ep == usbdesc::kEpAudioOut && !in) {
        if (submit.iso.empty()) {
            reply_submit(submit, 0, {}, 0, &submit.iso);
            return;
        }
        if (iso_out_.empty() && out_next_us_ < now_us_ - kMaxCatchUpUs) out_next_us_ = now_us_;
        iso_out_.push_back(IsoUrb{std::move(submit), 0});
        return;
    }
    if (ep == (usbdesc::kEpAudioIn & 0x0F) && in) {
        if (submit.iso.empty()) {
            reply_submit(submit, 0, {}, 0, &submit.iso);
            return;
        }
        if (iso_in_.empty() && in_next_us_ < now_us_ - kMaxCatchUpUs) in_next_us_ = now_us_;
        iso_in_.push_back(IsoUrb{std::move(submit), 0});
        return;
    }

    LOGW("URB for unknown endpoint %u (%s), stalling", ep, in ? "IN" : "OUT");
    reply_submit(submit, kEpipe, {}, 0);
}

void VirtualUsbDevice::handle_unlink(const usbip::Unlink &unlink) {
    ++stats_.unlinks;
    const uint32_t target = unlink.unlink_seqnum;

    auto erase_iso = [target](std::deque<IsoUrb> &queue) {
        const auto it = std::find_if(queue.begin(), queue.end(),
                                     [target](const IsoUrb &u) { return u.submit.seqnum == target; });
        if (it == queue.end()) return false;
        queue.erase(it);
        return true;
    };
    auto erase_hid = [this, target]() {
        const auto it = std::find_if(hid_in_.begin(), hid_in_.end(),
                                     [target](const usbip::Submit &s) { return s.seqnum == target; });
        if (it == hid_in_.end()) return false;
        hid_in_.erase(it);
        return true;
    };

    const bool found = erase_hid() || erase_iso(iso_out_) || erase_iso(iso_in_) ||
                       pending_control_.erase(target) > 0;
    // -ECONNRESET: we dropped it; 0: it already completed (the RET_SUBMIT is
    // on its way), which is what the kernel expects in that race.
    queue_bytes(usbip::encode_ret_unlink(unlink.seqnum, found ? kEconnreset : 0));
}

void VirtualUsbDevice::handle_control(usbip::Submit &submit) {
    const SetupPacket setup = parse_setup(submit.setup);
    ControlReply reply;
    if (setup.type() == 0) {
        reply = standard_request(setup);
    } else {
        reply = handler_.on_class_request(setup, submit.data, submit.seqnum);
    }

    switch (reply.kind) {
        case ControlReply::Kind::kStall:
            LOGD("stall control bmRequestType=0x%02X bRequest=0x%02X wValue=0x%04X wIndex=0x%04X wLength=%u",
                 setup.bmRequestType, setup.bRequest, setup.wValue, setup.wIndex, setup.wLength);
            reply_submit(submit, kEpipe, {}, 0);
            break;
        case ControlReply::Kind::kPending:
            pending_control_[submit.seqnum] = setup.wLength;
            break;
        case ControlReply::Kind::kData:
            if (setup.is_in()) {
                auto &data = reply.data;
                const size_t n = std::min<size_t>({data.size(), setup.wLength,
                                                   static_cast<size_t>(submit.transfer_buffer_length)});
                data.resize(n);
                reply_submit(submit, 0, data, static_cast<int32_t>(n));
            } else {
                reply_submit(submit, 0, {}, static_cast<int32_t>(submit.data.size()));
            }
            break;
    }
}

void VirtualUsbDevice::complete_control(uint32_t seqnum, int32_t status, const std::vector<uint8_t> &data) {
    auto it = pending_control_.find(seqnum);
    if (it == pending_control_.end()) return; // unlinked meanwhile
    const uint16_t w_length = it->second;
    pending_control_.erase(it);
    usbip::Submit submit;
    submit.seqnum = seqnum;
    if (status != 0) {
        reply_submit(submit, status, {}, 0);
    } else {
        std::vector<uint8_t> d(data.begin(), data.begin() + static_cast<long>(std::min<size_t>(data.size(), w_length)));
        reply_submit(submit, 0, d, static_cast<int32_t>(d.size()));
    }
    flush();
}

ControlReply VirtualUsbDevice::standard_request(const SetupPacket &setup) {
    constexpr uint8_t kGetStatus = 0x00, kClearFeature = 0x01, kSetFeature = 0x03, kSetAddress = 0x05,
                      kGetDescriptor = 0x06, kGetConfiguration = 0x08, kSetConfiguration = 0x09,
                      kGetInterface = 0x0A, kSetInterface = 0x0B;
    const uint8_t recipient = setup.recipient();

    switch (setup.bRequest) {
        case kGetStatus:
            return ControlReply::ok({static_cast<uint8_t>(recipient == 0 ? 0x01 : 0x00), 0x00});
        case kClearFeature:
        case kSetFeature:
        case kSetAddress:
            return ControlReply::ok();
        case kGetConfiguration:
            return ControlReply::ok({static_cast<uint8_t>(configured_ ? 1 : 0)});
        case kSetConfiguration:
            configured_ = (setup.wValue & 0xFF) != 0;
            std::fill(std::begin(alt_), std::end(alt_), 0);
            iso_out_.clear();
            iso_in_.clear();
            LOGI("host %s the virtual controller", configured_ ? "configured" : "deconfigured");
            handler_.on_configured(configured_);
            return ControlReply::ok();
        case kGetInterface:
            if (setup.wIndex >= 4) return ControlReply::stall();
            return ControlReply::ok({alt_[setup.wIndex]});
        case kSetInterface: {
            const uint8_t itf = static_cast<uint8_t>(setup.wIndex & 0xFF);
            const uint8_t alt = static_cast<uint8_t>(setup.wValue & 0xFF);
            if (itf >= 4) return ControlReply::stall();
            if ((itf == usbdesc::kItfAudioOut || itf == usbdesc::kItfAudioIn) ? alt > 1 : alt != 0) {
                return ControlReply::stall();
            }
            alt_[itf] = alt;
            handler_.on_set_interface(itf, alt);
            return ControlReply::ok();
        }
        case kGetDescriptor: {
            const uint8_t type = static_cast<uint8_t>(setup.wValue >> 8);
            const uint8_t index = static_cast<uint8_t>(setup.wValue & 0xFF);
            if (recipient == 0) {
                switch (type) {
                    case 0x01: {
                        auto d = usbdesc::device_descriptor(edge_ ? 0x0DF2 : 0x0CE6);
                        d[16] = serial_.empty() ? 0 : 3;
                        return ControlReply::ok(std::move(d));
                    }
                    case 0x02:
                        return ControlReply::ok(usbdesc::configuration_descriptor(edge_));
                    case 0x03: {
                        auto d = usbdesc::string_descriptor(index, edge_, serial_);
                        if (d.empty()) return ControlReply::stall();
                        return ControlReply::ok(std::move(d));
                    }
                    default:
                        return ControlReply::stall(); // device qualifier / BOS / other speed
                }
            }
            if (recipient == 1 && setup.wIndex == usbdesc::kItfHid) {
                if (type == 0x22) return ControlReply::ok(usbdesc::hid_report_descriptor(edge_));
                if (type == 0x21) return ControlReply::ok(usbdesc::hid_descriptor(edge_));
            }
            return ControlReply::stall();
        }
        default:
            return ControlReply::stall();
    }
}

void VirtualUsbDevice::send_input_report(const uint8_t *report, size_t len) {
    latest_report_.assign(report, report + len);
    if (!hid_in_.empty()) {
        usbip::Submit submit = std::move(hid_in_.front());
        hid_in_.pop_front();
        const size_t n = std::min(len, static_cast<size_t>(submit.transfer_buffer_length));
        reply_submit(submit, 0, std::vector<uint8_t>(report, report + n), static_cast<int32_t>(n));
        report_dirty_ = false;
        ++stats_.input_reports;
        flush();
    } else {
        report_dirty_ = true;
    }
}

void VirtualUsbDevice::complete_iso_out(IsoUrb &urb) {
    int32_t total = 0;
    for (const auto &p : urb.submit.iso) total += static_cast<int32_t>(p.actual_length);
    reply_submit(urb.submit, 0, {}, total, &urb.submit.iso);
}

void VirtualUsbDevice::complete_iso_in(IsoUrb &urb) {
    // urb.submit.data holds the packed packet payloads (USB/IP sends only the
    // actual bytes, the host re-spreads them using the descriptors' offsets).
    reply_submit(urb.submit, 0, urb.submit.data, static_cast<int32_t>(urb.submit.data.size()), &urb.submit.iso);
}

void VirtualUsbDevice::tick_iso_out(int64_t now_us) {
    while (!iso_out_.empty() && out_next_us_ <= now_us) {
        IsoUrb &urb = iso_out_.front();
        auto &packet = urb.submit.iso[urb.next_packet];
        const size_t end = static_cast<size_t>(packet.offset) + packet.length;
        if (end <= urb.submit.data.size()) {
            handler_.on_audio_out_packet(urb.submit.data.data() + packet.offset, packet.length);
            packet.actual_length = packet.length;
            packet.status = 0;
        } else {
            packet.actual_length = 0;
            packet.status = kEpipe;
        }
        ++stats_.audio_out_packets;
        out_next_us_ += kFrameUs;
        if (++urb.next_packet == urb.submit.iso.size()) {
            complete_iso_out(urb);
            iso_out_.pop_front();
        }
    }
    if (iso_out_.empty() && out_next_us_ < now_us - kMaxCatchUpUs) out_next_us_ = now_us;
}

void VirtualUsbDevice::tick_iso_in(int64_t now_us) {
    uint8_t buf[1024];
    while (!iso_in_.empty() && in_next_us_ <= now_us) {
        IsoUrb &urb = iso_in_.front();
        if (urb.next_packet == 0) urb.submit.data.clear();
        auto &packet = urb.submit.iso[urb.next_packet];
        const size_t max_len = std::min<size_t>(packet.length, sizeof(buf));
        const size_t n = handler_.on_audio_in_packet(buf, max_len);
        urb.submit.data.insert(urb.submit.data.end(), buf, buf + n);
        packet.actual_length = static_cast<uint32_t>(n);
        packet.status = 0;
        ++stats_.audio_in_packets;
        in_next_us_ += kFrameUs;
        if (++urb.next_packet == urb.submit.iso.size()) {
            complete_iso_in(urb);
            iso_in_.pop_front();
        }
    }
    if (iso_in_.empty() && in_next_us_ < now_us - kMaxCatchUpUs) in_next_us_ = now_us;
}

void VirtualUsbDevice::tick(int64_t now_us) {
    now_us_ = now_us;
    if (fd_ < 0) return;
    tick_iso_out(now_us);
    tick_iso_in(now_us);
    if (!out_.empty()) flush();
}
