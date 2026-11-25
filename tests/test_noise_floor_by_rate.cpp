// Diagnostic tool, not a pass/fail test. Answers a specific question: at the
// *same* bits-per-sample setting, why does the 128kHz encode rate show a
// higher noise floor around the RDS band than 192kHz (as test_rds_snr.cpp's
// numbers already hinted at: bps=4.0 gives ~54.6dB RDS SNR at 128kHz vs
// ~69.5dB at 192kHz)?
//
// Two effects are in play, and this tool isolates the one that actually
// dominates:
//   1. Prediction gain: WavPack's hybrid mode is a *predictive* coder — it
//      predicts each sample from previous ones and quantizes the residual.
//      At a lower encode rate, adjacent samples are less redundant (each
//      one is a bigger step forward in time relative to the signal's real
//      bandwidth), so the predictor has less to work with and the residual
//      carries more genuine entropy for the same bit budget.
//   2. Noise-shaping placement: WavPack's noise shaping pushes quantization
//      noise toward the *top* of the encode band, away from (presumably)
//      more perceptually/structurally important lower frequencies. RDS at
//      57kHz sits at 57/64 = 89% of Nyquist at 128kHz, but only 57/96 = 59%
//      of Nyquist at 192kHz — much further from where shaped noise piles
//      up. Same bits-per-sample, very different position in the shaped
//      noise curve.
//
// This generates the composite MPX signal *natively* at each rate (not via
// the resampler) so the comparison isolates encode/decode+shaping behavior
// specifically, uncontaminated by any resampling question (already
// separately verified clean in test_resampler_passband.cpp /
// probe_real_mpx_resample.cpp).

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

// Matches production settings exactly (sender_app.cpp: very_high_quality =
// true, noise_shaping left at its default).
std::vector<int16_t> EncodeDecodeRoundTrip(const std::vector<int16_t>& original, int sample_rate, float bps) {
    WavpackEncoder::Config cfg;
    cfg.sample_rate = sample_rate;
    cfg.channels = kChannels;
    cfg.bits_per_sample = bps;
    cfg.block_samples = kBlockSamples;
    cfg.very_high_quality = true;

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
    const size_t kNumSeconds = 3;

    // Probe points shared between both rates (all comfortably below
    // 128kHz's 64kHz Nyquist), chosen clear of every injected tone.
    const double probes[] = {2000, 5000, 8000, 11000, 16000, 22000, 25000, 30000,
                              38000, 46000, 50000, 53000, 61000};

    struct RateSpec { const char* label; int rate; };
    const RateSpec rates[] = {
        {"128kHz", kEncodeSampleRateLow},
        {"192kHz", kEncodeSampleRateHigh},
    };

    printf("Where does the RDS frequency (57kHz) sit relative to each rate's Nyquist?\n");
    for (const auto& r : rates) {
        printf("  %s: Nyquist=%d Hz, 57000 Hz = %.0f%% of Nyquist\n",
               r.label, r.rate / 2, 100.0 * 57000.0 / (r.rate / 2.0));
    }
    printf("\n");

    for (float bps : {2.25f, kBpsDefault, 5.0f}) {
        printf("=== bits_per_sample = %.2f ===\n", bps);
        printf("%8s", "Hz");
        for (const auto& r : rates) printf(" %12s", r.label);
        printf("   delta (128-192)\n");

        std::vector<std::vector<double>> floor_db(sizeof(rates) / sizeof(rates[0]));
        std::vector<double> avg_lin(sizeof(rates) / sizeof(rates[0]), 0.0);

        for (size_t ri = 0; ri < sizeof(rates) / sizeof(rates[0]); ++ri) {
            size_t num_frames = static_cast<size_t>(rates[ri].rate) * kNumSeconds;
            std::vector<int16_t> original = GenerateCompositeMpx(num_frames, rates[ri].rate);
            std::vector<int16_t> decoded = EncodeDecodeRoundTrip(original, rates[ri].rate, bps);
            if (decoded.empty()) {
                printf("FAILED: round trip for %s at bps=%.2f\n", rates[ri].label, bps);
                return 1;
            }
            double sum_lin = 0.0;
            for (double f : probes) {
                double mag = MeasureMagnitude(decoded, rates[ri].rate, f);
                floor_db[ri].push_back(mag > 1e-9 ? 20.0 * std::log10(mag) : -180.0);
                sum_lin += mag;
            }
            avg_lin[ri] = sum_lin / (sizeof(probes) / sizeof(probes[0]));
        }

        for (size_t p = 0; p < sizeof(probes) / sizeof(probes[0]); ++p) {
            printf("%8.0f", probes[p]);
            for (size_t ri = 0; ri < sizeof(rates) / sizeof(rates[0]); ++ri) {
                printf(" %11.1f ", floor_db[ri][p]);
            }
            printf("   %+.1f dB\n", floor_db[0][p] - floor_db[1][p]);
        }
        double avg_db0 = 20.0 * std::log10(std::max(avg_lin[0], 1e-9));
        double avg_db1 = 20.0 * std::log10(std::max(avg_lin[1], 1e-9));
        printf("%8s %11.1f  %11.1f    %+.1f dB\n\n", "AVERAGE", avg_db0, avg_db1, avg_db0 - avg_db1);
    }

    printf("Positive delta = 128kHz noisier at that frequency than 192kHz, same bps.\n"
           "If the gap widens specifically near 53-61kHz (close to 128kHz's 64kHz\n"
           "Nyquist, but mid-band at 192kHz) rather than being uniform across all\n"
           "probes, that points at noise-shaping placement (shaped noise piles up\n"
           "near Nyquist, and RDS sits far closer to Nyquist at 128kHz) rather than\n"
           "a uniform loss of prediction gain.\n");
    return 0;
}
