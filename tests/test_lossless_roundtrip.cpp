// In-process encode -> ByteRingBuffer -> decode round trip in *lossless*
// mode (SenderConfig::lossless / WavpackEncoder::Config::lossless), at all
// three selectable encode rates. Mirrors test_encode_decode_roundtrip.cpp,
// but where hybrid lossy mode only has to be "close", lossless mode has a
// hard correctness property to check: bit-exact reconstruction. This is a
// genuinely new code path (CONFIG_HYBRID_FLAG conditionally never set —
// see wavpack_encoder.cpp) that nothing else exercises.

#include <cstdio>
#include <vector>

#include "app_config.h"
#include "byte_ring_buffer.h"
#include "synthetic_signal.h"
#include "wavpack_decoder.h"
#include "wavpack_encoder.h"

using namespace mpx;

namespace {

bool RunRoundTrip(int sample_rate) {
    const int kNumFrames = sample_rate * 2; // 2 seconds
    std::vector<int16_t> original = GenerateSineSweep(kNumFrames, sample_rate, 50.0, 90000.0);

    ByteRingBuffer ring(4 * 1024 * 1024); // lossless blocks can approach raw PCM size
    WavpackEncoder encoder;
    WavpackEncoder::Config enc_cfg;
    enc_cfg.sample_rate = sample_rate;
    enc_cfg.channels = kChannels;
    enc_cfg.block_samples = kBlockSamples;
    enc_cfg.lossless = true;
    enc_cfg.very_high_quality = true; // matches production sender_app.cpp

    bool opened = encoder.open(enc_cfg, [&](const uint8_t* data, size_t len) {
        ring.push(data, len);
    });
    if (!opened) {
        std::printf("[%d Hz] FAILED: encoder.open(): %s\n", sample_rate, encoder.lastError().c_str());
        return false;
    }

    size_t offset = 0;
    while (offset + kBlockSamples <= original.size()) {
        if (!encoder.pushSamples(original.data() + offset, kBlockSamples)) {
            std::printf("[%d Hz] FAILED: pushSamples(): %s\n", sample_rate, encoder.lastError().c_str());
            return false;
        }
        offset += kBlockSamples;
    }
    encoder.flush();
    encoder.close();

    double raw_bytes = static_cast<double>(original.size()) * sizeof(int16_t);
    std::printf("[%d Hz] Encoded %zu frames into %zu bytes (%.1f%% of raw PCM, %.0f kbps).\n",
                sample_rate, original.size(), ring.availableBytes(),
                100.0 * ring.availableBytes() / raw_bytes,
                ring.availableBytes() * 8.0 / (static_cast<double>(original.size()) / sample_rate) / 1000.0);

    WavpackDecoder decoder;
    if (!decoder.open(&ring, /*read_timeout_ms=*/200)) {
        std::printf("[%d Hz] FAILED: decoder.open(): %s\n", sample_rate, decoder.lastError().c_str());
        return false;
    }

    std::vector<int32_t> decoded_buf(kBlockSamples * kChannels);
    std::vector<int16_t> decoded;
    decoded.reserve(original.size());

    for (;;) {
        uint32_t got = decoder.decode(decoded_buf.data(), kBlockSamples);
        if (got == 0) break;
        for (uint32_t i = 0; i < got; ++i) {
            decoded.push_back(static_cast<int16_t>(decoded_buf[i]));
        }
    }

    std::printf("[%d Hz] Decoded %zu frames (errors reported by libwavpack: %d).\n",
                sample_rate, decoded.size(), decoder.lastErrorCount());

    if (decoder.sampleRate() != sample_rate || decoder.numChannels() != kChannels) {
        std::printf("[%d Hz] FAILED: format mismatch — decoded sample_rate=%d channels=%d\n",
                    sample_rate, decoder.sampleRate(), decoder.numChannels());
        return false;
    }
    if (decoder.lastErrorCount() > 0) {
        std::printf("[%d Hz] FAILED: libwavpack reported decode errors on a clean in-process stream.\n",
                    sample_rate);
        return false;
    }
    // Allow for the final short block (fewer than kBlockSamples leftover
    // frames not pushed) but nothing more — lossless must not drop data.
    if (decoded.size() < original.size() - kBlockSamples) {
        std::printf("[%d Hz] FAILED: decoded far fewer frames (%zu) than encoded (%zu)\n",
                    sample_rate, decoded.size(), original.size());
        return false;
    }

    size_t compare_n = std::min(decoded.size(), original.size());
    size_t mismatches = 0;
    for (size_t i = 0; i < compare_n; ++i) {
        if (decoded[i] != original[i]) ++mismatches;
    }
    if (mismatches > 0) {
        std::printf("[%d Hz] FAILED: %zu / %zu samples were NOT bit-exact — lossless must round-trip exactly.\n",
                    sample_rate, mismatches, compare_n);
        return false;
    }

    std::printf("[%d Hz] Bit-exact match across %zu samples. OK\n", sample_rate, compare_n);
    return true;
}

} // namespace

int main() {
    bool all_ok = true;
    for (int rate : {kEncodeSampleRateLow, kEncodeSampleRateMid, kEncodeSampleRateHigh}) {
        if (!RunRoundTrip(rate)) all_ok = false;
    }

    std::printf(all_ok ? "PASS\n" : "FAILED\n");
    return all_ok ? 0 : 1;
}
