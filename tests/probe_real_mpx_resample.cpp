// Diagnostic tool, not a pass/fail test. Loads a real captured MPX WAV file
// (mono 16-bit PCM at 192kHz — see app_config.h) and runs it through the
// actual Resampler at several candidate encode rates, chunked the same way
// production code chunks it (see sender_app.cpp's kCaptureChunkFrames /
// receiver_app.cpp's kPlaybackChunkFrames), to answer two concrete safety
// questions on real content rather than synthetic tones:
//   1. Does the anti-alias filter clip/saturate the signal (sample values
//      pinned at +-32767/-32768)?
//   2. How much of the RDS-band (57kHz +- ~2.4kHz occupied bandwidth) energy
//      survives a full down/up round trip, and is there real energy in this
//      file at the frequencies that would alias into that band at each
//      candidate rate?
//
// Usage: probe_real_mpx_resample <path-to-mpx.wav>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

#include "app_config.h"
#include "resampler.h"

using namespace mpx;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kChunkFrames = 480; // matches kCaptureChunkFrames / kPlaybackChunkFrames in production

// Minimal RIFF/WAVE reader: mono 16-bit PCM only (integer PCM tag 1, or
// WAVE_FORMAT_EXTENSIBLE tag 0xFFFE with a 16-bit PCM subformat). Skips any
// chunk it doesn't recognize (LIST, fact, etc.) rather than assuming a fixed
// layout, since real-world WAV files routinely carry extra chunks.
bool LoadMonoWav16(const std::string& path, int* out_rate, std::vector<int16_t>* out_samples) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    char tag[4];
    uint32_t riff_size = 0;
    f.read(tag, 4);
    if (std::memcmp(tag, "RIFF", 4) != 0) return false;
    f.read(reinterpret_cast<char*>(&riff_size), 4);
    f.read(tag, 4);
    if (std::memcmp(tag, "WAVE", 4) != 0) return false;

    bool have_fmt = false;
    uint16_t channels = 0, bits_per_sample = 0;
    uint32_t sample_rate = 0;

    while (f) {
        char chunk_id[4];
        uint32_t chunk_size = 0;
        f.read(chunk_id, 4);
        if (!f) break;
        f.read(reinterpret_cast<char*>(&chunk_size), 4);
        if (!f) break;

        if (std::memcmp(chunk_id, "fmt ", 4) == 0) {
            uint16_t fmt_tag = 0;
            f.read(reinterpret_cast<char*>(&fmt_tag), 2);
            f.read(reinterpret_cast<char*>(&channels), 2);
            f.read(reinterpret_cast<char*>(&sample_rate), 4);
            uint32_t byte_rate = 0;
            uint16_t block_align = 0;
            f.read(reinterpret_cast<char*>(&byte_rate), 4);
            f.read(reinterpret_cast<char*>(&block_align), 2);
            f.read(reinterpret_cast<char*>(&bits_per_sample), 2);
            uint32_t consumed = 16;
            if (chunk_size > consumed) f.seekg(chunk_size - consumed, std::ios::cur);
            have_fmt = (fmt_tag == 1 || fmt_tag == 0xFFFE) && bits_per_sample == 16;
        } else if (std::memcmp(chunk_id, "data", 4) == 0) {
            if (!have_fmt || channels != 1) return false;
            size_t num_samples = chunk_size / sizeof(int16_t);
            out_samples->resize(num_samples);
            f.read(reinterpret_cast<char*>(out_samples->data()), static_cast<std::streamsize>(chunk_size));
            *out_rate = static_cast<int>(sample_rate);
            return true; // data chunk found and read — done
        } else {
            uint32_t skip = chunk_size + (chunk_size % 2); // chunks are word-aligned
            f.seekg(skip, std::ios::cur);
        }
    }
    return false;
}

// Whole-buffer Hann-windowed Goertzel magnitude — same technique used in
// test_resampler_passband.cpp / test_rds_snr.cpp.
double MeasureMagnitude(const int16_t* signal, size_t n, int sample_rate, double freq_hz) {
    if (n < 2) return 0.0;
    double sum_re = 0.0, sum_im = 0.0, window_sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double w = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / (n - 1));
        double sample = (signal[i] / 32768.0) * w;
        double phase = 2.0 * kPi * freq_hz * static_cast<double>(i) / sample_rate;
        sum_re += sample * std::cos(phase);
        sum_im -= sample * std::sin(phase);
        window_sum += w;
    }
    return std::sqrt(sum_re * sum_re + sum_im * sum_im) / (window_sum / 2.0);
}

double BandMagnitude(const int16_t* signal, size_t n, int sample_rate, double lo_hz, double hi_hz, double step_hz) {
    double sum_sq = 0.0;
    for (double f = lo_hz; f <= hi_hz; f += step_hz) {
        double m = MeasureMagnitude(signal, n, sample_rate, f);
        sum_sq += m * m;
    }
    return std::sqrt(sum_sq);
}

size_t CountClipped(const std::vector<int16_t>& s) {
    size_t n = 0;
    for (int16_t v : s) {
        if (v >= 32767 || v <= -32768) ++n;
    }
    return n;
}

