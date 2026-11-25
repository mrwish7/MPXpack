#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct WavpackContext;

namespace mpx {

// Wraps WavPack's block-callback encoder API (WavpackOpenFileOutput) to
// compress a live, unbounded-length 192kHz mono 16-bit PCM stream in hybrid
// (lossy) mode. Each completed block is delivered synchronously to the sink
// supplied to open() — call sites are responsible for getting those bytes to
// the network quickly (the sink runs on whatever thread calls pushSamples()).
class WavpackEncoder {
public:
    struct Config {
        // WavPack only honors an explicit noise-shaping choice if
        // CONFIG_SHAPE_OVERRIDE is set alongside it (see pack_utils.c:327)
        // — without it, the library silently ignores CONFIG_HYBRID_SHAPE
        // and picks its own default, which for our sample rate/bps range
        // is usually to auto-enable CONFIG_DYNAMIC_SHAPING. LibraryDefault
        // below reproduces that (what every earlier benchmark in this
        // project actually used, unrealized); the other modes force a
        // specific, verifiable choice via CONFIG_SHAPE_OVERRIDE.
        enum class NoiseShaping {
            LibraryDefault, // don't touch shaping flags at all; let WavPack decide (usually -> dynamic, see above)
            None,           // force off: CONFIG_SHAPE_OVERRIDE with shaping_weight=0
            Static,         // force on at a fixed weight: CONFIG_SHAPE_OVERRIDE | CONFIG_HYBRID_SHAPE, uses shaping_weight
            Dynamic,        // force on, adaptive: CONFIG_DYNAMIC_SHAPING
        };

        int sample_rate = 192000;
        int channels = 1;
        float bits_per_sample = 4.0f;   // hybrid target, NOT kbps (see app_config.h); ignored when lossless
        int32_t block_samples = 1536;
        // Plain WavPack lossless compression instead of hybrid/lossy —
        // CONFIG_HYBRID_FLAG and bits_per_sample are skipped entirely.
        // noise_shaping below is a hybrid-only concept (shapes the hybrid
        // quantization noise) and is ignored here too; high_quality/
        // very_high_quality/extra_mode remain meaningful in both modes.
        bool lossless = false;

        // Extra quality knobs, all off/library-default here, which keeps
        // per-block metadata size predictable. Benchmarked in
        // tests/test_quality_flags.cpp and
        // tests/test_noise_floor_spectrum.cpp. (SenderApp turns on
        // very_high_quality — see sender_app.cpp.)
        bool high_quality = false;      // CONFIG_HIGH_FLAG
        bool very_high_quality = false; // CONFIG_HIGH_FLAG | CONFIG_VERY_HIGH_FLAG (WavPack sets both together)
        NoiseShaping noise_shaping = NoiseShaping::LibraryDefault;
        float shaping_weight = 1.0f;    // only used when noise_shaping == Static; range -1.0..1.0
        int extra_mode = 0;             // 0 = off, 1-6 = CONFIG_EXTRA_MODE with this xmode level
    };

    using BlockSink = std::function<void(const uint8_t* data, size_t len)>;

    WavpackEncoder() = default;
    ~WavpackEncoder();
    WavpackEncoder(const WavpackEncoder&) = delete;
    WavpackEncoder& operator=(const WavpackEncoder&) = delete;

    // Returns true on success; on failure, lastError() has details.
    bool open(const Config& cfg, BlockSink sink);

    // pcm holds `frame_count * cfg.channels` interleaved int16 samples.
    bool pushSamples(const int16_t* pcm, size_t frame_count);

    // Forces out a final, possibly short, block. Call before close() at the
    // end of a session so the last few samples aren't silently discarded.
    void flush();

    void close();

    const std::string& lastError() const { return last_error_; }

private:
    static int blockOutTrampoline(void* id, void* data, int32_t bcount);

    WavpackContext* ctx_ = nullptr;
    BlockSink sink_;
    std::string last_error_;
    std::vector<int32_t> widen_buf_;
    int channels_ = 1;
};

} // namespace mpx
