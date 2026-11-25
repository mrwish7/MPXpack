// Answers a question the other tests don't: does WavPack's hybrid lossy
// bit-reduction leave enough SNR at 57kHz for RDS to remain demodulable?
// test_resampler_passband validated the anti-alias FILTER's passband
// (near-perfect); this test validates the actual lossy ENTROPY compression,
// which is a different mechanism — WavPack hybrid mode injects
// approximately flat quantization noise across the coded bandwidth (we
// deliberately do not enable its noise-shaping option, see
// wavpack_encoder.cpp), and RDS is a very low-amplitude subcarrier
// relative to full modulation, so it's the one most exposed to that noise
// floor.
//
// Builds a synthetic composite signal approximating a real FM MPX signal
// (broadband program audio + 19kHz pilot + stereo subcarrier energy + a
// 57kHz RDS tone at an assumed realistic injection level), runs it through
// the REAL pipeline components (Resampler + WavpackEncoder/Decoder) at
// each candidate bits-per-sample setting and both selectable encode rates,
// then measures the RDS tone's SNR (signal at 57kHz vs. the noise floor at
// nearby quiet frequencies) before and after the round trip.
//
// NOTE: assumes RDS injection at 3.5% of peak composite level (a commonly
// cited figure; real stations vary roughly 2-4.5%). Since SNR scales
// directly with tone amplitude, if your actual injection level differs by
// a factor of K, mentally adjust the reported dB figures by 20*log10(K).

#include <cmath>
#include <cstdio>
#include <vector>

#include "app_config.h"
#include "resampler.h"
#include "wavpack_decoder.h"
#include "wavpack_encoder.h"
#include "byte_ring_buffer.h"

using namespace mpx;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRdsInjectionLevel = 0.035; // 3.5% of full scale — see file header

std::vector<int16_t> GenerateCompositeMpx(size_t num_frames, int sample_rate) {
    std::vector<double> buf(num_frames, 0.0);

    // Broadband program audio (mono sum channel), approximated as a sum of
    // tones across the audio band rather than a single frequency — real
    // program material is wideband, which is what actually drives the
    // hybrid quantizer's noise floor, unlike a pure single tone.
    const double program_tones[] = {120, 300, 800, 1500, 3000, 6000, 9000, 12000, 14500};
    for (double f : program_tones) {
        for (size_t n = 0; n < num_frames; ++n) {
            buf[n] += 0.09 * std::sin(2.0 * kPi * f * n / sample_rate);
        }
    }

    // Pilot tone: 19kHz, ~9% injection (typical 8-10% per FM broadcast practice).
    for (size_t n = 0; n < num_frames; ++n) {
        buf[n] += 0.09 * std::sin(2.0 * kPi * 19000.0 * n / sample_rate);
    }

    // Stand-in for stereo (L-R) subcarrier energy around 38kHz — a real
    // DSB-SC subcarrier has sidebands across 23-53kHz; approximated here
    // with a couple of tones at comparable level to the program audio.
    for (double f : {34000.0, 42000.0}) {
        for (size_t n = 0; n < num_frames; ++n) {
            buf[n] += 0.08 * std::sin(2.0 * kPi * f * n / sample_rate);
        }
    }

    // RDS: 57kHz at the assumed injection level.
    for (size_t n = 0; n < num_frames; ++n) {
        buf[n] += kRdsInjectionLevel * std::sin(2.0 * kPi * 57000.0 * n / sample_rate);
    }

    // Normalize to a realistic near-full-modulation operating level (~90%
    // peak), which is common broadcast practice to maximize SNR.
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

// Noise floor near 57kHz, averaged over a few quiet frequencies that don't
// coincide with any of our injected tones or their near vicinity.
double MeasureNoiseFloor(const std::vector<int16_t>& signal, int sample_rate) {
    const double probe_freqs[] = {50000.0, 52500.0, 61500.0, 64000.0};
    double sum = 0.0;
    for (double f : probe_freqs) sum += MeasureMagnitude(signal, sample_rate, f);
    return sum / (sizeof(probe_freqs) / sizeof(probe_freqs[0]));
}

double ToDb(double ratio) {
    return ratio > 1e-9 ? 20.0 * std::log10(ratio) : -180.0;
}

// Runs the composite signal through the real encode/decode pipeline (plus
// resampling, when encode_rate != kSampleRate), matching what SenderApp/
// ReceiverApp actually do.
bool RunPipeline(const std::vector<int16_t>& original, int encode_rate, float bps,
                  std::vector<int16_t>& out) {
    std::vector<int16_t> pre_encode;
    const std::vector<int16_t>* to_encode = &original;

    Resampler down, up;
    bool resampling = (encode_rate != kSampleRate);
    if (resampling) {
        if (!down.open(kSampleRate, encode_rate, kChannels)) return false;
        std::vector<int16_t> tail;
        if (!down.process(original.data(), original.size(), pre_encode)) return false;
        if (!down.flush(tail)) return false;
        pre_encode.insert(pre_encode.end(), tail.begin(), tail.end());
        to_encode = &pre_encode;
    }

    ByteRingBuffer ring(4 * 1024 * 1024);
    WavpackEncoder encoder;
    WavpackEncoder::Config enc_cfg;
    enc_cfg.sample_rate = encode_rate;
    enc_cfg.channels = kChannels;
    enc_cfg.bits_per_sample = bps;
    enc_cfg.block_samples = kBlockSamples;

    if (!encoder.open(enc_cfg, [&](const uint8_t* data, size_t len) { ring.push(data, len); })) return false;

    size_t offset = 0;
    while (offset + kBlockSamples <= to_encode->size()) {
        if (!encoder.pushSamples(to_encode->data() + offset, kBlockSamples)) return false;
        offset += kBlockSamples;
    }
    encoder.flush();
    encoder.close();

    WavpackDecoder decoder;
    if (!decoder.open(&ring, 200)) return false;

    std::vector<int32_t> decode_buf(kBlockSamples * kChannels);
    std::vector<int16_t> decoded;
    decoded.reserve(to_encode->size());
    for (;;) {
        uint32_t got = decoder.decode(decode_buf.data(), kBlockSamples);
        if (got == 0) break;
        for (uint32_t i = 0; i < got; ++i) decoded.push_back(static_cast<int16_t>(decode_buf[i]));
    }
    decoder.close();

    if (!resampling) {
        out = std::move(decoded);
        return true;
    }

    if (!up.open(encode_rate, kSampleRate, kChannels)) return false;
    std::vector<int16_t> tail;
    if (!up.process(decoded.data(), decoded.size(), out)) return false;
    if (!up.flush(tail)) return false;
    out.insert(out.end(), tail.begin(), tail.end());
    return true;
}

} // namespace

