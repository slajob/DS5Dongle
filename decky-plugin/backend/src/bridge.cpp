#include "bridge.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <spawn.h>
#include <sys/socket.h>
#include <unistd.h>

#include "json.h"
#include "log.h"
#include "usb_descriptors.h"
#include "usbip.h"

extern char **environ;

namespace {

constexpr int32_t kEpipe = -32;
constexpr int64_t kAttachTimeoutUs = 3'000'000;
constexpr int64_t kRetryDelayUs = 3'000'000;
constexpr int kMaxAttachFailures = 5;
constexpr uint32_t kUsbipDevid = (1u << 16) | 2u;

// DualSense (non-Edge) firmware info, used when an Edge is presented as a
// plain DualSense. Copied from the firmware's src/fake_ds5.h (report20).
const uint8_t kFakeDs5Report20[63] = {
    0x4a, 0x75, 0x6c, 0x20, 0x20, 0x34, 0x20, 0x32, 0x30, 0x32, 0x35, 0x31, 0x30, 0x3a, 0x33, 0x38,
    0x3a, 0x34, 0x30, 0x03, 0x00, 0x0b, 0x00, 0x11, 0x08, 0x00, 0x00, 0x2a, 0x00, 0x10, 0x01, 0x00,
    0x28, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x06, 0x00, 0x00, 0x01,
    0x00, 0x03, 0x00, 0x10, 0x10, 0x03, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

const uint8_t kPrefetchReports[] = {0x09, 0x20, 0x22, 0x05}; // init_feature() order

// UAC1 request codes.
constexpr uint8_t kSetCur = 0x01, kGetCur = 0x81, kGetMin = 0x82, kGetMax = 0x83, kGetRes = 0x84;
constexpr uint8_t kFuMute = 0x01, kFuVolume = 0x02;

int connect_tcp(const std::string &host_port, std::string *error) {
    const auto colon = host_port.rfind(':');
    if (colon == std::string::npos) {
        if (error) *error = "bad host:port '" + host_port + "'";
        return -1;
    }
    const std::string host = host_port.substr(0, colon);
    const std::string port = host_port.substr(colon + 1);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;
    if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
        if (error) *error = "cannot resolve " + host_port;
        return -1;
    }
    const int fd = ::socket(res->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    const bool ok = fd >= 0 && ::connect(fd, res->ai_addr, res->ai_addrlen) == 0;
    ::freeaddrinfo(res);
    if (!ok) {
        if (error) *error = "connect " + host_port + ": " + std::strerror(errno);
        if (fd >= 0) ::close(fd);
        return -1;
    }
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

void set_nonblocking(int fd) { ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK); }

} // namespace

Bridge::Bridge(std::unique_ptr<BtLink> link, const BridgeConfig &config, BridgeOptions options)
    : link_(std::move(link)), cfg_(config), options_(std::move(options)) {}

Bridge::~Bridge() {
    detach_usb();
    worker_.reset(); // joins before the link goes away
}

bool Bridge::init(std::string *error) {
    if (!audio_out_.init(error) || !mic_.init(error)) return false;
    worker_ = std::make_unique<FeatureWorker>(*link_);
    LOGI("bridging %s (%s, %s) from %s", info().name.c_str(), info().mac.c_str(),
         info().product_id == ds5::kDualSenseEdgePid ? "Edge" : "DualSense", info().hidraw_path.c_str());

    // What the firmware does when the HID interrupt channel opens: fetch the
    // feature reports the host will ask for, set the light bar / audio
    // routing, and (re-)arm mic streaming.
    for (const uint8_t id : kPrefetchReports) request_feature(id, 0, false);
    send_initial_state();
    update_mic_status();
    return true;
}

// ---------------------------------------------------------------------------
// Bluetooth side
// ---------------------------------------------------------------------------
bool Bridge::on_bt_readable(int64_t now_us) {
    uint8_t buf[1024];
    while (true) {
        const ssize_t n = link_->read_input(buf, sizeof(buf));
        if (n < 0) return false;
        if (n == 0) return true;
        handle_input_report(buf, static_cast<size_t>(n), now_us);
    }
}

void Bridge::handle_input_report(const uint8_t *r, size_t len, int64_t now_us) {
    // Port of on_bt_data() (src/main.cpp). hidraw strips the 0xA1 header, so
    // firmware offset data[i] is r[i - 1] here.
    if (len < 3 || r[0] != ds5::kBtInputReportId) return;
    ++stats_.bt_input_reports;

    // Mic audio: bit1 of the tag byte; the Opus frame follows at offset 3.
    if ((r[1] >> 1) & 1) {
        if (mic_enabled() && len > 3) {
            mic_.push_opus(r + 3, len - 3);
            ++stats_.mic_frames;
        }
        return;
    }
    if (len < 2 + ds5::kUsbInputBodySize) return;
    const uint8_t *body = r + 2;

    const uint8_t plugged = body[ds5::kBodyPlugged];
    const uint8_t previous = body_[ds5::kBodyPlugged];
    if ((plugged & 1) != (previous & 1) || !have_input_) {
        headset_ = plugged & 1;
    }
    if (((plugged >> 2) & 1) != ((previous >> 2) & 1)) {
        // On Bluetooth the controller does not drive its own mute LED.
        ds5::SetStateData state{};
        state.AllowMuteLight = 1;
        state.MuteLightMode = ((plugged >> 2) & 1) ? ds5::MuteLight::On : ds5::MuteLight::Off;
        update_state(state);
    }

    // Idle detection, same thresholds as the firmware's l2cap handler.
    if (r[1] & 1) {
        const bool idle = body[0] >= 120 && body[0] <= 140 && body[1] >= 120 && body[1] <= 140 &&
                          body[2] >= 120 && body[2] <= 140 && body[3] >= 120 && body[3] <= 140 &&
                          body[4] == 0 && body[5] == 0 && body[ds5::kBodyButtons0] == 0x08 &&
                          body[ds5::kBodyButtons1] == 0x00 && body[ds5::kBodyButtons2] == 0x00;
        if (!idle || last_activity_us_ == 0) last_activity_us_ = now_us;
    }

    std::memcpy(body_, body, ds5::kUsbInputBodySize);
    have_input_ = true;

    if (usb_ && usb_->fd() >= 0) {
        uint8_t report[1 + ds5::kUsbInputBodySize];
        report[0] = ds5::kUsbInputReportId;
        std::memcpy(report + 1, body, ds5::kUsbInputBodySize);
        usb_->send_input_report(report, sizeof(report));
    }
}

void Bridge::bt_write(uint8_t *report, size_t len) {
    ds5::fill_output_report_checksum(report, len);
    if (!link_->write_output(report, len)) ++stats_.write_errors;
}

void Bridge::update_state(const ds5::SetStateData &state) {
    uint8_t pkt[ds5::kBtStateReportSize]{};
    pkt[0] = ds5::kBtStateReportId;
    pkt[1] = 0x10;
    pkt[2] = 0x90;
    pkt[3] = 0x3F;
    std::memcpy(pkt + 4, &state, sizeof(state));
    bt_write(pkt, sizeof(pkt));
}

void Bridge::update_mic_status() {
    uint8_t pkt[ds5::kBtStateReportSize]{};
    pkt[0] = ds5::kBtStateReportId;
    pkt[1] = static_cast<uint8_t>(seq_ << 4);
    seq_ = (seq_ + 1) & 0x0F;
    pkt[2] = 0x11 | 0 << 6 | 1 << 7;
    pkt[3] = 1;
    pkt[4] = mic_enabled() ? 0b00000011 : 0b00000010;
    bt_write(pkt, sizeof(pkt));
}

void Bridge::send_initial_state() {
    ds5::SetStateData state{};
    state.AllowAudioControl = 1;
    state.AllowLedColor = 1;
    state.MicSelect = cfg_.mic_select;
    state.NoiseCancelEnable = 1;
    state.AllowLightBrightnessChange = 1;
    state.AllowColorLightFadeAnimation = 1;
    state.LightFadeAnimation = ds5::LightFadeAnimation::FadeOut;
    state.LightBrightness = ds5::LightBrightness::Bright;
    state.LedRed = cfg_.led_red;
    state.LedGreen = cfg_.led_green;
    state.LedBlue = cfg_.led_blue;
    update_state(state);
}

void Bridge::apply_config_state() {
    // Same report set_config() sends when the web config changes.
    ds5::SetStateData state{};
    if (cfg_.trigger_reduce > 0) {
        state.AllowMotorPowerLevel = 1;
        state.TriggerMotorPowerReduction = cfg_.trigger_reduce;
    }
    if (cfg_.speaker_gain > 0) {
        state.AllowAudioControl2 = 1;
        state.SpeakerCompPreGain = cfg_.speaker_gain;
    }
    state.AllowAudioControl = 1;
    state.MicSelect = cfg_.mic_select;
    state.NoiseCancelEnable = 1;
    update_state(state);
}

bool Bridge::mic_enabled() const { return mic_active_ && cfg_.mic_select != 3; }
bool Bridge::speaker_enabled() const { return cfg_.speaker_select != 3; }

// ---------------------------------------------------------------------------
// Host -> controller
// ---------------------------------------------------------------------------
void Bridge::on_hid_output(const uint8_t *data, size_t len) {
    if (len >= 2 && data[0] == ds5::kUsbOutputReportId) handle_usb_output_report(data + 1, len - 1);
}

void Bridge::handle_usb_output_report(const uint8_t *payload, size_t len) {
    // Port of tud_hid_set_report_cb(): USB output report 0x02 -> BT 0x31.
    uint8_t out[ds5::kBtOutputReportSize]{};
    out[0] = ds5::kBtOutputReportId;
    out[1] = static_cast<uint8_t>(seq_ << 4);
    seq_ = (seq_ + 1) & 0x0F;
    out[2] = 0x10;

    ds5::SetStateData state{};
    std::memcpy(&state, payload, std::min(len, sizeof(state)));
    if (cfg_.trigger_reduce > 0) {
        state.AllowMotorPowerLevel = 1;
        state.TriggerMotorPowerReduction = cfg_.trigger_reduce;
    }
    if (cfg_.speaker_gain > 0) {
        state.AllowAudioControl2 = 1;
        state.SpeakerCompPreGain = cfg_.speaker_gain;
    }
    if (cfg_.mic_select != 0) {
        state.AllowAudioControl = 1;
        state.MicSelect = cfg_.mic_select;
        state.NoiseCancelEnable = 1;
    }
    if (cfg_.lock_volume) {
        state.AllowHeadphoneVolume = 0;
        state.AllowMicVolume = 0;
        state.AllowSpeakerVolume = 0;
        state.AllowAudioMute = 0;
        state.AllowMuteLight = 0;
    }
    std::memcpy(out + 3, &state, sizeof(state));
    bt_write(out, sizeof(out));
    ++stats_.bt_output_reports;
}

void Bridge::on_audio_out_packet(const uint8_t *data, size_t len) {
    constexpr size_t kFrameBytes = 4 * sizeof(int16_t);
    int16_t samples[64 * 4];
    const size_t frames = std::min(len / kFrameBytes, sizeof(samples) / kFrameBytes);
    if (frames == 0) return;
    std::memcpy(samples, data, frames * kFrameBytes);
    audio_out_.feed(samples, frames, cfg_.haptics_gain, speaker_enabled());
    send_audio_reports();
}

void Bridge::send_audio_reports() {
    // Port of audio_bt_task(): one 0x39 report carries 2 haptics chunks and
    // (unless the speaker is disabled) 2 Opus packets.
    const bool speaker = speaker_enabled();
    while (audio_out_.report_ready(speaker)) {
        AudioOutPipeline::HapticsChunk haptics[2];
        AudioOutPipeline::SpeakerPacket opus[2];
        audio_out_.take(haptics, opus, speaker);

        uint8_t pkt[ds5::kBtAudioReportSize]{};
        pkt[0] = ds5::kBtAudioReportId;
        pkt[1] = static_cast<uint8_t>(seq_ << 4);
        seq_ = (seq_ + 1) & 0x0F;
        pkt[2] = 0x11 | 0 << 6 | 1 << 7;
        pkt[3] = 6;
        pkt[4] = mic_enabled() ? 0b01111111 : 0b01111110;
        const uint8_t buf_len = cfg_.audio_buffer_length;
        pkt[5] = buf_len;
        pkt[6] = buf_len;
        pkt[7] = buf_len;
        pkt[8] = buf_len;
        packet_counter_ = static_cast<uint8_t>(packet_counter_ + 2);
        pkt[9] = packet_counter_;
        pkt[10] = 0x12 | 1 << 6 | 1 << 7;
        pkt[11] = AudioOutPipeline::kHapticsChunk;
        std::memcpy(pkt + 12, haptics[0].data(), AudioOutPipeline::kHapticsChunk);
        std::memcpy(pkt + 12 + AudioOutPipeline::kHapticsChunk, haptics[1].data(), AudioOutPipeline::kHapticsChunk);
        if (speaker) {
            const bool headphones = cfg_.speaker_select == 2 || (cfg_.speaker_select == 0 && headset_);
            pkt[140] = static_cast<uint8_t>((headphones ? 0x16 : 0x13) | 1 << 6 | 1 << 7);
            pkt[141] = AudioOutPipeline::kSpeakerPacket;
            std::memcpy(pkt + 142, opus[0].data(), AudioOutPipeline::kSpeakerPacket);
            std::memcpy(pkt + 142 + AudioOutPipeline::kSpeakerPacket, opus[1].data(),
                        AudioOutPipeline::kSpeakerPacket);
        }
        bt_write(pkt, sizeof(pkt));
        ++stats_.audio_reports;
    }
}

size_t Bridge::on_audio_in_packet(uint8_t *data, size_t max_len) {
    if (mic_enabled()) return mic_.fill_usb_packet(data, max_len);
    const size_t n = std::min<size_t>(192, max_len);
    std::memset(data, 0, n);
    return n;
}

void Bridge::on_configured(bool configured) {
    configured_ = configured;
    if (!configured) {
        speaker_stream_ = false;
        if (mic_active_) {
            mic_active_ = false;
            update_mic_status();
        }
    }
}

void Bridge::on_set_interface(uint8_t itf, uint8_t alt) {
    if (itf == usbdesc::kItfAudioOut) {
        LOGI("speaker/haptics stream %s", alt ? "started" : "stopped");
        speaker_stream_ = alt != 0;
        audio_out_.reset();
    } else if (itf == usbdesc::kItfAudioIn) {
        LOGI("microphone stream %s", alt ? "started" : "stopped");
        mic_active_ = alt != 0;
        mic_.reset();
        update_mic_status();
    }
}

// ---------------------------------------------------------------------------
// Control requests
// ---------------------------------------------------------------------------
ControlReply Bridge::on_class_request(const SetupPacket &setup, const std::vector<uint8_t> &out_data,
                                      uint32_t seqnum) {
    if (setup.type() != 1) return ControlReply::stall();
    if (setup.recipient() == 1) {
        const uint8_t itf = static_cast<uint8_t>(setup.wIndex & 0xFF);
        if (itf == usbdesc::kItfHid) return hid_class_request(setup, out_data, seqnum);
        if (itf == usbdesc::kItfAudioControl) return audio_control_request(setup, out_data);
        return ControlReply::stall();
    }
    if (setup.recipient() == 2) {
        // Audio streaming endpoints: sampling frequency control. Only 48 kHz exists.
        const uint8_t selector = static_cast<uint8_t>(setup.wValue >> 8);
        if (selector != 0x01) return ControlReply::stall();
        if (setup.bRequest == kSetCur) return ControlReply::ok();
        if (setup.bRequest == kGetCur) return ControlReply::ok({0x80, 0xBB, 0x00});
    }
    return ControlReply::stall();
}

ControlReply Bridge::hid_class_request(const SetupPacket &setup, const std::vector<uint8_t> &out_data,
                                       uint32_t seqnum) {
    constexpr uint8_t kGetReport = 0x01, kGetIdle = 0x02, kGetProtocol = 0x03, kSetReport = 0x09,
                      kSetIdle = 0x0A, kSetProtocol = 0x0B;
    const uint8_t type = static_cast<uint8_t>(setup.wValue >> 8);
    const uint8_t id = static_cast<uint8_t>(setup.wValue & 0xFF);

    switch (setup.bRequest) {
        case kGetReport:
            if (type == 3) { // feature
                if (id == 0x20 && real_is_edge_ && !present_as_edge_) {
                    // Edge presented as a plain DualSense: report DualSense firmware info.
                    std::vector<uint8_t> fake(1 + sizeof(kFakeDs5Report20));
                    fake[0] = 0x20;
                    std::memcpy(fake.data() + 1, kFakeDs5Report20, sizeof(kFakeDs5Report20));
                    return ControlReply::ok(std::move(fake));
                }
                const auto it = features_.find(id);
                if (it != features_.end()) return ControlReply::ok(it->second);
                request_feature(id, seqnum, true);
                return ControlReply::pending();
            }
            if (type == 1 && id == ds5::kUsbInputReportId) {
                std::vector<uint8_t> report(1 + ds5::kUsbInputBodySize);
                report[0] = id;
                std::memcpy(report.data() + 1, body_, ds5::kUsbInputBodySize);
                return ControlReply::ok(std::move(report));
            }
            return ControlReply::stall();
        case kSetReport:
            if (type == 2 && id == ds5::kUsbOutputReportId && !out_data.empty()) {
                // Output report through the control pipe (HIDIOCSOUTPUT).
                if (out_data[0] == id) handle_usb_output_report(out_data.data() + 1, out_data.size() - 1);
                else handle_usb_output_report(out_data.data(), out_data.size());
                return ControlReply::ok();
            }
            if (type == 3) {
                // The firmware does not forward feature writes (test commands,
                // Edge profile writes) either; acknowledge and drop them.
                LOGD("dropping SET_FEATURE 0x%02X (%zu bytes)", id, out_data.size());
                return ControlReply::ok();
            }
            return ControlReply::stall();
        case kSetIdle:
        case kSetProtocol:
            return ControlReply::ok();
        case kGetIdle:
            return ControlReply::ok({0});
        case kGetProtocol:
            return ControlReply::ok({1});
        default:
            return ControlReply::stall();
    }
}

ControlReply Bridge::audio_control_request(const SetupPacket &setup, const std::vector<uint8_t> &out_data) {
    // Port of audio10_set_req_entity() / audio10_get_req_entity() (src/usb.cpp).
    const uint8_t control = static_cast<uint8_t>(setup.wValue >> 8);
    const uint8_t entity = static_cast<uint8_t>(setup.wIndex >> 8);
    if (entity != usbdesc::kEntitySpeakerFeatureUnit && entity != usbdesc::kEntityMicFeatureUnit) {
        return ControlReply::stall();
    }
    const bool speaker = entity == usbdesc::kEntitySpeakerFeatureUnit;
    const int index = speaker ? 0 : 1;

    if (control == kFuMute) {
        if (setup.bRequest == kSetCur) {
            if (out_data.size() != 1) return ControlReply::stall();
            mute_[index] = out_data[0];
            ds5::SetStateData state{};
            state.AllowAudioMute = 1;
            state.MicMute = mute_[1] ? 1 : 0;
            state.SpeakerMute = mute_[0] ? 1 : 0;
            state.HeadphoneMute = mute_[0] ? 1 : 0;
            update_state(state);
            return ControlReply::ok();
        }
        return ControlReply::ok({mute_[index]});
    }
    if (control != kFuVolume) return ControlReply::stall();

    auto int16_bytes = [](int16_t v) {
        return std::vector<uint8_t>{static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF)};
    };
    switch (setup.bRequest) {
        case kSetCur: {
            if (out_data.size() != 2) return ControlReply::stall();
            const auto raw = static_cast<int16_t>(out_data[0] | out_data[1] << 8);
            volume_[index] = static_cast<float>(raw) / 256.0f;
            ds5::SetStateData state{};
            if (speaker) {
                speaker_volume_set_ = true;
                const float v = std::clamp(100.0f + volume_[0], 0.0f, 100.0f);
                state.AllowHeadphoneVolume = 1;
                state.AllowSpeakerVolume = 1;
                state.VolumeHeadphones = static_cast<uint8_t>(v);
                state.VolumeSpeaker = static_cast<uint8_t>(v);
            } else {
                state.AllowMicVolume = 1;
                state.VolumeMic = static_cast<uint8_t>(std::clamp(volume_[1], 0.0f, 48.0f));
            }
            update_state(state);
            return ControlReply::ok();
        }
        case kGetCur: {
            if (speaker && !speaker_volume_set_) {
                volume_[0] = -100.0f + static_cast<float>(std::min<int>(cfg_.speaker_volume, 100));
            }
            return ControlReply::ok(int16_bytes(static_cast<int16_t>(volume_[index] * 256.0f)));
        }
        case kGetMin:
            return ControlReply::ok(speaker ? std::vector<uint8_t>{0x00, 0x9C} : std::vector<uint8_t>{0x00, 0x00});
        case kGetMax:
            return ControlReply::ok(speaker ? std::vector<uint8_t>{0x00, 0x00} : std::vector<uint8_t>{0x00, 0x30});
        case kGetRes:
            return ControlReply::ok(speaker ? std::vector<uint8_t>{0x00, 0x01} : std::vector<uint8_t>{0x7A, 0x00});
        default:
            return ControlReply::stall();
    }
}

