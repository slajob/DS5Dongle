// DualSense protocol definitions shared by the bridge.
//
// Ported from the DS5Dongle firmware (src/utils.h, src/bt.cpp, src/audio.cpp):
// the CRC helpers, the SetStateData layout and the Bluetooth report sizes are
// byte-for-byte the same so the controller sees exactly what the Pico sends.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ds5 {

constexpr uint16_t kSonyVid = 0x054C;
constexpr uint16_t kDualSensePid = 0x0CE6;
constexpr uint16_t kDualSenseEdgePid = 0x0DF2;

// Bluetooth report ids / sizes (sizes include the report id, exclude the
// 0xA1/0xA2 HIDP header that the kernel adds and strips for us).
constexpr uint8_t kBtInputReportId = 0x31;
constexpr size_t kBtInputReportSize = 78;
constexpr uint8_t kBtOutputReportId = 0x31;
constexpr size_t kBtOutputReportSize = 78;
constexpr uint8_t kBtStateReportId = 0x32;
constexpr size_t kBtStateReportSize = 142;
constexpr uint8_t kBtAudioReportId = 0x39;
constexpr size_t kBtAudioReportSize = 547;

// USB side.
constexpr uint8_t kUsbInputReportId = 0x01;
constexpr size_t kUsbInputBodySize = 63; // report is 64 bytes including the id
constexpr uint8_t kUsbOutputReportId = 0x02;

// ---------------------------------------------------------------------------
// CRC32 (reflected, poly 0xEDB88320). The DualSense prepends the HIDP header
// byte to every checksummed report; the seeds below are the CRC state after
// that byte, exactly like the firmware.
// ---------------------------------------------------------------------------
constexpr uint32_t crc32_table_entry(uint32_t index) {
    for (unsigned bit = 0; bit < 8; ++bit) {
        index = (index >> 1) ^ (0xEDB88320u & (0u - (index & 1u)));
    }
    return index;
}

constexpr std::array<uint32_t, 256> make_crc32_table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < table.size(); ++i) {
        table[i] = crc32_table_entry(i);
    }
    return table;
}

inline constexpr std::array<uint32_t, 256> kCrc32Table = make_crc32_table();

inline uint32_t crc32_seeded(const uint8_t *data, size_t size, uint32_t seed) {
    uint32_t crc = ~seed;
    while (size--) {
        crc = (crc >> 8) ^ kCrc32Table[(crc ^ *data++) & 0xFF];
    }
    return ~crc;
}

constexpr uint32_t kCrcSeedOutput = 0xEADA2D49; // after 0xA2 (HIDP DATA | OUTPUT)
constexpr uint32_t kCrcSeedFeature = 0x2060EFC3; // after 0x53 (HIDP SET_REPORT | FEATURE)
constexpr uint32_t kCrcSeedInput = 0x73D37CF3;   // after 0xA1 (HIDP DATA | INPUT)

inline void put_le32(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

inline uint32_t get_le32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
           static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}

// report[0] is the report id; the last 4 bytes receive the CRC.
inline void fill_output_report_checksum(uint8_t *report, size_t len) {
    put_le32(report + len - 4, crc32_seeded(report, len - 4, kCrcSeedOutput));
}

inline void fill_feature_report_checksum(uint8_t *report, size_t len) {
    put_le32(report + len - 4, crc32_seeded(report, len - 4, kCrcSeedFeature));
}

inline bool input_report_checksum_ok(const uint8_t *report, size_t len) {
    if (len < 5) return false;
    return crc32_seeded(report, len - 4, kCrcSeedInput) == get_le32(report + len - 4);
}

namespace MuteLight {
enum MuteLight : uint8_t { Off = 0, On, Breathing, DoNothing };
}

namespace LightBrightness {
enum LightBrightness : uint8_t { Bright = 0, Mid, Dim };
}

namespace LightFadeAnimation {
enum LightFadeAnimation : uint8_t { Nothing = 0, FadeIn, FadeOut };
}

