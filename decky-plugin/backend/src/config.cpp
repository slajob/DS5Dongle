#include "config.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

bool parse_int(const std::string &value, long min, long max, long &out) {
    if (value.empty()) return false;
    char *end = nullptr;
    const long v = std::strtol(value.c_str(), &end, 0);
    if (*end != '\0' || v < min || v > max) return false;
    out = v;
    return true;
}

} // namespace

bool BridgeConfig::set(const std::string &key, const std::string &value, std::string *error) {
    auto fail = [&](const std::string &msg) {
        if (error) *error = msg;
        return false;
    };
    auto set_u8 = [&](uint8_t &field, long min, long max) {
        long v = 0;
        if (!parse_int(value, min, max, v)) {
            return fail(key + " must be an integer in [" + std::to_string(min) + ", " + std::to_string(max) + "]");
        }
        field = static_cast<uint8_t>(v);
        return true;
    };

    if (key == "haptics_gain") {
        char *end = nullptr;
        const float v = std::strtof(value.c_str(), &end);
        if (value.empty() || *end != '\0' || std::isnan(v) || v < 1.0f || v > 2.0f) {
            return fail("haptics_gain must be a number in [1.0, 2.0]");
        }
        haptics_gain = v;
        return true;
    }
    if (key == "speaker_volume") return set_u8(speaker_volume, 0, 127);
    if (key == "speaker_gain") return set_u8(speaker_gain, 0, 7);
    if (key == "inactive_time") return set_u8(inactive_time, 0, 60);
    if (key == "audio_buffer_length") return set_u8(audio_buffer_length, 16, 128);
    if (key == "controller_mode") return set_u8(controller_mode, 0, 2);
    if (key == "mic_select") return set_u8(mic_select, 0, 3);
    if (key == "speaker_select") return set_u8(speaker_select, 0, 3);
    if (key == "trigger_reduce") return set_u8(trigger_reduce, 0, 10);
    if (key == "lock_volume") return set_u8(lock_volume, 0, 1);
    if (key == "lightbar_color") {
        unsigned r = 0, g = 0, b = 0;
        std::string hex = value;
        if (!hex.empty() && hex[0] == '#') hex.erase(0, 1);
        if (hex.size() != 6 || std::sscanf(hex.c_str(), "%2x%2x%2x", &r, &g, &b) != 3) {
            return fail("lightbar_color must look like #RRGGBB");
        }
        led_red = static_cast<uint8_t>(r);
        led_green = static_cast<uint8_t>(g);
        led_blue = static_cast<uint8_t>(b);
        return true;
    }
    return fail("unknown setting '" + key + "'");
}

std::string BridgeConfig::to_json() const {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "{\"haptics_gain\":%.2f,\"speaker_volume\":%u,\"speaker_gain\":%u,\"inactive_time\":%u,"
                  "\"audio_buffer_length\":%u,\"controller_mode\":%u,\"mic_select\":%u,\"speaker_select\":%u,"
                  "\"trigger_reduce\":%u,\"lock_volume\":%u,\"lightbar_color\":\"#%02x%02x%02x\"}",
                  static_cast<double>(haptics_gain), speaker_volume, speaker_gain, inactive_time,
                  audio_buffer_length, controller_mode, mic_select, speaker_select, trigger_reduce, lock_volume,
                  led_red, led_green, led_blue);
    return buf;
}