// ---------------------------------------------------------------------------
// Feature reports
// ---------------------------------------------------------------------------
void Bridge::request_feature(uint8_t report_id, uint32_t seqnum, bool has_urb) {
    FeatureWorker::Job job;
    job.report_id = report_id;
    job.token = next_token_++;
    if (has_urb) token_to_urb_[job.token] = seqnum;
    worker_->post(std::move(job));
}

void Bridge::on_worker_event() {
    for (auto &result : worker_->drain()) {
        const uint8_t id = result.job.report_id;
        if (result.ok) {
            const bool prefetched = std::find(std::begin(kPrefetchReports), std::end(kPrefetchReports), id) !=
                                    std::end(kPrefetchReports);
            if (prefetched) features_[id] = result.data;
            if (id == 0x20 && result.data.size() > 22) {
                real_is_edge_ = real_is_edge_ || result.data[22] == 0x44;
            }
        } else {
            LOGW("feature report 0x%02X unavailable", id);
        }

        const auto urb = token_to_urb_.find(result.job.token);
        if (urb != token_to_urb_.end()) {
            if (usb_) usb_->complete_control(urb->second, result.ok ? 0 : kEpipe, result.data);
            token_to_urb_.erase(urb);
        }
        if (id == 0x20 && !presentation_decided_) decide_presentation();
    }
}

