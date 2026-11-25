// Diagnostic tool, not a pass/fail test in the ctest sense (though it does
// assert and return non-zero on failure) — hardware-dependent, needs a
// loopback device pair (e.g. VB-Audio Hi-Fi Cable) to actually round-trip
// audio back for inspection.
//
// Verifies the empirically uncertain part of ReceiverApp's new output
// channel-select feature (receiver_app.cpp): not the injection arithmetic
// itself (pcm[i*2+offset]=sample, pcm[i*2+other]=0 — simple, and mirrors
// the already-proven sender-side capture-channel-extraction code), but
// whether the real Windows audio stack actually preserves that channel
// isolation end-to-end rather than something in WASAPI/the virtual cable
// driver silently bleeding or remixing across channels. Plays a tone
// injected into just one channel (bypassing ReceiverApp/WavPack/network
// entirely — this isolates the AudioDevice/driver layer specifically) and
// captures it back in stereo to check the other channel actually stays
// silent.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "app_config.h"
#include "audio_device.h"

using namespace mpx;

namespace {

constexpr double kPi = 3.14159265358979323846;

bool FindHiFiCableDevice(const std::vector<AudioDeviceInfo>& devices, std::string* out_id) {
    for (const auto& d : devices) {
        if (d.name.find("Hi-Fi Cable") != std::string::npos) {
            *out_id = d.id;
            return true;
        }
    }
    return false;
}

double Peak(const std::vector<int16_t>& channel_samples) {
    int peak = 0;
    for (int16_t v : channel_samples) peak = std::max(peak, std::abs(static_cast<int>(v)));
    return peak / 32768.0;
}

// Injects a 1kHz tone into `inject_channel` (0=left, 1=right) of a stereo
// playback stream, captures the loopback stereo input for ~1 second, and
// returns the de-interleaved peak level of each captured channel.
bool RunOneDirection(const std::string& playback_id, const std::string& capture_id,
                     int inject_channel, double* out_peak_left, double* out_peak_right) {
    AudioDevice playback, capture;
    std::vector<int16_t> captured; // interleaved stereo
    uint64_t phase = 0;

    bool play_opened = playback.openPlayback(
        playback_id, kSampleRate, 2,
        [&phase, inject_channel](int16_t* pcm, size_t frame_count) -> size_t {
            for (size_t i = 0; i < frame_count; ++i, ++phase) {
                double sample = 0.5 * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(phase) / kSampleRate);
                int16_t s16 = static_cast<int16_t>(sample * 32767.0);
                pcm[i * 2 + inject_channel] = s16;
                pcm[i * 2 + (1 - inject_channel)] = 0;
            }
            return frame_count;
        });
    if (!play_opened) {
        std::printf("FAILED: playback.openPlayback(): %s\n", playback.lastError().c_str());
        return false;
    }

    bool cap_opened = capture.openCapture(
        capture_id, kSampleRate, 2,
        [&captured](const int16_t* pcm, size_t frame_count) {
            captured.insert(captured.end(), pcm, pcm + frame_count * 2);
        });
    if (!cap_opened) {
        std::printf("FAILED: capture.openCapture(): %s\n", capture.lastError().c_str());
        return false;
    }

    if (!playback.start() || !capture.start()) {
        std::printf("FAILED: start() — playback err=%s capture err=%s\n",
                    playback.lastError().c_str(), capture.lastError().c_str());
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    playback.stop();
    capture.stop();
    playback.close();
    capture.close();

    if (captured.size() < 2) {
        std::printf("FAILED: captured no audio at all\n");
        return false;
    }

    // Trim the first/last 10% to avoid device startup/shutdown transients.
    size_t total_frames = captured.size() / 2;
    size_t margin = total_frames / 10;
    std::vector<int16_t> left, right;
    for (size_t i = margin; i < total_frames - margin; ++i) {
        left.push_back(captured[i * 2]);
        right.push_back(captured[i * 2 + 1]);
    }

    *out_peak_left = Peak(left);
    *out_peak_right = Peak(right);
    return true;
}

} // namespace

int main() {
    auto playback_devices = AudioDevice::enumerate(AudioDeviceKind::Playback);
    auto capture_devices = AudioDevice::enumerate(AudioDeviceKind::Capture);

    std::string playback_id, capture_id;
    if (!FindHiFiCableDevice(playback_devices, &playback_id) ||
        !FindHiFiCableDevice(capture_devices, &capture_id)) {
        std::printf("SKIPPED: no VB-Audio Hi-Fi Cable playback+capture device pair found.\n");
        return 0;
    }

    bool all_ok = true;

    double left_peak = 0, right_peak = 0;
    if (!RunOneDirection(playback_id, capture_id, /*inject_channel=*/0, &left_peak, &right_peak)) {
        return 1;
    }
    std::printf("Inject LEFT:  captured left_peak=%.4f right_peak=%.4f\n", left_peak, right_peak);
    if (left_peak < 0.1) { std::printf("FAILED: left channel too quiet — signal not reaching it\n"); all_ok = false; }
    if (right_peak > 0.02) { std::printf("FAILED: right channel not silent — channel isolation leaking\n"); all_ok = false; }

    double left_peak2 = 0, right_peak2 = 0;
    if (!RunOneDirection(playback_id, capture_id, /*inject_channel=*/1, &left_peak2, &right_peak2)) {
        return 1;
    }
    std::printf("Inject RIGHT: captured left_peak=%.4f right_peak=%.4f\n", left_peak2, right_peak2);
    if (right_peak2 < 0.1) { std::printf("FAILED: right channel too quiet — signal not reaching it\n"); all_ok = false; }
    if (left_peak2 > 0.02) { std::printf("FAILED: left channel not silent — channel isolation leaking\n"); all_ok = false; }

    if (!all_ok) {
        std::printf("FAILED\n");
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
