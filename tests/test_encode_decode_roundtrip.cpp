// In-process encode -> ByteRingBuffer -> decode round trip on a synthetic
// signal, with no sockets involved. Isolates WavpackEncoder/WavpackDecoder
// wrapper bugs from any transport-layer bugs (verification plan step 2 in
// docs/phase2-plan.md). Run at both selectable encode sample rates
// (128kHz default, 192kHz full-bandwidth option) since WavPack itself
// hadn't been exercised at the low rate before that option was added.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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

    // Sized generously; this test isn't exercising the bounded-latency drop
    // policy, just correctness of the encode/decode round trip.
    ByteRingBuffer ring(1024 * 1024);

    WavpackEncoder encoder;
    WavpackEncoder::Config enc_cfg;
    enc_cfg.sample_rate = sample_rate;
    enc_cfg.channels = kChannels;
    enc_cfg.bits_per_sample = kBpsDefault;
    enc_cfg.block_samples = kBlockSamples;

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

    std::printf("[%d Hz] Encoded %zu frames into %zu bytes (%.0f kbps).\n",
                sample_rate, original.size(), ring.availableBytes(),
                ring.availableBytes() * 8.0 / (static_cast<double>(original.size()) / sample_rate) / 1000.0);

    WavpackDecoder decoder;
    if (!decoder.open(&ring, /*read_timeout_ms=*/200)) {
        std::printf("[%d Hz] FAILED: decoder.open(): %s\n", sample_rate, decoder.lastError().c_str());
        return false;
    }

    std::vector<int32_t> decoded_buf(kBlockSamples * kChannels);
    std::vector<int16_t> decoded;
    decoded.reserve(original.size());

    // The ring buffer is finite and already fully populated (no live
    // producer), so once it runs dry decode() will start returning 0 —
    // that's expected end-of-stream here, not a stall to retry on.
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

    // Hybrid lossy mode does not round-trip bit-exact; check the frame count
    // is close (allow for the final short block) and that the signal isn't
    // structurally broken (gross amplitude/DC sanity check).
    if (decoded.size() < original.size() - kBlockSamples) {
        std::printf("[%d Hz] FAILED: decoded far fewer frames (%zu) than encoded (%zu)\n",
                    sample_rate, decoded.size(), original.size());
        return false;
    }

    double sum_abs_diff = 0;
    size_t compare_n = std::min(decoded.size(), original.size());
    for (size_t i = 0; i < compare_n; ++i) {
        sum_abs_diff += std::abs(static_cast<int>(decoded[i]) - static_cast<int>(original[i]));
    }
    double avg_abs_diff = sum_abs_diff / compare_n;
    std::printf("[%d Hz] Average |decoded - original| per sample: %.1f (out of 32767 full scale)\n",
                sample_rate, avg_abs_diff);

    if (decoder.lastErrorCount() > 0) {
        std::printf("[%d Hz] FAILED: libwavpack reported decode errors on a clean in-process stream.\n",
                    sample_rate);
        return false;
    }

    if (avg_abs_diff > 2000.0) { // generous bound; hybrid lossy error should be far smaller in practice
        std::printf("[%d Hz] FAILED: average error too large for hybrid lossy mode.\n", sample_rate);
        return false;
    }

    return true;
}

} // namespace

int main() {
    bool all_ok = true;
    for (int rate : {kEncodeSampleRateLow, kEncodeSampleRateHigh}) {
        if (!RunRoundTrip(rate)) all_ok = false;
    }

    std::printf(all_ok ? "PASS\n" : "FAILED\n");
    return all_ok ? 0 : 1;
}