void Bridge::decide_presentation() {
    presentation_decided_ = true;
    real_is_edge_ = real_is_edge_ || info().product_id == ds5::kDualSenseEdgePid;
    // controller_mode: 0 DualSense, 1 DualSense Edge, 2 auto (same as the
    // physical controller), like ds_mode() in the firmware.
    present_as_edge_ = cfg_.controller_mode == 1 || (cfg_.controller_mode == 2 && real_is_edge_);
    LOGI("controller is a %s, presenting it as a wired %s", real_is_edge_ ? "DualSense Edge" : "DualSense",
         present_as_edge_ ? "DualSense Edge" : "DualSense");
    retry_at_us_ = 0; // attach on the next housekeeping pass
    usb_state_ = UsbState::kRetrying;
}

// ---------------------------------------------------------------------------
// USB attach / detach
// ---------------------------------------------------------------------------
bool Bridge::attach_usb(std::string *error) {
    detach_usb();
    usb_ = std::make_unique<VirtualUsbDevice>(*this, present_as_edge_, std::string());
    configured_ = false;
    speaker_stream_ = false;
    speaker_volume_set_ = false;

    int our_end = -1;
    if (!options_.usbip_connect.empty()) {
        our_end = connect_tcp(options_.usbip_connect, error);
        if (our_end < 0) return false;
    } else {
        if (!usbip::vhci_available(error)) return false;
        const int port = usbip::vhci_find_free_port(error);
        if (port < 0) return false;
        int kernel_end = -1;
        if (!usbip::make_loopback_pair(kernel_end, our_end, error)) return false;
        const bool ok = usbip::vhci_attach(port, kernel_end, kUsbipDevid, usbip::kSpeedFull, error);
        ::close(kernel_end); // the kernel holds its own reference now
        if (!ok) {
            ::close(our_end);
            return false;
        }
        vhci_port_ = port;
    }
    set_nonblocking(our_end);
    usb_->set_socket(our_end);
    ++fd_generation_;
    if (have_input_) {
        uint8_t report[1 + ds5::kUsbInputBodySize];
        report[0] = ds5::kUsbInputReportId;
        std::memcpy(report + 1, body_, ds5::kUsbInputBodySize);
        usb_->send_input_report(report, sizeof(report));
    }
    if (vhci_port_ >= 0) LOGI("virtual wired controller attached on vhci port %d", vhci_port_);
    else LOGI("virtual wired controller connected to %s", options_.usbip_connect.c_str());
    return true;
}

