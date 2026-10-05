#include "audio.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <opus.h>

#include "log.h"

namespace {

int64_t floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

} // namespace

// ---------------------------------------------------------------------------
// LinearResampler
// ---------------------------------------------------------------------------
LinearResampler::LinearResampler(int in_rate, int out_rate, int channels)
    : in_rate_(in_rate), out_rate_(out_rate), channels_(channels), prev_(static_cast<size_t>(channels), 0.0f) {}

void LinearResampler::reset() {
    pos_ = 0;
    have_prev_ = false;
    std::fill(prev_.begin(), prev_.end(), 0.0f);
}

void LinearResampler::process(const float *in, size_t frames, std::vector<float> &out) {
    if (frames == 0) return;
    const int64_t limit = static_cast<int64_t>(frames - 1) * out_rate_;
    while (pos_ <= limit) {
        const int64_t i0 = floor_div(pos_, out_rate_);
        const int64_t rem = pos_ - i0 * out_rate_;
        const float frac = static_cast<float>(rem) / static_cast<float>(out_rate_);
        for (int c = 0; c < channels_; ++c) {
            const float a = i0 < 0 ? (have_prev_ ? prev_[static_cast<size_t>(c)] : in[c])
                                   : in[i0 * channels_ + c];
            if (rem == 0) {
                out.push_back(a);
            } else {
                const float b = in[(i0 + 1) * channels_ + c];
                out.push_back(a + (b - a) * frac);
            }
        }
        pos_ += in_rate_;
    }
    pos_ -= static_cast<int64_t>(frames) * out_rate_;
    for (int c = 0; c < channels_; ++c) {
        prev_[static_cast<size_t>(c)] = in[(frames - 1) * static_cast<size_t>(channels_) + static_cast<size_t>(c)];
    }
    have_prev_ = true;
}

// ---------------------------------------------------------------------------
// AudioOutPipeline
// ---------------------------------------------------------------------------
AudioOutPipeline::AudioOutPipeline() = default;

AudioOutPipeline::~AudioOutPipeline() {
    if (encoder_) opus_encoder_destroy(encoder_);
}

bool AudioOutPipeline::init(std::string *error) {
    int err = 0;
    encoder_ = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &err);
    if (err != OPUS_OK || !encoder_) {
        if (error) *error = std::string("opus_encoder_create failed: ") + opus_strerror(err);
        encoder_ = nullptr;
        return false;
    }
    // Same encoder settings as core1_entry() in the firmware.
    opus_encoder_ctl(encoder_, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
    opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(static_cast<opus_int32>(kSpeakerPacket * 8 * 100)));
    opus_encoder_ctl(encoder_, OPUS_SET_VBR(0));
    opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(0));
    reset();
    return true;
}

void AudioOutPipeline::reset() {
    haptics_resampler_.reset();
    speaker_resampler_.reset();
    haptics_in_.clear();
    haptics_out_.clear();
    haptics_pos_ = 0;
    haptics_queue_.clear();
    speaker_block_.clear();
    speaker_resampled_.clear();
    speaker_queue_.clear();
    if (encoder_) opus_encoder_ctl(encoder_, OPUS_RESET_STATE);
}

void AudioOutPipeline::feed(const int16_t *samples, size_t frames, float haptics_gain, bool speaker_enabled) {
    constexpr size_t kChannels = 4;
    constexpr size_t kSpeakerBlockFrames = 512;

    if (!speaker_enabled && !speaker_block_.empty()) {
        speaker_block_.clear();
        speaker_resampled_.clear();
    }

    haptics_in_.resize(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        const int16_t *f = samples + i * kChannels;
        if (speaker_enabled) {
            speaker_block_.push_back(static_cast<float>(f[0]) / 32768.0f);
            speaker_block_.push_back(static_cast<float>(f[1]) / 32768.0f);
            if (speaker_block_.size() == kSpeakerBlockFrames * 2) {
                encode_speaker_block();
                speaker_block_.clear();
            }
        }
        haptics_in_[i * 2] = static_cast<float>(f[2]) / 32768.0f;
        haptics_in_[i * 2 + 1] = static_cast<float>(f[3]) / 32768.0f;
    }

    // 48 kHz -> 3 kHz, then int8 with gain; 64 bytes make one chunk.
    haptics_out_.clear();
    haptics_resampler_.process(haptics_in_.data(), frames, haptics_out_);
    for (float v : haptics_out_) {
        const int scaled = static_cast<int>(v * 127.0f * haptics_gain);
        haptics_buf_[haptics_pos_++] = static_cast<uint8_t>(static_cast<int8_t>(std::clamp(scaled, -128, 127)));
        if (haptics_pos_ == kHapticsChunk) {
            haptics_queue_.push(haptics_buf_);
            haptics_pos_ = 0;
        }
    }
}