double PeakNormalized(const int16_t* s, size_t n) {
    int peak = 0;
    for (size_t i = 0; i < n; ++i) peak = std::max(peak, std::abs(static_cast<int>(s[i])));
    return peak / 32768.0;
}

bool ChunkedResample(Resampler& r, const std::vector<int16_t>& in, std::vector<int16_t>* out) {
    for (size_t pos = 0; pos < in.size(); pos += kChunkFrames) {
        size_t n = std::min<size_t>(kChunkFrames, in.size() - pos);
        if (!r.process(in.data() + pos, n, *out)) return false;
    }
    std::vector<int16_t> tail;
    if (!r.flush(tail)) return false;
    out->insert(out->end(), tail.begin(), tail.end());
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: probe_real_mpx_resample <path-to-mpx.wav>\n");
        return 1;
    }

    int wav_rate = 0;
    std::vector<int16_t> original;
    if (!LoadMonoWav16(argv[1], &wav_rate, &original)) {
        std::printf("FAILED to load '%s' as mono 16-bit PCM WAV\n", argv[1]);
        return 1;
    }
    if (wav_rate != kSampleRate) {
        std::printf("NOTE: file is %d Hz, not the expected %d Hz — results below assume %d Hz.\n",
                     wav_rate, kSampleRate, wav_rate);
    }

    double duration_s = static_cast<double>(original.size()) / wav_rate;
    std::printf("Loaded '%s': %zu samples, %d Hz, %.2f sec\n", argv[1], original.size(), wav_rate, duration_s);
    std::printf("  original peak=%.4f  clipped_samples=%zu\n\n",
                PeakNormalized(original.data(), original.size()), CountClipped(original));

    const int candidate_rates[] = {128000, 130000, 132000, 136000, 140000, 144000};

    std::printf("%-10s %8s %8s %10s %10s %10s %10s %10s %10s\n",
                "rate", "peak_mid", "peak_rt", "clip_mid", "clip_rt",
                "1k_ratio", "19k_ratio", "rds_ratio", "alias_mag");
    std::printf("(rds_ratio = RDS-band [54.6-59.4kHz] magnitude, after round-trip / before)\n");
    std::printf("(alias_mag = original's magnitude at the frequency that folds onto 57kHz at this rate)\n\n");

    for (int rate : candidate_rates) {
        Resampler down, up;
        if (!down.open(wav_rate, rate, kChannels) || !up.open(rate, wav_rate, kChannels)) {
            std::printf("%-10d FAILED to open resampler\n", rate);
            continue;
        }

        std::vector<int16_t> mid, roundtrip;
        if (!ChunkedResample(down, original, &mid) || !ChunkedResample(up, mid, &roundtrip)) {
            std::printf("%-10d FAILED during resample\n", rate);
            continue;
        }

        size_t compare_n = std::min(original.size(), roundtrip.size());
        size_t margin = compare_n / 10;
        const int16_t* orig_trim = original.data() + margin;
        const int16_t* rt_trim = roundtrip.data() + margin;
        size_t trim_n = compare_n - 2 * margin;

        double mag_1k_before = MeasureMagnitude(orig_trim, trim_n, wav_rate, 1000.0);
        double mag_1k_after = MeasureMagnitude(rt_trim, trim_n, wav_rate, 1000.0);
        double mag_19k_before = MeasureMagnitude(orig_trim, trim_n, wav_rate, 19000.0);
        double mag_19k_after = MeasureMagnitude(rt_trim, trim_n, wav_rate, 19000.0);
        double rds_before = BandMagnitude(orig_trim, trim_n, wav_rate, 54600.0, 59400.0, 600.0);
        double rds_after = BandMagnitude(rt_trim, trim_n, wav_rate, 54600.0, 59400.0, 600.0);

        double mirror_freq = rate - 57000.0; // folds onto 57kHz when downsampled to `rate`
        double alias_source_mag = (mirror_freq > 0 && mirror_freq < wav_rate / 2.0)
                                       ? MeasureMagnitude(orig_trim, trim_n, wav_rate, mirror_freq)
                                       : 0.0;

        double ratio_1k = mag_1k_before > 1e-6 ? mag_1k_after / mag_1k_before : 0.0;
        double ratio_19k = mag_19k_before > 1e-6 ? mag_19k_after / mag_19k_before : 0.0;
        double ratio_rds = rds_before > 1e-6 ? rds_after / rds_before : 0.0;

        std::printf("%-10d %8.4f %8.4f %10zu %10zu %10.3f %10.3f %10.3f %10.5f\n",
                    rate, PeakNormalized(mid.data(), mid.size()), PeakNormalized(roundtrip.data(), roundtrip.size()),
                    CountClipped(mid), CountClipped(roundtrip), ratio_1k, ratio_19k, ratio_rds, alias_source_mag);
        std::printf("           raw: 1k=%.6f->%.6f  19k=%.6f->%.6f  rds=%.6f->%.6f\n",
                    mag_1k_before, mag_1k_after, mag_19k_before, mag_19k_after, rds_before, rds_after);
    }

    return 0;
}
