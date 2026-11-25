#pragma once

#include <cstdint>
#include <string>
#include <vector>

typedef struct SRC_STATE_tag SRC_STATE;

namespace mpx {

// Wraps libsamplerate's stateful streaming API (src_new/src_process) for
// continuous mono int16 PCM resampling between two fixed rates — the
// bridge between the fixed 192kHz hardware capture/playback rate and a
// selectable, lower encode/transmit sample rate (see app_config.h).
//
// Uses SRC_SINC_BEST_QUALITY: given that the entire reason this feature
// exists is to shave bitrate without disturbing the passband up to the RDS
// carrier region (~62kHz), correctness/quality of the anti-alias filtering
// matters far more here than CPU cost, and this runs on a background
// thread, not the real-time audio callback.
//
// Deliberately has no way to nudge the ratio at runtime. An earlier version
// did, specifically to let the receiver correct for clock drift between
// independent capture/playback hardware — that was removed after it was
// found to visibly wobble the RDS/stereo subcarriers in a real SDR capture:
// changing this ratio shifts every frequency component passing through it,
// so continuously recalculating an adjustment is genuine, ongoing frequency
// modulation of the exact carriers this app exists to relay intact. Any
// future drift-correction attempt needs a fundamentally different
// mechanism — not a small, "inaudible" tweak to this ratio, since
// inaudible-to-a-human-ear is the wrong bar for a signal whose purpose is
// carrying frequency-critical subcarriers to a PLL-based FM receiver.
class Resampler {
public:
    Resampler() = default;
    ~Resampler();
    Resampler(const Resampler&) = delete;
    Resampler& operator=(const Resampler&) = delete;

    bool open(int in_rate, int out_rate, int channels = 1);
    void close();

    // Feeds `in_frames` interleaved int16 frames; appends resampled output
    // frames to `out` (out is not cleared first — caller controls that).
    bool process(const int16_t* in, size_t in_frames, std::vector<int16_t>& out);

    // Call once at end-of-stream to flush any audio still buffered inside
    // the filter (matters for the tail of a session).
    bool flush(std::vector<int16_t>& out);

    double ratio() const { return ratio_; }
    const std::string& lastError() const { return last_error_; }

private:
    SRC_STATE* state_ = nullptr;
    double ratio_ = 1.0;
    int channels_ = 1;
    std::string last_error_;
    std::vector<float> in_float_buf_;
    std::vector<float> out_float_buf_;
};

} // namespace mpx
