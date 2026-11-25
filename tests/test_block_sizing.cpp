// Empirically measures actual per-block byte sizes out of WavpackEncoder at
// each selectable bits-per-sample setting, to validate the block_samples=1536
// derivation in docs/phase2-plan.md (assumed ~90 bytes of WavPack overhead on
// top of the entropy-coded payload). Run standalone, no audio hardware
// needed — feeds a synthetic sweep straight into the encoder.

#include <cstdio>
#include <vector>

#include "app_config.h"
#include "synthetic_signal.h"
#include "udp_framing.h"
#include "wavpack_encoder.h"

using namespace mpx;

namespace {

struct BlockStats {
    size_t count = 0;
    size_t min_bytes = SIZE_MAX;
    size_t max_bytes = 0;
    size_t total_bytes = 0;
};

} // namespace

int main() {
    const int kNumFrames = kSampleRate * 2; // 2 seconds
    std::vector<int16_t> signal = GenerateSineSweep(kNumFrames, kSampleRate, 50.0, 90000.0);

    bool all_ok = true;

    for (float bps : {kBpsMin, 3.0f, kBpsDefault, kBpsMax, 6.0f}) {
        BlockStats stats;

        WavpackEncoder encoder;
        WavpackEncoder::Config cfg;
        cfg.sample_rate = kSampleRate;
        cfg.channels = kChannels;
        cfg.bits_per_sample = bps;
        cfg.block_samples = kBlockSamples;
        cfg.very_high_quality = true; // matches the shipped SenderApp config — see sender_app.cpp

        bool opened = encoder.open(cfg, [&](const uint8_t*, size_t len) {
            stats.count++;
            stats.min_bytes = std::min(stats.min_bytes, len);
            stats.max_bytes = std::max(stats.max_bytes, len);
            stats.total_bytes += len;
        });

        if (!opened) {
            std::printf("bps=%.2f: FAILED to open encoder: %s\n", bps, encoder.lastError().c_str());
            all_ok = false;
            continue;
        }

        // Feed in block_samples-sized chunks to mirror how the real capture
        // callback would deliver audio.
        size_t offset = 0;
        while (offset + kBlockSamples <= signal.size()) {
            encoder.pushSamples(signal.data() + offset, kBlockSamples);
            offset += kBlockSamples;
        }
        encoder.flush();
        encoder.close();

        double avg = stats.count ? static_cast<double>(stats.total_bytes) / stats.count : 0.0;
        size_t worst_with_framing = stats.max_bytes + sizeof(UdpPacketHeader);
        bool fits_one_packet = worst_with_framing <= kMaxUdpPacketBytes;

        std::printf("bps=%.2f: blocks=%zu min=%zu max=%zu avg=%.1f bytes | +UDP header=%zu | fits 1 packet (<=%zu)? %s\n",
                    bps, stats.count, stats.min_bytes, stats.max_bytes, avg,
                    worst_with_framing, kMaxUdpPacketBytes, fits_one_packet ? "yes" : "NO");

        if (!fits_one_packet) {
            std::printf("  NOTE: fragmentation path will be exercised for this bps at block_samples=%d.\n",
                        kBlockSamples);
        }
    }

    return all_ok ? 0 : 1;
}
