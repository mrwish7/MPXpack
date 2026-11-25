// Measures the actual CPU cost and RDS-SNR effect of WavPack's optional
// quality flags (CONFIG_HIGH_FLAG/VERY_HIGH_FLAG, CONFIG_EXTRA_MODE,
// CONFIG_HYBRID_SHAPE) that are deliberately left off in the shipped
// encoder config (see wavpack_encoder.cpp). Answers two questions with
// real numbers instead of guessing:
//   1. How much slower is encoding with each flag combination?
//   2. Does noise shaping actually help or hurt RDS specifically? (Default
//      hybrid noise shaping pushes quantization noise toward *higher*
//      frequencies for human-hearing perceptual reasons — which is exactly
//      where the RDS carrier at 57kHz lives, so this could plausibly make
//      things worse for this application even though it's "higher
//      quality" in the general sense.)
//
// Uses the same composite-MPX synthetic signal and Goertzel-style
// magnitude measurement as test_rds_snr.cpp.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "app_config.h"
#include "byte_ring_buffer.h"
#include "wavpack_decoder.h"
#include "wavpack_encoder.h"

using namespace mpx;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRdsInjectionLevel = 0.035; // see test_rds_snr.cpp for rationale

std::vector<int16_t> GenerateCompositeMpx(size_t num_frames, int sample_rate) {
    std::vector<double> buf(num_frames, 0.0);

    const double program_tones[] = {120, 300, 800, 1500, 3000, 6000, 9000, 12000, 14500};
    for (double f : program_tones) {
        for (size_t n = 0; n < num_frames; ++n) {
            buf[n] += 0.09 * std::sin(2.0 * kPi * f * n / sample_rate);
        }
    }
    for (size_t n = 0; n < num_frames; ++n) {
        buf[n] += 0.09 * std::sin(2.0 * kPi * 19000.0 * n / sample_rate);
    }
    for (double f : {34000.0, 42000.0}) {
        for (size_t n = 0; n < num_frames; ++n) {
            buf[n] += 0.08 * std::sin(2.0 * kPi * f * n / sample_rate);
        }
    }
    for (size_t n = 0; n < num_frames; ++n) {
        buf[n] += kRdsInjectionLevel * std::sin(2.0 * kPi * 57000.0 * n / sample_rate);
    }

    double peak = 0.0;
    for (double v : buf) peak = std::max(peak, std::abs(v));
    double scale = (peak > 0.0) ? (0.90 / peak) : 1.0;

    std::vector<int16_t> out(num_frames);
    for (size_t n = 0; n < num_frames; ++n) {
        out[n] = static_cast<int16_t>(buf[n] * scale * 32767.0);
    }
    return out;
}

double MeasureMagnitude(const std::vector<int16_t>& signal, int sample_rate, double freq_hz) {
    size_t n = signal.size();
    if (n < 2) return 0.0;
    double sum_re = 0.0, sum_im = 0.0, window_sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (n - 1));
        double sample = (signal[i] / 32768.0) * w;
        double phase = 2.0 * kPi * freq_hz * i / sample_rate;
        sum_re += sample * std::cos(phase);
        sum_im -= sample * std::sin(phase);
        window_sum += w;
    }
    return std::sqrt(sum_re * sum_re + sum_im * sum_im) / (window_sum / 2.0);
}

double MeasureNoiseFloor(const std::vector<int16_t>& signal, int sample_rate) {
    const double probe_freqs[] = {50000.0, 52500.0, 61500.0, 64000.0};
    double sum = 0.0;
    for (double f : probe_freqs) sum += MeasureMagnitude(signal, sample_rate, f);
    return sum / (sizeof(probe_freqs) / sizeof(probe_freqs[0]));
}

double ToDb(double ratio) {
    return ratio > 1e-9 ? 20.0 * std::log10(ratio) : -180.0;
}

std::vector<int16_t> DecodeAll(const std::vector<uint8_t>& encoded, int sample_rate) {
    ByteRingBuffer ring(encoded.size() + 4096);
    ring.push(encoded.data(), encoded.size());

    WavpackDecoder decoder;
    std::vector<int16_t> decoded;
    if (!decoder.open(&ring, 200)) return decoded;

    std::vector<int32_t> buf(kBlockSamples * kChannels);
    for (;;) {
        uint32_t got = decoder.decode(buf.data(), kBlockSamples);
        if (got == 0) break;
        for (uint32_t i = 0; i < got; ++i) decoded.push_back(static_cast<int16_t>(buf[i]));
    }
    decoder.close();
    (void)sample_rate;
    return decoded;
}