void Bridge::detach_usb() {
    if (vhci_port_ >= 0) {
        std::string error;
        if (!usbip::vhci_detach(vhci_port_, &error)) LOGD("vhci detach: %s", error.c_str());
        vhci_port_ = -1;
    }
    if (usb_) {
        usb_.reset();
        ++fd_generation_;
    }
    token_to_urb_.clear(); // their URBs died with the device
    if (mic_active_) {
        mic_active_ = false;
        update_mic_status();
    }
    configured_ = false;
    speaker_stream_ = false;
}

void Bridge::reattach() {
    if (!presentation_decided_) return;
    LOGI("re-attaching the virtual controller");
    detach_usb();
    usb_state_ = UsbState::kRetrying;
    attach_failures_ = 0;
    retry_at_us_ = 0;
}

void Bridge::on_usb_event(bool readable, bool writable, int64_t now_us) {
    if (!usb_ || usb_->fd() < 0) return;
    bool ok = true;
    if (readable) ok = usb_->on_readable();
    if (ok && writable) ok = usb_->on_writable();
    if (!ok) {
        LOGW("virtual controller lost: %s", usb_->last_error().c_str());
        detach_usb();
        usb_state_ = UsbState::kRetrying;
        retry_at_us_ = now_us + kRetryDelayUs;
    }
}