int main() {
    const size_t kNumFrames = static_cast<size_t>(kSampleRate) * 2; // 2 seconds
    std::vector<int16_t> original = GenerateCompositeMpx(kNumFrames, kSampleRate);

    double orig_signal = MeasureMagnitude(original, kSampleRate, 57000.0);
    double orig_noise = MeasureNoiseFloor(original, kSampleRate);
    std::printf("Original signal (uncompressed): RDS magnitude=%.5f, noise floor=%.6f, SNR=%.1f dB\n\n",
                orig_signal, orig_noise, ToDb(orig_signal / std::max(orig_noise, 1e-9)));

    struct RateSpec { const char* label; int rate; };
    const RateSpec rates[] = {
        {"128kHz (default)", kEncodeSampleRateLow},
        {"192kHz (full bandwidth)", kEncodeSampleRateHigh},
    };
    const float bps_values[] = {kBpsMin, 3.0f, kBpsDefault, kBpsMax};

    for (const auto& r : rates) {
        std::printf("=== Encode rate: %s ===\n", r.label);
        for (float bps : bps_values) {
            std::vector<int16_t> result;
            if (!RunPipeline(original, r.rate, bps, result)) {
                std::printf("  bps=%.2f: FAILED (pipeline error)\n", bps);
                continue;
            }

            double sig = MeasureMagnitude(result, kSampleRate, 57000.0);
            double noise = MeasureNoiseFloor(result, kSampleRate);
            double snr_db = ToDb(sig / std::max(noise, 1e-9));
            double signal_retained_pct = 100.0 * sig / std::max(orig_signal, 1e-9);

            std::printf("  bps=%.2f (%d kbps): RDS magnitude=%.5f (%.0f%% of original), noise floor=%.6f, RDS SNR=%.1f dB\n",
                        bps, BpsToKbps(bps, r.rate), sig, signal_retained_pct, noise, snr_db);
        }
        std::printf("\n");
    }

    std::printf("(Informational only — no pass/fail threshold asserted; RDS decoders vary in\n"
                "their required SNR, and the 3.5%% injection-level assumption may not match\n"
                "your actual station. Read the SNR figures above directly.)\n");
    return 0;
}
