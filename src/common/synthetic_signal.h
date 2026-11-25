#pragma once

#include <cstdint>
#include <vector>

namespace mpx {

// Generates a mono int16 sine sweep from start_hz to end_hz over
// num_frames samples at sample_rate — used both by the test tools (no
// audio hardware needed) and by the sender's dev-only synthetic-source GUI
// mode.
std::vector<int16_t> GenerateSineSweep(size_t num_frames, int sample_rate,
                                        double start_hz, double end_hz,
                                        double amplitude = 0.8);

} // namespace mpx