void Bridge::tick(int64_t now_us) {
    if (started_us_ < 0) started_us_ = now_us;
    if (usb_) usb_->tick(now_us);
}

void Bridge::housekeeping(int64_t now_us) {
    if (started_us_ < 0) started_us_ = now_us;
    if (!presentation_decided_ && now_us - started_us_ > kAttachTimeoutUs) {
        LOGW("controller did not answer feature report 0x20, attaching anyway");
        decide_presentation();
    }
    if (usb_state_ == UsbState::kRetrying && now_us >= retry_at_us_) {
        std::string error;
        if (attach_usb(&error)) {
            usb_state_ = UsbState::kAttached;
            usb_error_.clear();
            attach_failures_ = 0;
        } else {
            usb_error_ = error;
            LOGE("cannot attach virtual controller: %s", error.c_str());
            if (++attach_failures_ >= kMaxAttachFailures) {
                usb_state_ = UsbState::kFailed;
            } else {
                retry_at_us_ = now_us + kRetryDelayUs;
            }
        }
    }

    if (cfg_.inactive_time > 0 && last_activity_us_ > 0 &&
        now_us - last_activity_us_ > static_cast<int64_t>(cfg_.inactive_time) * 60 * 1'000'000) {
        LOGI("controller idle for %u minutes, disconnecting", cfg_.inactive_time);
        last_activity_us_ = now_us;
        disconnect_controller();
    }
}

