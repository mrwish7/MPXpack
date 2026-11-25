// Diagnostic (not a pass/fail test): lists every capture/playback device
// miniaudio can see, along with its full native-format table, so you can
// check whether a given device (e.g. a virtual audio cable) is actually
// configured for 192kHz mono 16-bit before wiring it into mpx_sender/
// mpx_receiver — see audio_device.h on why we insist on an exact native
// match rather than silently resampling.
//
// Usage: probe_audio_devices [default|wasapi|mme|dsound]
// Optional backend override (Windows only — see PreferredAudioBackend),
// so a device/format regression under a specific backend can be checked
// directly rather than assumed from the default backend's results.

#include <cstdio>
#include <cstring>

#include "app_config.h"
#include "audio_device.h"

using namespace mpx;

namespace {

void dump(AudioDeviceKind kind, const char* label) {
    std::printf("=== %s devices ===\n", label);
    auto devices = AudioDevice::enumerate(kind);
    if (devices.empty()) {
        std::printf("  (none found)\n\n");
        return;
    }

    for (const auto& dev : devices) {
        std::printf("[%s] %s%s\n", dev.id.c_str(), dev.name.c_str(), dev.is_default ? "  (default)" : "");
        if (!dev.native_id.empty() && dev.native_id != dev.name) {
            std::printf("    native id: %s\n", dev.native_id.c_str());
        }

        auto formats = AudioDevice::listNativeFormats(kind, dev.id);
        if (formats.empty()) {
            std::printf("    (no native format info reported)\n");
        }
        for (const auto& f : formats) {
            std::printf("    rate=%-8s channels=%-4s bits=%s\n",
                        f.sample_rate ? std::to_string(f.sample_rate).c_str() : "any",
                        f.channels ? std::to_string(f.channels).c_str() : "any",
                        f.bits_per_sample ? std::to_string(f.bits_per_sample).c_str() : "any");
        }

        bool rate_ok = AudioDevice::supportsFormatNatively(kind, dev.id, kSampleRate, kChannels);
        std::printf("    -> %d Hz natively supported (channels/bit-depth handled by conversion): %s\n\n",
                    kSampleRate, rate_ok ? "YES" : "no");
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        PreferredAudioBackend backend = PreferredAudioBackend::Default;
        if (std::strcmp(argv[1], "wasapi") == 0) backend = PreferredAudioBackend::Wasapi;
        else if (std::strcmp(argv[1], "mme") == 0) backend = PreferredAudioBackend::Mme;
        else if (std::strcmp(argv[1], "dsound") == 0) backend = PreferredAudioBackend::Dsound;
        else if (std::strcmp(argv[1], "default") != 0) {
            std::printf("usage: probe_audio_devices [default|wasapi|mme|dsound]\n");
            return 1;
        }
        std::printf("Backend override: %s\n\n", argv[1]);
        AudioDevice::setPreferredBackend(backend);
    }

    dump(AudioDeviceKind::Capture, "Capture");
    dump(AudioDeviceKind::Playback, "Playback");
    return 0;
}
