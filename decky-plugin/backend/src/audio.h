// Audio paths of the bridge, ported from the DS5Dongle firmware (src/audio.cpp).
//
// Host -> controller ("audio out"): the virtual USB audio device receives
// 4 channel 48 kHz PCM. Channels 3/4 are the HD haptics: they are resampled to
// 3 kHz and converted to signed 8 bit. Channels 1/2 are the speaker/headset
// audio: 512-frame blocks are resampled to 480 frames and Opus encoded into
// 200 byte CBR packets. Two haptics chunks plus two Opus packets make up one
// Bluetooth report 0x39.
//
// Controller -> host ("mic"): the controller sends 71 byte Opus frames (48 kHz
// mono, 10 ms) which are decoded and handed to the USB mic endpoint as 2ch.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

struct OpusEncoder;
struct OpusDecoder;

// Linear interpolating resampler with exact rational stepping. With the
// firmware's WDL settings (interpolation on, no filter, no sinc) WDL performs
// plain linear interpolation, which is what this reproduces.
class LinearResampler {
public:
    LinearResampler(int in_rate, int out_rate, int channels);
    void reset();
    // Appends the produced interleaved frames to out.
    void process(const float *in, size_t frames, std::vector<float> &out);

private:
    int64_t in_rate_;
    int64_t out_rate_;
    int channels_;
    int64_t pos_ = 0; // next output position, in 1/out_rate input samples
    std::vector<float> prev_;
    bool have_prev_ = false;
};

template <typename T, size_t Depth>
class DropOldestQueue {
public:
    void push(const T &v) {
        if (q_.size() == Depth) q_.pop_front();
        q_.push_back(v);
    }
    bool pop(T &out) {
        if (q_.empty()) return false;
        out = q_.front();
        q_.pop_front();
        return true;
    }
    size_t size() const { return q_.size(); }
    void clear() { q_.clear(); }

private:
    std::deque<T> q_;
};

class AudioOutPipeline {
public:
    static constexpr size_t kHapticsChunk = 64;  // bytes = 32 stereo int8 frames at 3 kHz
    static constexpr size_t kSpeakerPacket = 200; // Opus CBR 160 kbps, 10 ms
    using HapticsChunk = std::array<uint8_t, kHapticsChunk>;
    using SpeakerPacket = std::array<uint8_t, kSpeakerPacket>;

    AudioOutPipeline();
    ~AudioOutPipeline();
    bool init(std::string *error);
    void reset();

    // frames of 4ch interleaved s16 at 48 kHz (one USB isochronous packet).
    void feed(const int16_t *samples, size_t frames, float haptics_gain, bool speaker_enabled);

    // Mirrors audio_bt_task(): a report is due once two haptics chunks (and,
    // with the speaker enabled, two Opus packets) are queued.
    bool report_ready(bool speaker_enabled) const;
    void take(HapticsChunk haptics[2], SpeakerPacket speaker[2], bool speaker_enabled);

    uint64_t encoded_packets() const { return encoded_packets_; }

private:
    void encode_speaker_block();

    LinearResampler haptics_resampler_{48000, 3000, 2};
    LinearResampler speaker_resampler_{51200, 48000, 2}; // 512 -> 480 frames per block
    std::vector<float> haptics_in_;
    std::vector<float> haptics_out_;
    HapticsChunk haptics_buf_{};
    size_t haptics_pos_ = 0;
    DropOldestQueue<HapticsChunk, 2> haptics_queue_;

    std::vector<float> speaker_block_; // 512 stereo frames
    std::vector<float> speaker_resampled_;
    DropOldestQueue<SpeakerPacket, 2> speaker_queue_;
    OpusEncoder *encoder_ = nullptr;
    uint64_t encoded_packets_ = 0;
};

class MicPipeline {
public:
    static constexpr size_t kOpusFrameSize = 71;
    static constexpr size_t kFrameSamples = 480; // 10 ms at 48 kHz, mono

    MicPipeline();
    ~MicPipeline();
    bool init(std::string *error);
    void reset();
    void push_opus(const uint8_t *data, size_t len);
    // Fills one 1 ms USB packet (48 stereo s16 frames = 192 bytes).
    size_t fill_usb_packet(uint8_t *out, size_t max_len);

    uint64_t decoded_frames() const { return decoded_frames_; }
    uint64_t underruns() const { return underruns_; }

private:
    OpusDecoder *decoder_ = nullptr;
    std::deque<int16_t> pcm_;
    bool primed_ = false;
    uint64_t decoded_frames_ = 0;
    uint64_t underruns_ = 0;
};
