// USB descriptors of the virtual wired DualSense.
//
// These are the descriptors of the DS5Dongle firmware (src/usb_descriptors.cpp)
// in its default build: no CDC serial, no wake keyboard, polling mode
// "real-time" (bInterval 1). The host therefore enumerates the exact same
// composite device it would see with the Pico plugged in: a UAC1 audio
// function (4ch speaker+haptics out, 2ch mic in) and the DualSense HID.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace usbdesc {

enum Interface : uint8_t {
    kItfAudioControl = 0,
    kItfAudioOut = 1,
    kItfAudioIn = 2,
    kItfHid = 3,
};

enum Endpoint : uint8_t {
    kEpAudioOut = 0x01, // isochronous OUT, 4ch 16bit 48kHz
    kEpAudioIn = 0x82,  // isochronous IN, 2ch 16bit 48kHz
    kEpHidOut = 0x03,   // interrupt OUT
    kEpHidIn = 0x84,    // interrupt IN
};

// Audio entity ids (UAC1 feature units), same as the firmware.
constexpr uint8_t kEntitySpeakerFeatureUnit = 0x02;
constexpr uint8_t kEntityMicFeatureUnit = 0x05;

std::vector<uint8_t> device_descriptor(uint16_t product_id);
std::vector<uint8_t> configuration_descriptor(bool edge);
const std::vector<uint8_t> &hid_report_descriptor(bool edge);
std::vector<uint8_t> hid_descriptor(bool edge);
// Returns an empty vector for unknown indices.
std::vector<uint8_t> string_descriptor(uint8_t index, bool edge, const std::string &serial);

} // namespace usbdesc
