// Follow-up to test_quality_flags.cpp: that test only measured the noise
// floor right around the RDS band (57kHz). Noise shaping is fundamentally
// a *redistribution* — for a fixed hybrid bit budget, total quantization
// noise power is roughly conserved, just tilted from one part of the
// spectrum to another. This test sweeps the noise floor across the whole
// 0-96kHz band (at frequencies chosen to sit between our injected signal
// tones) to answer two distinct questions with real data:
//   1. Does noise shaping lower the noise floor broadly ("cleaner
//      overall"), or just move it around?
//   2. Does "very high quality + noise shaping" — the combo asked about —
//      actually help, hurt, or do nothing relative to very-high alone?

#include <algorithm>
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
constexpr double kRdsInjectionLevel = 0.035;

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

std::vector<int16_t> EncodeDecodeRoundTrip(const std::vector<int16_t>& original, int sample_rate,
                                            bool very_high, WavpackEncoder::Config::NoiseShaping shaping,
                                            float shaping_weight = 1.0f) {
    WavpackEncoder::Config cfg;
    cfg.sample_rate = sample_rate;
    cfg.channels = kChannels;
    cfg.bits_per_sample = kBpsDefault;
    cfg.block_samples = kBlockSamples;
    cfg.very_high_quality = very_high;
    cfg.noise_shaping = shaping;
    cfg.shaping_weight = shaping_weight;

    std::vector<uint8_t> encoded;
    encoded.reserve(original.size());

    WavpackEncoder encoder;
    if (!encoder.open(cfg, [&](const uint8_t* data, size_t len) {
            encoded.insert(encoded.end(), data, data + len);
        })) {
        return {};
    }

    size_t offset = 0;
    while (offset + kBlockSamples <= original.size()) {
        encoder.pushSamples(original.data() + offset, kBlockSamples);
        offset += kBlockSamples;
    }
    encoder.flush();
    encoder.close();

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
    return decoded;
}

} // namespace

int main() {
    const int kRate = kSampleRate; // 192000
    const size_t kNumFrames = static_cast<size_t>(kRate) * 3;
    std::vector<int16_t> original = GenerateCompositeMpx(kNumFrames, kRate);

    // Quiet probe points spanning DC to Nyquist, each chosen to sit well
    // clear of every injected signal tone (120..14500 program, 19000
    // pilot, 34000/42000 stereo stand-in, 57000 RDS) so what's measured is
    // pure quantization noise floor, not signal leakage from windowing.
    const double probes[] = {
        2000, 5000, 8000, 11000, 16000, 22000, 25000, 30000,
        38000, 46000, 50000, 53000, 61000, 65000, 70000, 80000, 88000, 92000
    };

    using NS = WavpackEncoder::Config::NoiseShaping;
    struct Combo { const char* label; bool very_high; NS shaping; float weight; };
    const Combo combos[] = {
        {"no shaping (forced off)",       false, NS::None,           0.0f},
        {"library default (old 'base')",  false, NS::LibraryDefault, 0.0f},
        {"dynamic shaping (explicit)",     false, NS::Dynamic,        0.0f},
        {"static shaping w=1.0",           false, NS::Static,         1.0f},
        {"very high, no shaping",          true,  NS::None,           0.0f},
        {"very high + dynamic shaping",    true,  NS::Dynamic,        0.0f},
        {"very high + static w=1.0",       true,  NS::Static,         1.0f},
    };

    std::vector<std::vector<double>> floor_db(sizeof(combos) / sizeof(combos[0]));
    std::vector<double> avg_db(sizeof(combos) / sizeof(combos[0]), 0.0);

    for (size_t c = 0; c < sizeof(combos) / sizeof(combos[0]); ++c) {
        std::vector<int16_t> decoded = EncodeDecodeRoundTrip(original, kRate, combos[c].very_high, combos[c].shaping, combos[c].weight);
        if (decoded.empty()) {
            std::printf("FAILED: round trip for %s\n", combos[c].label);
            return 1;
        }
        double sum_db = 0.0;
        for (double f : probes) {
            double mag = MeasureMagnitude(decoded, kRate, f);
            double db = mag > 1e-9 ? 20.0 * std::log10(mag) : -180.0;
            floor_db[c].push_back(db);
            sum_db += mag; // average in linear magnitude, report in dB after
        }
        avg_db[c] = 20.0 * std::log10(std::max(sum_db / (sizeof(probes) / sizeof(probes[0])), 1e-9));
    }

    std::printf("Noise floor (dBFS) at each probe frequency — lower is quieter:\n\n");
    std::printf("%8s", "Hz");
    for (const auto& combo : combos) std::printf(" %26s", combo.label);
    std::printf("\n");

    for (size_t p = 0; p < sizeof(probes) / sizeof(probes[0]); ++p) {
        std::printf("%8.0f", probes[p]);
        for (size_t c = 0; c < sizeof(combos) / sizeof(combos[0]); ++c) {
            std::printf(" %18.1f", floor_db[c][p]);
        }
        std::printf("\n");
    }

    std::printf("\n%8s", "AVERAGE");
    for (double v : avg_db) std::printf(" %18.1f", v);
    std::printf("\n");

    std::printf("\nRDS band (57kHz) sits between the 53000 Hz and 61000 Hz probe rows above.\n"
                "If shaping is a pure redistribution, the AVERAGE row should stay roughly flat\n"
                "across configs while individual rows shift — a broad drop in AVERAGE would mean\n"
                "very-high's better prediction is doing genuine work, not just reshaping noise.\n");
    return 0;
}
