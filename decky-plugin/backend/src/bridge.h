// One bridged controller: a Bluetooth DualSense on one side, a virtual wired
// DualSense on the other. This is the Steam Deck equivalent of the whole
// DS5Dongle firmware main loop for a single controller.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "audio.h"
#include "bt_link.h"
#include "config.h"
#include "ds5_proto.h"
#include "feature_worker.h"
#include "virtual_device.h"

struct BridgeOptions {
    // Test mode: instead of attaching to vhci-hcd, connect to a fake host
    // speaking USB/IP at host:port.
    std::string usbip_connect;
    // Command used to disconnect an idle controller (inactive_time).
    bool allow_bt_disconnect = true;
};

class Bridge final : public UsbDeviceHandler {
public:
    Bridge(std::unique_ptr<BtLink> link, const BridgeConfig &config, BridgeOptions options);
    ~Bridge() override;

    bool init(std::string *error);

    int bt_fd() const { return link_->input_fd(); }
    int usb_fd() const { return usb_ ? usb_->fd() : -1; }
    int worker_fd() const { return worker_->event_fd(); }
    bool usb_wants_write() const { return usb_ && usb_->wants_write(); }

    // Returns false once the controller is gone; the bridge should then be destroyed.
    bool on_bt_readable(int64_t now_us);
    void on_usb_event(bool readable, bool writable, int64_t now_us);
    void on_worker_event();
    void tick(int64_t now_us);
    void housekeeping(int64_t now_us);

    void config_changed(const BridgeConfig &previous);
    void reattach();
    void disconnect_controller();

    std::string status_json() const;
    const ControllerInfo &info() const { return link_->info(); }
    // Bumped whenever the set of fds to watch changes.
    uint64_t fd_generation() const { return fd_generation_; }

    // UsbDeviceHandler
    ControlReply on_class_request(const SetupPacket &setup, const std::vector<uint8_t> &out_data,
                                  uint32_t seqnum) override;
    void on_configured(bool configured) override;
    void on_set_interface(uint8_t itf, uint8_t alt) override;
    void on_hid_output(const uint8_t *data, size_t len) override;
    void on_audio_out_packet(const uint8_t *data, size_t len) override;
    size_t on_audio_in_packet(uint8_t *data, size_t max_len) override;

private:
    enum class UsbState { kWaitingForController, kAttached, kRetrying, kFailed };

    // Firmware ports.
    void handle_input_report(const uint8_t *report, size_t len, int64_t now_us);
    void handle_usb_output_report(const uint8_t *payload, size_t len);
    void bt_write(uint8_t *report, size_t len);
    void update_state(const ds5::SetStateData &state);
    void update_mic_status();
    void send_initial_state();
    void apply_config_state();
    void send_audio_reports();
    ControlReply audio_control_request(const SetupPacket &setup, const std::vector<uint8_t> &out_data);
    ControlReply hid_class_request(const SetupPacket &setup, const std::vector<uint8_t> &out_data, uint32_t seqnum);

    void decide_presentation();
    bool attach_usb(std::string *error);
    void detach_usb();
    void request_feature(uint8_t report_id, uint32_t seqnum, bool has_urb);
    bool mic_enabled() const;
    bool speaker_enabled() const;

    std::unique_ptr<BtLink> link_;
    const BridgeConfig &cfg_;
    BridgeOptions options_;
    std::unique_ptr<FeatureWorker> worker_;
    std::unique_ptr<VirtualUsbDevice> usb_;
    AudioOutPipeline audio_out_;
    MicPipeline mic_;

    UsbState usb_state_ = UsbState::kWaitingForController;
    std::string usb_error_;
    int vhci_port_ = -1;
    int64_t started_us_ = -1;
    int64_t retry_at_us_ = 0;
    int attach_failures_ = 0;
    bool presentation_decided_ = false;
    bool real_is_edge_ = false;
    bool present_as_edge_ = false;
    uint64_t fd_generation_ = 0;

    // Feature reports prefetched on connect (0x09, 0x20, 0x22, 0x05).
    std::map<uint8_t, std::vector<uint8_t>> features_;
    uint64_t next_token_ = 1;
    std::map<uint64_t, uint32_t> token_to_urb_;

    // Controller state.
    uint8_t body_[ds5::kUsbInputBodySize] = {};
    bool have_input_ = false;
    bool headset_ = false;
    uint8_t seq_ = 0;            // reportSeqCounter
    uint8_t packet_counter_ = 0; // packetCounter (0x39 reports)
    int64_t last_activity_us_ = 0;

    // Host state.
    bool configured_ = false;
    bool speaker_stream_ = false;
    bool mic_active_ = false;
    uint8_t mute_[2] = {0, 0};          // 0: speaker, 1: mic
    float volume_[2] = {0.0f, 48.0f};   // dB
    bool speaker_volume_set_ = false;

    struct Stats {
        uint64_t bt_input_reports = 0;
        uint64_t bt_output_reports = 0;
        uint64_t audio_reports = 0;
        uint64_t mic_frames = 0;
        uint64_t write_errors = 0;
    } stats_;
};