void AudioOutPipeline::encode_speaker_block() {
    if (!encoder_) return;
    // 512 frames -> 480 frames (the firmware's fix for the speaker noise).
    speaker_resampler_.process(speaker_block_.data(), speaker_block_.size() / 2, speaker_resampled_);
    constexpr size_t kOpusFrames = 480;
    while (speaker_resampled_.size() >= kOpusFrames * 2) {
        SpeakerPacket packet{};
        const opus_int32 n = opus_encode_float(encoder_, speaker_resampled_.data(), kOpusFrames, packet.data(),
                                               static_cast<opus_int32>(packet.size()));
        speaker_resampled_.erase(speaker_resampled_.begin(),
                                 speaker_resampled_.begin() + static_cast<long>(kOpusFrames * 2));
        if (n <= 0) {
            LOGD("opus_encode_float failed: %d", n);
            continue;
        }
        speaker_queue_.push(packet); // zero padded to 200 bytes
        ++encoded_packets_;
    }
}

bool AudioOutPipeline::report_ready(bool speaker_enabled) const {
    if (haptics_queue_.size() < 2) return false;
    if (speaker_enabled && speaker_queue_.size() < 2) return false;
    return true;
}

void AudioOutPipeline::take(HapticsChunk haptics[2], SpeakerPacket speaker[2], bool speaker_enabled) {
    haptics_queue_.pop(haptics[0]);
    haptics_queue_.pop(haptics[1]);
    if (speaker_enabled) {
        speaker_queue_.pop(speaker[0]);
        speaker_queue_.pop(speaker[1]);
    }
}

// ---------------------------------------------------------------------------
// MicPipeline
// ---------------------------------------------------------------------------
MicPipeline::MicPipeline() = default;

MicPipeline::~MicPipeline() {
    if (decoder_) opus_decoder_destroy(decoder_);
}

bool MicPipeline::init(std::string *error) {
    int err = 0;
    decoder_ = opus_decoder_create(48000, 1, &err);
    if (err != OPUS_OK || !decoder_) {
        if (error) *error = std::string("opus_decoder_create failed: ") + opus_strerror(err);
        decoder_ = nullptr;
        return false;
    }
    return true;
}

void MicPipeline::reset() {
    pcm_.clear();
    primed_ = false;
    if (decoder_) opus_decoder_ctl(decoder_, OPUS_RESET_STATE);
}

void MicPipeline::push_opus(const uint8_t *data, size_t len) {
    if (!decoder_ || len < kOpusFrameSize) return;
    int16_t pcm[kFrameSamples];
    const int n = opus_decode(decoder_, data, kOpusFrameSize, pcm, kFrameSamples, 0);
    if (n <= 0) {
        LOGD("mic opus_decode failed: %d", n);
        return;
    }
    ++decoded_frames_;
    pcm_.insert(pcm_.end(), pcm, pcm + n);
    // Same depth as the firmware's mic queue (8 frames, ~80 ms): drop the
    // oldest audio rather than let latency grow.
    constexpr size_t kMaxSamples = kFrameSamples * 8;
    if (pcm_.size() > kMaxSamples) {
        pcm_.erase(pcm_.begin(), pcm_.begin() + static_cast<long>(pcm_.size() - kMaxSamples));
    }
}

size_t MicPipeline::fill_usb_packet(uint8_t *out, size_t max_len) {
    constexpr size_t kFramesPerPacket = 48;
    constexpr size_t kBytes = kFramesPerPacket * 2 * sizeof(int16_t); // 192
    if (max_len < kBytes) return 0;

    // Small jitter buffer: after an underrun wait for 20 ms of audio before
    // playing again, so Bluetooth arrival jitter does not crackle.
    if (!primed_ && pcm_.size() >= kFrameSamples * 2) primed_ = true;

    auto *dst = reinterpret_cast<int16_t *>(out);
    for (size_t i = 0; i < kFramesPerPacket; ++i) {
        int16_t s = 0;
        if (primed_ && !pcm_.empty()) {
            s = pcm_.front();
            pcm_.pop_front();
        }
        dst[i * 2] = s;
        dst[i * 2 + 1] = s;
    }
    if (primed_ && pcm_.empty()) {
        primed_ = false;
        ++underruns_;
    }
    return kBytes;
}