// Output report payload ("SetStateData"), identical to the firmware struct.
// USB report 0x02 carries the first 47 bytes; the BT 0x31/0x32 reports carry
// all 63 (the tail is reserved / padding before the CRC).
struct __attribute__((packed)) SetStateData {
    uint8_t EnableRumbleEmulation : 1;
    uint8_t UseRumbleNotHaptics : 1;
    uint8_t AllowRightTriggerFFB : 1;
    uint8_t AllowLeftTriggerFFB : 1;
    uint8_t AllowHeadphoneVolume : 1;
    uint8_t AllowSpeakerVolume : 1;
    uint8_t AllowMicVolume : 1;
    uint8_t AllowAudioControl : 1;
    uint8_t AllowMuteLight : 1;
    uint8_t AllowAudioMute : 1;
    uint8_t AllowLedColor : 1;
    uint8_t ResetLights : 1;
    uint8_t AllowPlayerIndicators : 1;
    uint8_t AllowHapticLowPassFilter : 1;
    uint8_t AllowMotorPowerLevel : 1;
    uint8_t AllowAudioControl2 : 1;
    uint8_t RumbleEmulationRight;
    uint8_t RumbleEmulationLeft;
    uint8_t VolumeHeadphones;
    uint8_t VolumeSpeaker;
    uint8_t VolumeMic;
    uint8_t MicSelect : 2;
    uint8_t EchoCancelEnable : 1;
    uint8_t NoiseCancelEnable : 1;
    uint8_t OutputPathSelect : 2;
    uint8_t InputPathSelect : 2;
    MuteLight::MuteLight MuteLightMode;
    uint8_t TouchPowerSave : 1;
    uint8_t MotionPowerSave : 1;
    uint8_t HapticPowerSave : 1;
    uint8_t AudioPowerSave : 1;
    uint8_t MicMute : 1;
    uint8_t SpeakerMute : 1;
    uint8_t HeadphoneMute : 1;
    uint8_t HapticMute : 1;
    uint8_t RightTriggerFFB[11];
    uint8_t LeftTriggerFFB[11];
    uint32_t HostTimestamp;
    uint8_t RumbleMotorPowerReduction : 4;
    uint8_t TriggerMotorPowerReduction : 4;
    uint8_t SpeakerCompPreGain : 3;
    uint8_t BeamformingEnable : 1;
    uint8_t UnkAudioControl2 : 4;
    uint8_t AllowLightBrightnessChange : 1;
    uint8_t AllowColorLightFadeAnimation : 1;
    uint8_t EnableImprovedRumbleEmulation : 1;
    uint8_t UseRumbleNotHaptics2 : 1;
    uint8_t UNKBITC : 4;
    uint8_t HapticLowPassFilter : 1;
    uint8_t UNKBIT : 7;
    uint8_t UNKBYTE;
    LightFadeAnimation::LightFadeAnimation LightFadeAnimation;
    LightBrightness::LightBrightness LightBrightness;
    uint8_t PlayerLight1 : 1;
    uint8_t PlayerLight2 : 1;
    uint8_t PlayerLight3 : 1;
    uint8_t PlayerLight4 : 1;
    uint8_t PlayerLight5 : 1;
    uint8_t PlayerLightFade : 1;
    uint8_t PlayerLightUNK : 2;
    uint8_t LedRed;
    uint8_t LedGreen;
    uint8_t LedBlue;
    uint8_t reserved[16];
};
static_assert(sizeof(SetStateData) == 63, "SetStateData must match the firmware layout");

// Offsets inside the 63-byte input body (USB report 0x01 without the id,
// BT report 0x31 from byte 2). See USBGetStateData in the firmware.
constexpr size_t kBodyLeftStickX = 0;
constexpr size_t kBodyButtons0 = 7;   // dpad + face buttons
constexpr size_t kBodyButtons1 = 8;
constexpr size_t kBodyButtons2 = 9;   // PS / touchpad / mute (+ Edge buttons)
constexpr size_t kBodyPower = 52;     // low nibble: level 0..10, high nibble: state
constexpr size_t kBodyPlugged = 53;   // bit0 headphones, bit1 mic, bit2 mic muted

} // namespace ds5
