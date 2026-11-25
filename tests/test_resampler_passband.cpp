// The single most safety-critical numeric check in this project: verifies
// the 192kHz<->kEncodeSampleRateLow resampler (the sender's optional lower
// encode rate, see app_config.h) preserves FM MPX passband content up to
// the RDS carrier region (~62kHz) with negligible attenuation, and
// properly filters — rather than aliases — content above the low rate's
// Nyquist back into the passband. If this regresses, RDS decoding on the
// far end could silently break with no other symptom. See also
// tests/probe_real_mpx_resample.cpp, which checks the same thing against a
// real captured MPX+RDS file rather than synthetic tones.
//
// Tests the Resampler in isolation (no WavPack encode/decode involved) so
// a failure here can't be confused with hybrid-lossy quantization noise,
// which is already covered by test_encode_decode_roundtrip.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "app_config.h"
#include "resampler.h"

using namespace mpx;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<int16_t> GenerateTone(size_t num_frames, int sample_rate, double freq_hz, double amplitude = 0.5) {
    std::vector<int16_t> out(num_frames);
    for (size_t n = 0; n < num_frames; ++n) {
        double sample = amplitude * std::sin(2.0 * kPi * freq_hz * static_cast<double>(n) / sample_rate);
        out[n] = static_cast<int16_t>(sample * 32767.0);
    }
    return out;
}

// Single-frequency (Goertzel-style) magnitude estimate over a Hann-windowed
// buffer — good enough for a pure-tone synthetic test without pulling in an
// FFT dependency.
double MeasureMagnitude(const std::vector<int16_t>& signal, int sample_rate, double freq_hz) {
    size_t n = signal.size();
    if (n < 2) return 0.0;
    double sum_re = 0.0, sum_im = 0.0, window_sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double w = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / (n - 1)); // Hann
        double sample = (signal[i] / 32768.0) * w;
        double phase = 2.0 * kPi * freq_hz * static_cast<double>(i) / sample_rate;
        sum_re += sample * std::cos(phase);
        sum_im -= sample * std::sin(phase);
        window_sum += w;
    }
    return std::sqrt(sum_re * sum_re + sum_im * sum_im) / (window_sum / 2.0);
}

// Runs `signal` (at kInRate) down to kMidRate and back up to kInRate,
// trimming filter-settling transients from both ends before comparison.
bool RoundTrip(const std::vector<int16_t>& original, int in_rate, int mid_rate,
               std::vector<int16_t>& before_trimmed, std::vector<int16_t>& after_trimmed) {
    Resampler down, up;
    if (!down.open(in_rate, mid_rate, kChannels) || !up.open(mid_rate, in_rate, kChannels)) {
        return false;
    }

    std::vector<int16_t> mid, mid_tail, roundtrip, up_tail;
    if (!down.process(original.data(), original.size(), mid)) return false;
    if (!down.flush(mid_tail)) return false;
    mid.insert(mid.end(), mid_tail.begin(), mid_tail.end());

    if (!up.process(mid.data(), mid.size(), roundtrip)) return false;
    if (!up.flush(up_tail)) return false;
    roundtrip.insert(roundtrip.end(), up_tail.begin(), up_tail.end());

    size_t compare_n = std::min(original.size(), roundtrip.size());
    size_t margin = compare_n / 10; // skip 10% at each end (filter startup/settling)
    if (compare_n <= 2 * margin) return false;

    before_trimmed.assign(original.begin() + margin, original.begin() + compare_n - margin);
    after_trimmed.assign(roundtrip.begin() + margin, roundtrip.begin() + compare_n - margin);
    return true;
}

} // namespace

int main() {
    const int kInRate = kSampleRate;            // 192000
    const int kMidRate = kEncodeSampleRateLow;  // 128000
    const size_t kNumFrames = static_cast<size_t>(kInRate); // 1 second

    struct ToneSpec { const char* label; double freq; double min_ratio; };
    const ToneSpec passband_tones[] = {
        {"1kHz (mono audio)",         1000.0,  0.90},
        {"19kHz (pilot tone)",        19000.0, 0.90},
        {"38kHz (stereo subcarrier)", 38000.0, 0.90},
        {"57kHz (RDS carrier)",       57000.0, 0.85},
        {"61kHz (RDS edge margin)",   61000.0, 0.85},
    };

    bool all_ok = true;

    for (const auto& t : passband_tones) {
        std::vector<int16_t> original = GenerateTone(kNumFrames, kInRate, t.freq);
        std::vector<int16_t> before, after;
        if (!RoundTrip(original, kInRate, kMidRate, before, after)) {
            std::printf("FAILED: round trip setup failed for %s\n", t.label);
            all_ok = false;
            continue;
        }

        double mag_before = MeasureMagnitude(before, kInRate, t.freq);
        double mag_after = MeasureMagnitude(after, kInRate, t.freq);
        double ratio = mag_before > 1e-6 ? mag_after / mag_before : 0.0;
        bool ok = ratio >= t.min_ratio;
        std::printf("%-28s %6.0f Hz: before=%.4f after=%.4f ratio=%.3f (need >= %.2f) -> %s\n",
                    t.label, t.freq, mag_before, mag_after, ratio, t.min_ratio, ok ? "OK" : "FAIL");
        if (!ok) all_ok = false;
    }

    // Out-of-band tone at the exact frequency that folds onto 57kHz when
    // downsampled to kMidRate (mirror_freq = kMidRate - 57000): the single
    // most direct check that the anti-alias filter's margin (only ~2kHz at
    // 128kHz) actually holds, rather than an arbitrary above-Nyquist tone.
    // Must be substantially attenuated by the filter, not preserved, and
    // critically must not reappear aliased back into the RDS band.
    {
        const double freq = kMidRate - 57000.0;
        std::vector<int16_t> original = GenerateTone(kNumFrames, kInRate, freq);
        std::vector<int16_t> before, after;
        if (!RoundTrip(original, kInRate, kMidRate, before, after)) {
            std::printf("FAILED: round trip setup failed for RDS-mirror-frequency check\n");
            all_ok = false;
        } else {
            double mag_before = MeasureMagnitude(before, kInRate, freq);
            double mag_after = MeasureMagnitude(after, kInRate, freq);
            double ratio = mag_before > 1e-6 ? mag_after / mag_before : 0.0;
            bool ok = ratio < 0.1; // must be knocked down >20dB, not preserved/aliased through
            std::printf("%-28s %6.0f Hz: before=%.4f after=%.4f ratio=%.3f (need < 0.10)  -> %s\n",
                        "RDS-mirror (above Nyquist)", freq, mag_before, mag_after, ratio, ok ? "OK" : "FAIL");
            if (!ok) all_ok = false;

            double leaked_57k = MeasureMagnitude(after, kInRate, 57000.0);
            bool no_alias = leaked_57k < 0.02;
            std::printf("  -> aliased energy at 57kHz from the %.0fHz tone: %.4f (need < 0.02) -> %s\n",
                        freq, leaked_57k, no_alias ? "OK" : "FAIL");
            if (!no_alias) all_ok = false;
        }
    }

    if (!all_ok) {
        std::printf("FAILED\n");
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