struct FlagSpec {
    const char* label;
    bool high;
    bool very_high;
    WavpackEncoder::Config::NoiseShaping shaping;
    int extra_mode;
};

} // namespace

int main() {
    const int kRate = kSampleRate; // 192000
    const size_t kNumFrames = static_cast<size_t>(kRate) * 3; // 3 seconds — stable timing average
    std::vector<int16_t> original = GenerateCompositeMpx(kNumFrames, kRate);

    double orig_rds = MeasureMagnitude(original, kRate, 57000.0);

    using NS = WavpackEncoder::Config::NoiseShaping;
    const FlagSpec specs[] = {
        {"library default (old base)",  false, false, NS::LibraryDefault, 0},
        {"+ high quality",              true,  false, NS::LibraryDefault, 0},
        {"+ very high quality",         false, true,  NS::LibraryDefault, 0},
        {"no shaping (forced off)",     false, false, NS::None,           0},
        {"+ dynamic shaping (explicit)",false, false, NS::Dynamic,        0},
        {"+ very high + no shaping",    false, true,  NS::None,           0},
        {"+ very high + dynamic shape", false, true,  NS::Dynamic,        0},
        {"+ extra mode x1",             false, false, NS::LibraryDefault, 1},
        {"+ extra mode x2",             false, false, NS::LibraryDefault, 2},
        {"+ extra mode x6 (max)",       false, false, NS::LibraryDefault, 6},
        {"+ very high + extra x2",      false, true,  NS::LibraryDefault, 2},
    };

    std::printf("%-28s %9s %8s %11s %10s %12s\n",
                "config", "enc ms", "vs base", "max block", "RDS SNR", "RDS kept%");

    double baseline_ms = 0.0;

    for (size_t idx = 0; idx < sizeof(specs) / sizeof(specs[0]); ++idx) {
        const auto& spec = specs[idx];

        WavpackEncoder::Config cfg;
        cfg.sample_rate = kRate;
        cfg.channels = kChannels;
        cfg.bits_per_sample = kBpsDefault;
        cfg.block_samples = kBlockSamples;
        cfg.high_quality = spec.high;
        cfg.very_high_quality = spec.very_high;
        cfg.noise_shaping = spec.shaping;
        cfg.extra_mode = spec.extra_mode;

        size_t max_block = 0;
        std::vector<uint8_t> encoded;
        encoded.reserve(original.size());

        WavpackEncoder encoder;
        bool opened = encoder.open(cfg, [&](const uint8_t* data, size_t len) {
            max_block = std::max(max_block, len);
            encoded.insert(encoded.end(), data, data + len);
        });
        if (!opened) {
            std::printf("%-28s FAILED to open: %s\n", spec.label, encoder.lastError().c_str());
            continue;
        }

        auto t0 = std::chrono::steady_clock::now();
        size_t offset = 0;
        while (offset + kBlockSamples <= original.size()) {
            encoder.pushSamples(original.data() + offset, kBlockSamples);
            offset += kBlockSamples;
        }
        encoder.flush();
        auto t1 = std::chrono::steady_clock::now();
        encoder.close();

        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (idx == 0) baseline_ms = ms;
        double vs_base = baseline_ms > 0 ? ms / baseline_ms : 1.0;

        std::vector<int16_t> decoded = DecodeAll(encoded, kRate);
        double rds_sig = MeasureMagnitude(decoded, kRate, 57000.0);
        double rds_noise = MeasureNoiseFloor(decoded, kRate);
        double snr_db = ToDb(rds_sig / std::max(rds_noise, 1e-9));
        double kept_pct = 100.0 * rds_sig / std::max(orig_rds, 1e-9);

        std::printf("%-28s %9.1f %7.2fx %11zu %9.1fdB %11.0f%%\n",
                    spec.label, ms, vs_base, max_block, snr_db, kept_pct);
    }

    std::printf("\n(Single-threaded encode-only timing on this machine, 3s of audio per row.\n"
                "'max block' matters for whether a config still fits one UDP datagram —\n"
                "kMaxUdpPacketBytes is 1200.)\n");
    return 0;
}