void Bridge::config_changed(const BridgeConfig &previous) {
    if (previous.trigger_reduce != cfg_.trigger_reduce || previous.speaker_gain != cfg_.speaker_gain ||
        previous.mic_select != cfg_.mic_select) {
        apply_config_state();
    }
    if (previous.led_red != cfg_.led_red || previous.led_green != cfg_.led_green ||
        previous.led_blue != cfg_.led_blue) {
        ds5::SetStateData state{};
        state.AllowLedColor = 1;
        state.LedRed = cfg_.led_red;
        state.LedGreen = cfg_.led_green;
        state.LedBlue = cfg_.led_blue;
        update_state(state);
    }
    if (previous.mic_select != cfg_.mic_select) update_mic_status();
    if (previous.controller_mode != cfg_.controller_mode && presentation_decided_) {
        const bool edge = cfg_.controller_mode == 1 || (cfg_.controller_mode == 2 && real_is_edge_);
        if (edge != present_as_edge_) {
            // Descriptors change: re-enumerate as the other model.
            present_as_edge_ = edge;
            LOGI("now presenting the controller as a wired %s", edge ? "DualSense Edge" : "DualSense");
            reattach();
        }
    }
}

void Bridge::disconnect_controller() {
    if (!options_.allow_bt_disconnect || info().mac.empty()) {
        LOGI("bluetooth disconnect requested for %s (disabled in this mode)", info().mac.c_str());
        return;
    }
    const std::string mac = info().mac;
    char arg0[] = "bluetoothctl";
    char arg1[] = "disconnect";
    std::vector<char> arg2(mac.begin(), mac.end());
    arg2.push_back('\0');
    char *argv[] = {arg0, arg1, arg2.data(), nullptr};
    pid_t pid = 0;
    if (posix_spawnp(&pid, "bluetoothctl", nullptr, nullptr, argv, environ) != 0) {
        LOGW("could not run bluetoothctl to disconnect %s", mac.c_str());
    }
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------
std::string Bridge::status_json() const {
    const char *usb_state = "waiting";
    switch (usb_state_) {
        case UsbState::kWaitingForController: usb_state = "starting"; break;
        case UsbState::kAttached: usb_state = configured_ ? "connected" : "attached"; break;
        case UsbState::kRetrying: usb_state = "retrying"; break;
        case UsbState::kFailed: usb_state = "failed"; break;
    }
    int battery = -1;
    bool charging = false;
    if (have_input_) {
        const uint8_t power = body_[ds5::kBodyPower];
        const int level = power & 0x0F;
        const int state = power >> 4;
        charging = state == 0x1;
        battery = state == 0x2 ? 100 : std::min(level * 10 + 5, 100);
    }
    const auto usb_stats = usb_ ? usb_->stats() : VirtualUsbDevice::Stats{};
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  "{\"id\":%s,\"name\":%s,\"mac\":%s,\"hidraw\":%s,\"type\":%s,\"presented_as\":%s,"
                  "\"battery\":%d,\"charging\":%s,\"headphones\":%s,\"usb\":%s,\"usb_error\":%s,\"vhci_port\":%d,"
                  "\"speaker_stream\":%s,\"mic_stream\":%s,\"stats\":{\"bt_in\":%llu,\"bt_out\":%llu,"
                  "\"audio_reports\":%llu,\"mic_frames\":%llu,\"mic_underruns\":%llu,\"write_errors\":%llu,"
                  "\"usb_urbs\":%llu,\"usb_audio_out\":%llu,\"usb_audio_in\":%llu}}",
                  json_string(info().id).c_str(), json_string(info().name).c_str(), json_string(info().mac).c_str(),
                  json_string(info().hidraw_path).c_str(),
                  json_string(real_is_edge_ || info().product_id == ds5::kDualSenseEdgePid ? "DualSense Edge"
                                                                                           : "DualSense")
                      .c_str(),
                  json_string(presentation_decided_ ? (present_as_edge_ ? "DualSense Edge" : "DualSense") : "")
                      .c_str(),
                  battery, json_bool(charging), json_bool(headset_), json_string(usb_state).c_str(),
                  json_string(usb_error_).c_str(), vhci_port_, json_bool(speaker_stream_), json_bool(mic_active_),
                  static_cast<unsigned long long>(stats_.bt_input_reports),
                  static_cast<unsigned long long>(stats_.bt_output_reports),
                  static_cast<unsigned long long>(stats_.audio_reports),
                  static_cast<unsigned long long>(stats_.mic_frames),
                  static_cast<unsigned long long>(mic_.underruns()),
                  static_cast<unsigned long long>(stats_.write_errors),
                  static_cast<unsigned long long>(usb_stats.urbs),
                  static_cast<unsigned long long>(usb_stats.audio_out_packets),
                  static_cast<unsigned long long>(usb_stats.audio_in_packets));
    return buf;
}
