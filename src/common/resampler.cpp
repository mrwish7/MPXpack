#include "resampler.h"

#include <samplerate.h>

namespace mpx {

Resampler::~Resampler() {
    close();
}

bool Resampler::open(int in_rate, int out_rate, int channels) {
    close();
    channels_ = channels;
    ratio_ = static_cast<double>(out_rate) / static_cast<double>(in_rate);

    if (!src_is_valid_ratio(ratio_)) {
        last_error_ = "invalid resample ratio";
        return false;
    }

    int error = 0;
    state_ = src_new(SRC_SINC_BEST_QUALITY, channels_, &error);
    if (!state_) {
        last_error_ = src_strerror(error);
        return false;
    }
    return true;
}

void Resampler::close() {
    if (state_) {
        src_delete(state_);
        state_ = nullptr;
    }
}

bool Resampler::process(const int16_t* in, size_t in_frames, std::vector<int16_t>& out) {
    if (!state_) return false;
    if (in_frames == 0) return true;

    size_t in_samples = in_frames * static_cast<size_t>(channels_);
    in_float_buf_.resize(in_samples);
    src_short_to_float_array(in, in_float_buf_.data(), static_cast<int>(in_samples));

    // Generous output capacity margin: src_process is allowed to produce
    // less than the theoretical ratio*in_frames in a single call, so we
    // loop below rather than assume one call drains all the input.
    size_t max_out_frames = static_cast<size_t>(static_cast<double>(in_frames) * ratio_) + 256;
    out_float_buf_.resize(max_out_frames * static_cast<size_t>(channels_));

    long total_input_used = 0;
    while (total_input_used < static_cast<long>(in_frames)) {
        SRC_DATA data{};
        data.data_in = in_float_buf_.data() + static_cast<size_t>(total_input_used) * channels_;
        data.input_frames = static_cast<long>(in_frames) - total_input_used;
        data.data_out = out_float_buf_.data();
        data.output_frames = static_cast<long>(max_out_frames);
        data.src_ratio = ratio_;
        data.end_of_input = 0;

        if (src_process(state_, &data) != 0) {
            last_error_ = src_strerror(src_error(state_));
            return false;
        }

        size_t produced = static_cast<size_t>(data.output_frames_gen) * static_cast<size_t>(channels_);
        if (produced > 0) {
            size_t old_size = out.size();
            out.resize(old_size + produced);
            src_float_to_short_array(out_float_buf_.data(), out.data() + old_size, static_cast<int>(produced));
        }

        if (data.input_frames_used == 0 && data.output_frames_gen == 0) {
            break; // no progress this iteration; avoid spinning forever
        }
        total_input_used += data.input_frames_used;
    }

    return true;
}

bool Resampler::flush(std::vector<int16_t>& out) {
    if (!state_) return false;

    size_t max_out_frames = 4096;
    out_float_buf_.resize(max_out_frames * static_cast<size_t>(channels_));

    SRC_DATA data{};
    data.data_in = nullptr;
    data.input_frames = 0;
    data.data_out = out_float_buf_.data();
    data.output_frames = static_cast<long>(max_out_frames);
    data.src_ratio = ratio_;
    data.end_of_input = 1;

    if (src_process(state_, &data) != 0) {
        last_error_ = src_strerror(src_error(state_));
        return false;
    }

    size_t produced = static_cast<size_t>(data.output_frames_gen) * static_cast<size_t>(channels_);
    if (produced > 0) {
        size_t old_size = out.size();
        out.resize(old_size + produced);
        src_float_to_short_array(out_float_buf_.data(), out.data() + old_size, static_cast<int>(produced));
    }
    return true;
}

} // namespace mpx
