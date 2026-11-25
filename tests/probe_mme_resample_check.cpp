// Diagnostic: checks whether opening a device at 192kHz under a given
// non-WASAPI backend actually gets true 192kHz content through, or
// whether Windows' compatibility shim for that backend is silently
// resampling behind the scenes despite ma_device_init() reporting success.
// Checks BOTH directions independently — playback under the tested
// backend (what the receiver needs) and capture under it (what the sender
// needs) — since a backend can be honest one way and not the other.
// Plays/expects a 90kHz test tone (comfortably above 48kHz's Nyquist of
// 24kHz, so it could not survive a hidden downsample to 48kHz), with the
// other side of each check always on WASAPI (known-honest) as the ground
// truth for what actually happened.
//
// Usage: probe_mme_resample_check [mme|dsound]   (default: mme)

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "app_config.h"
#include "audio_device.h"

using namespace mpx;

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kToneHz = 90000.0;

bool FindHiFiCableDevice(AudioDeviceKind kind, std::string* out_id) {
    for (const auto& d : AudioDevice::enumerate(kind)) {
        if (d.name.find("Hi-Fi Cable") != std::string::npos) {
            *out_id = d.id;
            return true;
        }
    }
    return false;
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

// direction_label is just for the printout; test_playback selects whether
// the backend under test is on the playback side (receiver's concern) or
// the capture side (sender's concern) — the other side always runs WASAPI.
bool RunDirectionCheck(PreferredAudioBackend backend, const char* backend_name, bool test_playback) {
    std::string playback_id, capture_id;
    AudioDevice::setPreferredBackend(test_playback ? backend : PreferredAudioBackend::Wasapi);
    if (!FindHiFiCableDevice(AudioDeviceKind::Playback, &playback_id)) {
        std::printf("SKIPPED: no VB-Audio Hi-Fi Cable playback device found.\n");
        return true;
    }
    AudioDevice::setPreferredBackend(test_playback ? PreferredAudioBackend::Wasapi : backend);
    if (!FindHiFiCableDevice(AudioDeviceKind::Capture, &capture_id)) {
        std::printf("SKIPPED: no VB-Audio Hi-Fi Cable capture device found.\n");
        return true;
    }

    uint64_t phase = 0;
    AudioDevice::setPreferredBackend(test_playback ? backend : PreferredAudioBackend::Wasapi);
    AudioDevice playback;
    bool play_opened = playback.openPlayback(
        playback_id, kSampleRate, 1,
        [&phase](int16_t* pcm, size_t frame_count) -> size_t {
            for (size_t i = 0; i < frame_count; ++i, ++phase) {
                double sample = 0.5 * std::sin(2.0 * kPi * kToneHz * static_cast<double>(phase) / kSampleRate);
                pcm[i] = static_cast<int16_t>(sample * 32767.0);
            }
            return frame_count;
        });
    if (!play_opened) {
        std::printf("FAILED: playback.openPlayback() under %s: %s\n",
                    test_playback ? backend_name : "wasapi", playback.lastError().c_str());
        return false;
    }

    std::vector<int16_t> captured;
    AudioDevice::setPreferredBackend(test_playback ? PreferredAudioBackend::Wasapi : backend);
    AudioDevice capture;
    bool cap_opened = capture.openCapture(
        capture_id, kSampleRate, 1,
        [&captured](const int16_t* pcm, size_t frame_count) {
            captured.insert(captured.end(), pcm, pcm + frame_count);
        });
    if (!cap_opened) {
        std::printf("FAILED: capture.openCapture() under %s: %s\n",
                    test_playback ? "wasapi" : backend_name, capture.lastError().c_str());
        return false;
    }

    if (!playback.start() || !capture.start()) {
        std::printf("FAILED: start() — playback err=%s capture err=%s\n",
                    playback.lastError().c_str(), capture.lastError().c_str());
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    playback.stop();
    capture.stop();
    playback.close();
    capture.close();

    if (captured.size() < 2) {
        std::printf("FAILED: captured no audio at all\n");
        return false;
    }

    size_t margin = captured.size() / 10;
    std::vector<int16_t> trimmed(captured.begin() + margin, captured.end() - margin);
    double mag = MeasureMagnitude(trimmed, kSampleRate, kToneHz);

    const char* role = test_playback ? "PLAYBACK (receiver's concern)" : "CAPTURE (sender's concern)";
    std::printf("=== %s under %s ===\n", role, backend_name);
    std::printf("Captured %zu frames (at claimed %d Hz).\n", trimmed.size(), kSampleRate);
    std::printf("90kHz tone magnitude in captured signal: %.4f (sent at 0.5)\n", mag);
    if (mag > 0.1) {
        std::printf("-> Tone survived intact: honest passthrough, not a silently downsampled shim.\n\n");
    } else {
        std::printf("-> Tone did NOT survive: this direction silently resamples/filters despite\n"
                    "   reporting success at 192kHz.\n\n");
    }
    return true;
}
} // namespace

int main(int argc, char** argv) {
    const char* backend_name = (argc > 1) ? argv[1] : "mme";
    PreferredAudioBackend backend;
    if (std::strcmp(backend_name, "mme") == 0) backend = PreferredAudioBackend::Mme;
    else if (std::strcmp(backend_name, "dsound") == 0) backend = PreferredAudioBackend::Dsound;
    else {
        std::printf("usage: probe_mme_resample_check [mme|dsound]\n");
        return 1;
    }

    bool ok1 = RunDirectionCheck(backend, backend_name, /*test_playback=*/true);
    bool ok2 = RunDirectionCheck(backend, backend_name, /*test_playback=*/false);
    return (ok1 && ok2) ? 0 : 1;
}
