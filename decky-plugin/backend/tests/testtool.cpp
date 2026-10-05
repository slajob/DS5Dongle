// Opus helper for the test-suite (not shipped in the plugin).
//
//   ds5bridge_testtool encode-mic FREQ FRAMES
//       Prints FRAMES hex lines, each a 71-byte Opus frame of a FREQ Hz sine
//       (48 kHz mono, 10 ms), i.e. what the DualSense sends for its mic.
//   ds5bridge_testtool decode-speaker < hex lines
//       Decodes 200-byte stereo Opus packets (one hex packet per line) and
//       writes the PCM as raw s16le stereo to stdout.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <opus.h>

static int encode_mic(double freq, int frames) {
    int err = 0;
    OpusEncoder *enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK) return 1;
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(71 * 8 * 100));
    opus_encoder_ctl(enc, OPUS_SET_VBR(0));
    double phase = 0;
    for (int f = 0; f < frames; ++f) {
        int16_t pcm[480];
        for (int i = 0; i < 480; ++i) {
            pcm[i] = static_cast<int16_t>(12000 * std::sin(phase));
            phase += 2 * M_PI * freq / 48000.0;
        }
        unsigned char out[71] = {};
        const int n = opus_encode(enc, pcm, 480, out, sizeof(out));
        if (n < 0) return 1;
        for (unsigned char c : out) std::printf("%02x", c);
        std::printf("\n");
    }
    opus_encoder_destroy(enc);
    return 0;
}

static int decode_speaker() {
    int err = 0;
    OpusDecoder *dec = opus_decoder_create(48000, 2, &err);
    if (err != OPUS_OK) return 1;
    std::string line;
    while (std::getline(std::cin, line)) {
        std::vector<unsigned char> packet;
        for (size_t i = 0; i + 1 < line.size(); i += 2) {
            packet.push_back(static_cast<unsigned char>(std::strtoul(line.substr(i, 2).c_str(), nullptr, 16)));
        }
        int16_t pcm[960 * 2];
        const int n = opus_decode(dec, packet.data(), static_cast<opus_int32>(packet.size()), pcm, 960, 0);
        if (n < 0) {
            std::fprintf(stderr, "decode failed: %d\n", n);
            return 1;
        }
        std::fwrite(pcm, sizeof(int16_t), static_cast<size_t>(n) * 2, stdout);
    }
    opus_decoder_destroy(dec);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 4 && std::strcmp(argv[1], "encode-mic") == 0) return encode_mic(std::atof(argv[2]), std::atoi(argv[3]));
    if (argc >= 2 && std::strcmp(argv[1], "decode-speaker") == 0) return decode_speaker();
    std::fprintf(stderr, "usage: %s encode-mic FREQ FRAMES | decode-speaker\n", argv[0]);
    return 2;
}
