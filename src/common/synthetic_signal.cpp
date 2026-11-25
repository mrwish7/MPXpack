#include "synthetic_signal.h"

#include <cmath>

namespace mpx {

namespace {
constexpr double kPi = 3.14159265358979323846;
} // namespace

std::vector<int16_t> GenerateSineSweep(size_t num_frames, int sample_rate,
                                        double start_hz, double end_hz,
                                        double amplitude) {
    std::vector<int16_t> out(num_frames);
    const double duration = static_cast<double>(num_frames) / sample_rate;
    const double k = (end_hz - start_hz) / (duration > 0 ? duration : 1.0);
    double phase = 0.0;

    for (size_t n = 0; n < num_frames; ++n) {
        double t = static_cast<double>(n) / sample_rate;
        double instantaneous_freq = start_hz + k * t;
        phase += 2.0 * kPi * instantaneous_freq / sample_rate;
        double sample = amplitude * std::sin(phase);
        out[n] = static_cast<int16_t>(sample * 32767.0);
    }
    return out;
}

} // namespace mpx
