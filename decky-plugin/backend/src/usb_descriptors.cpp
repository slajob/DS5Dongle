#include "usb_descriptors.h"

#include "ds5_proto.h"

namespace usbdesc {

#include "hid_report_descriptors.inc"

namespace {

constexpr uint16_t kConfigTotalLength = 0x00E3; // 227, same as CONFIG_DESC_LEN_BASE

// Byte-for-byte copy of descriptor_configuration[] from the firmware with
// ENABLE_SERIAL=0, no ENABLE_WAKE_HID and polling_rate_mode=2 (bInterval 1).
// The HID report descriptor length (DS vs DSE) is patched in at runtime.
const uint8_t kConfiguration[kConfigTotalLength] = {
    // --- CONFIGURATION DESCRIPTOR ---
    0x09, 0x02, kConfigTotalLength & 0xFF, kConfigTotalLength >> 8,
    0x04,       // bNumInterfaces
    0x01,       // bConfigurationValue
    0x00,       // iConfiguration
    0xC0,       // bmAttributes: self powered, no remote wakeup
    0xFA,       // bMaxPower: 500mA

    // --- INTERFACE 0.0: Audio Control ---
    0x09, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
    // AC Interface Header (bcdADC 1.00, wTotalLength 73, 2 streaming interfaces: 1, 2)
    0x0A, 0x24, 0x01, 0x00, 0x01, 0x49, 0x00, 0x02, 0x01, 0x02,
    // Input Terminal 1: USB streaming, 4 channels (FL FR SL SR)
    0x0C, 0x24, 0x02, 0x01, 0x01, 0x01, 0x06, 0x04, 0x33, 0x00, 0x00, 0x00,
    // Feature Unit 2 <- 1: master mute + volume
    0x0C, 0x24, 0x06, 0x02, 0x01, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
    // Output Terminal 3: speaker <- 2
    0x09, 0x24, 0x03, 0x03, 0x01, 0x03, 0x04, 0x02, 0x00,
    // Input Terminal 4: headset mic, 2 channels
    0x0C, 0x24, 0x02, 0x04, 0x02, 0x04, 0x03, 0x02, 0x03, 0x00, 0x00, 0x00,
    // Feature Unit 5 <- 4: master mute + volume
    0x09, 0x24, 0x06, 0x05, 0x04, 0x01, 0x03, 0x00, 0x00,
    // Output Terminal 6: USB streaming <- 5
    0x09, 0x24, 0x03, 0x06, 0x01, 0x01, 0x01, 0x05, 0x00,

    // --- INTERFACE 1.0 / 1.1: Audio Streaming OUT ---
    0x09, 0x04, 0x01, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
    0x09, 0x04, 0x01, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,
    0x07, 0x24, 0x01, 0x01, 0x01, 0x01, 0x00,                   // AS general, terminal 1, PCM
    0x0B, 0x24, 0x02, 0x01, 0x04, 0x02, 0x10, 0x01, 0x80, 0xBB, 0x00, // 4ch 16bit 48000Hz
    0x09, 0x05, 0x01, 0x09, 0x88, 0x01, 0x01, 0x00, 0x00,       // EP1 OUT iso adaptive, 392 bytes
    0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00,

    // --- INTERFACE 2.0 / 2.1: Audio Streaming IN ---
    0x09, 0x04, 0x02, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
    0x09, 0x04, 0x02, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,
    0x07, 0x24, 0x01, 0x06, 0x01, 0x01, 0x00,                   // AS general, terminal 6, PCM
    0x0B, 0x24, 0x02, 0x01, 0x02, 0x02, 0x10, 0x01, 0x80, 0xBB, 0x00, // 2ch 16bit 48000Hz
    0x09, 0x05, 0x82, 0x05, 0xC4, 0x00, 0x01, 0x00, 0x00,       // EP2 IN iso async, 196 bytes
    0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00,

    // --- INTERFACE 3.0: HID ---
    0x09, 0x04, 0x03, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x41, 0x01,       // HID 1.11, report desc 321 (DS)
    0x07, 0x05, 0x84, 0x03, 0x40, 0x00, 0x01,                   // EP4 IN interrupt, 64 bytes, 1ms
    0x07, 0x05, 0x03, 0x03, 0x40, 0x00, 0x01,                   // EP3 OUT interrupt, 64 bytes, 1ms
};

constexpr size_t kHidDescriptorOffset = kConfigTotalLength - 23; // 9 bytes, right after interface 3.0
constexpr size_t kReportLengthOffset = kHidDescriptorOffset + 7;

std::vector<uint8_t> utf16_string(const std::string &s) {
    std::vector<uint8_t> out;
    out.reserve(2 + s.size() * 2);
    out.push_back(static_cast<uint8_t>(2 + s.size() * 2));
    out.push_back(0x03);
    for (char c : s) {
        out.push_back(static_cast<uint8_t>(c));
        out.push_back(0);
    }
    return out;
}

} // namespace

std::vector<uint8_t> device_descriptor(uint16_t product_id) {
    return {
        18, 0x01,
        0x00, 0x02,             // bcdUSB 2.00
        0x00, 0x00, 0x00,       // class/subclass/protocol: per interface
        64,                     // bMaxPacketSize0
        ds5::kSonyVid & 0xFF, ds5::kSonyVid >> 8,
        static_cast<uint8_t>(product_id & 0xFF), static_cast<uint8_t>(product_id >> 8),
        0x00, 0x01,             // bcdDevice 1.00
        0x01, 0x02, 0x00,       // iManufacturer, iProduct, iSerialNumber (set by caller)
        0x01,                   // bNumConfigurations
    };
}

std::vector<uint8_t> configuration_descriptor(bool edge) {
    std::vector<uint8_t> out(kConfiguration, kConfiguration + sizeof(kConfiguration));
    const uint16_t len = static_cast<uint16_t>(hid_report_descriptor(edge).size());
    out[kReportLengthOffset] = static_cast<uint8_t>(len & 0xFF);
    out[kReportLengthOffset + 1] = static_cast<uint8_t>(len >> 8);
    return out;
}

const std::vector<uint8_t> &hid_report_descriptor(bool edge) {
    static const std::vector<uint8_t> ds(kHidReportDescriptorDs,
                                         kHidReportDescriptorDs + sizeof(kHidReportDescriptorDs));
    static const std::vector<uint8_t> dse(kHidReportDescriptorDse,
                                          kHidReportDescriptorDse + sizeof(kHidReportDescriptorDse));
    return edge ? dse : ds;
}

std::vector<uint8_t> hid_descriptor(bool edge) {
    const auto config = configuration_descriptor(edge);
    return {config.begin() + kHidDescriptorOffset, config.begin() + kHidDescriptorOffset + 9};
}

std::vector<uint8_t> string_descriptor(uint8_t index, bool edge, const std::string &serial) {
    switch (index) {
        case 0:
            return {0x04, 0x03, 0x09, 0x04}; // English (US)
        case 1:
            return utf16_string("Sony Interactive Entertainment");
        case 2:
            return utf16_string(edge ? "DualSense Edge Wireless Controller" : "DualSense Wireless Controller");
        case 3:
            if (serial.empty()) return {};
            return utf16_string(serial);
        default:
            return {};
    }
}

} // namespace usbdesc
