#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mpx {

struct AudioDeviceImpl; // defined in audio_device.cpp; kept out of the class body
                        // (rather than a nested type) so the miniaudio data
                        // callback, a free function, can access it without
                        // needing friend/class-member access.

struct AudioDeviceInfo {
    std::string id; // opaque, only valid against the enumerate() call that produced it
    std::string name;         // human-friendly description — NOT guaranteed unique (see native_id)
    std::string native_id;    // e.g. the raw ALSA PCM name ("hw:CARD=Loopback,DEV=0") when
                              // available. On ALSA, `name` is often an identical generic
                              // description ("Loopback Analogue Stereo") across every hw:/
                              // plughw:/dmix: variant of the same substream — this is the
                              // only thing that actually distinguishes them. Empty on
                              // backends (WASAPI, etc.) that don't have this ambiguity.
    bool is_default = false;

    // What the GUI should actually show — folds in native_id when it adds
    // real information, so duplicate-named devices remain distinguishable.
    std::string displayLabel() const {
        if (native_id.empty() || native_id == name) return name;
        return name + "  [" + native_id + "]";
    }
};

enum class AudioDeviceKind { Capture, Playback };

// Windows only — ignored (treated as Default) on other platforms, which
// already force a single specific backend of their own (see InitContext()
// in audio_device.cpp for the ALSA case on Linux). Default lets miniaudio's
// normal priority order pick (WASAPI first, falling through to DirectSound/
// MME only if WASAPI genuinely isn't available). The others force one
// specific backend with no fallback, so a real failure is loud rather than
// silently substituting a different one — same rationale as the Linux ALSA
// forcing.
//
// Mme (WinMM's waveOut/waveIn) is kept selectable, but it will never pass
// this app's 192kHz requirement in practice: verified with a real loopback
// tone test (tests/probe_mme_resample_check.cpp) that Windows' modern WinMM
// compatibility shim silently resamples/filters regardless of the
// requested rate — a 90kHz test tone vanished completely on capture even
// though ma_device_init() reported success at 192kHz, on a device
// independently confirmed via WASAPI to genuinely support it. So unlike
// the legacy-bitmask capability query merely under-reporting (which would
// have been a fixable false negative), this is an actual, confirmed hidden
// resample with no reliable way to detect it from open() succeeding —
// AudioDevice::supportsFormatNatively() deliberately does NOT special-case
// MME to work around this, since there's no trustworthy signal to fall
// back to. Selecting Mme here will correctly fail device validation (a
// manual override exists in the GUI for users who have independently
// verified their own setup — see SenderGui/ReceiverGui's mme_trust_override_).
//
// Dsound (DirectSound) sits structurally closer to WASAPI than MME here:
// miniaudio derives its native-format list from IDirectSound_GetCaps()'s
// dwMinSecondarySampleRate/dwMaxSecondarySampleRate — a genuine continuous
// range reported by the driver, not a fixed legacy bitmask with no room to
// express anything past 96kHz — so supportsFormatNatively() is NOT
// structurally blind here the way it is for Mme; a device that genuinely
// exposes 192kHz should be reported correctly. Still verify with
// tests/probe_mme_resample_check.cpp (pass "dsound") before trusting it in
// practice, the same way Mme's claim was checked and found wanting —
// DirectSound has always mixed secondary buffers through its own internal
// mixer even on original hardware, and on modern Windows is itself
// implemented on top of WASAPI, so a real end-to-end check matters more
// than the capability report alone.
enum class PreferredAudioBackend {
    Default,
    Wasapi,
    Mme,
    Dsound,
};

// One entry from a device's native-format table. A value of 0 for
// sample_rate/channels means "device accepts any" per miniaudio's
// ma_device_info convention.
struct NativeFormat {
    int sample_rate = 0;
    int channels = 0;
    int bits_per_sample = 0; // 0 = device accepts any sample format
};

// Wraps miniaudio for exactly what this project needs: enumerate devices,
// validate that a device natively supports our fixed 192kHz/mono/16-bit
// format (no silent resampling — see supportsFormatNatively()), and run a
// capture or playback callback at that format.
class AudioDevice {
public:
    // Delivered on miniaudio's real-time audio thread — must never block.
    using CaptureCallback = std::function<void(const int16_t* pcm, size_t frame_count)>;
    // Must fill up to frame_count frames and return how many it actually
    // wrote; the remainder is zero-filled (silence) on underrun.
    using PlaybackCallback = std::function<size_t(int16_t* pcm, size_t frame_count)>;

    AudioDevice();
    ~AudioDevice();
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    static std::vector<AudioDeviceInfo> enumerate(AudioDeviceKind kind);

    // Applies process-wide to every enumerate()/openCapture()/openPlayback()/
    // supportsFormatNatively()/listNativeFormats() call made from then on —
    // there's no ma_context per AudioDevice instance to scope it to more
    // tightly (see InitContext() in audio_device.cpp). Call this — then
    // re-enumerate — before opening any device, since a device's `id` is
    // only valid against the enumerate() call that produced it, and
    // switching backends can change the device list entirely.
    static void setPreferredBackend(PreferredAudioBackend backend);

    // True only if sample_rate is among the device's native formats — i.e.
    // opening it would need no implicit resampling. Deliberately does NOT
    // require an exact channel-count/bit-depth match: those are handled by
    // miniaudio's built-in format/channel converters, which just do bit-width
    // conversion and channel duplication/summing — no filtering involved, so
    // no risk to the RDS subcarrier region. Sample-rate mismatches are the
    // one case that matters, since that goes through miniaudio's resampler
    // (a real filter) instead. `channels` is
    // kept in the signature for API clarity/future use, not currently used
    // to reject a device.
    static bool supportsFormatNatively(AudioDeviceKind kind, const std::string& device_id,
                                       int sample_rate, int channels);

    // Full native-format table for a device — for diagnosing *why* a device
    // fails supportsFormatNatively() (wrong rate configured, wrong channel
    // count, etc.), not needed for normal sender/receiver operation.
    static std::vector<NativeFormat> listNativeFormats(AudioDeviceKind kind, const std::string& device_id);

    bool openCapture(const std::string& device_id, int sample_rate, int channels, CaptureCallback cb);
    bool openPlayback(const std::string& device_id, int sample_rate, int channels, PlaybackCallback cb);

    bool start();
    void stop();
    void close();

    const std::string& lastError() const { return last_error_; }

private:
    AudioDeviceImpl* impl_ = nullptr;
    std::string last_error_;
};

} // namespace mpx
