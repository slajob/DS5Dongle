// Runtime settings. Field names, ranges and defaults follow the firmware's
// Config_body (src/config.h / config_valid()) so the web-config semantics carry
// over; settings that only make sense on the Pico (LED, GPIO, wake, polling
// mode) are dropped.

#pragma once

#include <cstdint>
#include <string>

struct BridgeConfig {
    float haptics_gain = 1.0f;          // [1.0, 2.0]
    uint8_t speaker_volume = 100;       // [0, 127] initial UAC volume reported to the host
    uint8_t speaker_gain = 2;           // [0, 7] (0: let the game decide)
    uint8_t inactive_time = 30;         // [0, 60] minutes, 0 disables idle disconnect
    uint8_t audio_buffer_length = 48;   // [16, 128]
    uint8_t controller_mode = 2;        // 0: DualSense, 1: DualSense Edge, 2: auto
    uint8_t mic_select = 0;             // 0: auto, 1: built-in, 2: headset, 3: disabled
    uint8_t speaker_select = 0;         // 0: auto, 1: built-in, 2: headset, 3: disabled
    uint8_t trigger_reduce = 0;         // [0, 10] (0: let the game decide)
    uint8_t lock_volume = 0;            // bool
    uint8_t led_red = 0xFF;             // light bar colour set on connect
    uint8_t led_green = 0xD7;
    uint8_t led_blue = 0x00;

    // Parses one "key value" setting. Returns false (and sets error) for an
    // unknown key or an out-of-range value; the config is left unchanged.
    bool set(const std::string &key, const std::string &value, std::string *error);
    std::string to_json() const;
};
