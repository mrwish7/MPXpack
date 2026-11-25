#include "wavpack_encoder.h"

#include "wavpack.h"

namespace mpx {

WavpackEncoder::~WavpackEncoder() {
    close();
}

int WavpackEncoder::blockOutTrampoline(void* id, void* data, int32_t bcount) {
    auto* self = static_cast<WavpackEncoder*>(id);
    if (self->sink_) {
        self->sink_(static_cast<const uint8_t*>(data), static_cast<size_t>(bcount));
    }
    return 1;
}

bool WavpackEncoder::open(const Config& cfg, BlockSink sink) {
    close();
    sink_ = std::move(sink);
    channels_ = cfg.channels;

    ctx_ = WavpackOpenFileOutput(&WavpackEncoder::blockOutTrampoline, this, nullptr);
    if (!ctx_) {
        last_error_ = "WavpackOpenFileOutput failed";
        return false;
    }

    WavpackConfig config{};
    config.flags = 0;
    if (!cfg.lossless) {
        config.bitrate = cfg.bits_per_sample;   // hybrid bits/sample mode (bitrate < 24 => not kbps)
        config.flags |= CONFIG_HYBRID_FLAG;
    }
    if (cfg.high_quality || cfg.very_high_quality) config.flags |= CONFIG_HIGH_FLAG;
    if (cfg.very_high_quality) config.flags |= CONFIG_VERY_HIGH_FLAG;
    if (!cfg.lossless) {
        // Noise shaping only means something for the hybrid quantizer —
        // plain lossless compression has no quantization noise to shape.
        switch (cfg.noise_shaping) {
            case WavpackEncoder::Config::NoiseShaping::LibraryDefault:
                break; // deliberately untouched — see the enum's doc comment
            case WavpackEncoder::Config::NoiseShaping::None:
                config.flags |= CONFIG_SHAPE_OVERRIDE;
                config.shaping_weight = 0.0f;
                break;
            case WavpackEncoder::Config::NoiseShaping::Static:
                config.flags |= CONFIG_SHAPE_OVERRIDE | CONFIG_HYBRID_SHAPE;
                config.shaping_weight = cfg.shaping_weight;
                break;
            case WavpackEncoder::Config::NoiseShaping::Dynamic:
                config.flags |= CONFIG_DYNAMIC_SHAPING;
                break;
        }
    }
    if (cfg.extra_mode > 0) {
        config.flags |= CONFIG_EXTRA_MODE;
        config.xmode = cfg.extra_mode;
    }
    config.bits_per_sample = 16;
    config.bytes_per_sample = 2;
    config.num_channels = cfg.channels;
    config.channel_mask = (cfg.channels == 1) ? 4 : 3; // Microsoft standard: mono=4, stereo=3
    config.sample_rate = cfg.sample_rate;
    config.block_samples = cfg.block_samples;

    if (!WavpackSetConfiguration(ctx_, &config, static_cast<uint32_t>(-1))) {
        last_error_ = WavpackGetErrorMessage(ctx_);
        close();
        return false;
    }

    if (!WavpackPackInit(ctx_)) {
        last_error_ = WavpackGetErrorMessage(ctx_);
        close();
        return false;
    }

    return true;
}

bool WavpackEncoder::pushSamples(const int16_t* pcm, size_t frame_count) {
    if (!ctx_) return false;

    const size_t sample_count = frame_count * static_cast<size_t>(channels_);
    if (widen_buf_.size() < sample_count) {
        widen_buf_.resize(sample_count);
    }
    for (size_t i = 0; i < sample_count; ++i) {
        widen_buf_[i] = static_cast<int32_t>(pcm[i]);
    }

    if (!WavpackPackSamples(ctx_, widen_buf_.data(), static_cast<uint32_t>(frame_count))) {
        last_error_ = WavpackGetErrorMessage(ctx_);
        return false;
    }
    return true;
}

void WavpackEncoder::flush() {
    if (ctx_) {
        WavpackFlushSamples(ctx_);
    }
}

void WavpackEncoder::close() {
    if (ctx_) {
        WavpackCloseFile(ctx_);
        ctx_ = nullptr;
    }
    sink_ = nullptr;
}

} // namespace mpx
