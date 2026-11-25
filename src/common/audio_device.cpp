#include "audio_device.h"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <cstring>
#include <vector>

namespace mpx {

namespace {
// Cached ma_device_id list from the most recent enumerate() call for each
// kind — AudioDeviceInfo::id is just a stringified index into this cache
// (see audio_device.h: "opaque, only valid against the enumerate() call
// that produced it").
std::vector<ma_device_id> g_capture_ids;
std::vector<ma_device_id> g_playback_ids;

// See AudioDevice::setPreferredBackend() / PreferredAudioBackend's doc
// comment in audio_device.h.
PreferredAudioBackend g_preferred_backend = PreferredAudioBackend::Default;

// By default, miniaudio's ALSA backend collapses every "hw:"/"plughw:"/
// "dmix:"/"dsnoop:" alias for a given card/device pair into a single
// enumerated entry and silently picks one internally at open time. That's
// fine for a normal soundcard, but it means the caller has no way to
// specifically pick, say, "hw:CARD=Loopback,DEV=0" (direct hardware
// access, no ALSA-side conversion) versus "plughw:CARD=Loopback,DEV=0"
// (goes through ALSA's software "plug" layer, which can silently resample/
// convert). Setting useVerboseDeviceEnumeration makes ALSA enumerate every
// registered PCM name separately instead, so both variants show up as
// distinct, individually selectable devices. This is a real, documented
// miniaudio option — not a workaround — and it's a complete no-op on
// non-ALSA backends (WASAPI, CoreAudio, etc.), so it's always safe to set.
ma_context_config MakeContextConfig() {
    ma_context_config cfg = ma_context_config_init();
    cfg.alsa.useVerboseDeviceEnumeration = MA_TRUE;
    return cfg;
}

// miniaudio's default backend priority order (used whenever a NULL list is
// passed to ma_context_init) tries PulseAudio *before* ALSA on Linux — and
// PulseAudio, or the PipeWire-pulse compatibility layer present on
// virtually every desktop distro, is present and running on almost every
// real system. That silently defeats two things at once: the
// useVerboseDeviceEnumeration option above (ALSA-specific; PulseAudio
// ignores it and exposes its own sink/source names with no hw:/plughw:
// distinction at all), and this project's "exact native format, no silent
// resampling" requirement (Pulse/PipeWire happily resamples to whatever
// its own server is configured for, rather than reporting the underlying
// hardware's true capability). Forcing ma_backend_alsa specifically — with
// no fallback — makes format validation fail loudly if ALSA genuinely
// isn't available, rather than silently talking to Pulse/PipeWire instead.
ma_result InitContext(ma_context* context) {
    ma_context_config cfg = MakeContextConfig();
#if defined(__linux__)
    static const ma_backend kBackends[] = { ma_backend_alsa };
    return ma_context_init(kBackends, 1, &cfg, context);
#elif defined(_WIN32)
    switch (g_preferred_backend) {
        case PreferredAudioBackend::Wasapi: {
            static const ma_backend kBackends[] = { ma_backend_wasapi };
            return ma_context_init(kBackends, 1, &cfg, context);
        }
        case PreferredAudioBackend::Mme: {
            static const ma_backend kBackends[] = { ma_backend_winmm };
            return ma_context_init(kBackends, 1, &cfg, context);
        }
        case PreferredAudioBackend::Dsound: {
            static const ma_backend kBackends[] = { ma_backend_dsound };
            return ma_context_init(kBackends, 1, &cfg, context);
        }
        case PreferredAudioBackend::Default:
        default:
            return ma_context_init(nullptr, 0, &cfg, context);
    }
#else
    return ma_context_init(nullptr, 0, &cfg, context);
#endif
}
} // namespace

void AudioDevice::setPreferredBackend(PreferredAudioBackend backend) {
    g_preferred_backend = backend;
}

struct AudioDeviceImpl {
    ma_context context{};
    ma_device device{};
    bool context_initialized = false;
    bool device_initialized = false;
    int channels = 0;
    AudioDevice::CaptureCallback capture_cb;
    AudioDevice::PlaybackCallback playback_cb;
};

static void DataCallback(ma_device* device, void* output, const void* input, ma_uint32 frame_count) {
    auto* self = static_cast<AudioDeviceImpl*>(device->pUserData);

    if (input && self->capture_cb) {
        self->capture_cb(static_cast<const int16_t*>(input), frame_count);
    }

    if (output && self->playback_cb) {
        size_t written = self->playback_cb(static_cast<int16_t*>(output), frame_count);
        if (written < frame_count) {
            size_t remaining_samples = (frame_count - written) * static_cast<size_t>(self->channels);
            std::memset(static_cast<int16_t*>(output) + written * self->channels, 0,
                        remaining_samples * sizeof(int16_t));
        }
    }
}

AudioDevice::AudioDevice() : impl_(new AudioDeviceImpl()) {}

AudioDevice::~AudioDevice() {
    close();
    delete impl_;
}

std::vector<AudioDeviceInfo> AudioDevice::enumerate(AudioDeviceKind kind) {
    std::vector<AudioDeviceInfo> result;

    ma_context context;
    if (InitContext(&context) != MA_SUCCESS) {
        return result;
    }

    ma_device_info* playback_infos = nullptr;
    ma_uint32 playback_count = 0;
    ma_device_info* capture_infos = nullptr;
    ma_uint32 capture_count = 0;

    if (ma_context_get_devices(&context, &playback_infos, &playback_count,
                                &capture_infos, &capture_count) == MA_SUCCESS) {
        auto& cache = (kind == AudioDeviceKind::Capture) ? g_capture_ids : g_playback_ids;
        cache.clear();

        ma_device_info* infos = (kind == AudioDeviceKind::Capture) ? capture_infos : playback_infos;
        ma_uint32 count = (kind == AudioDeviceKind::Capture) ? capture_count : playback_count;

        for (ma_uint32 i = 0; i < count; ++i) {
            AudioDeviceInfo info;
            info.id = std::to_string(cache.size());
            info.name = infos[i].name;
#if defined(__linux__)
            // .alsa is a plain char[256] PCM name string (e.g.
            // "hw:CARD=Loopback,DEV=0") — meaningful here because
            // InitContext() forces ma_backend_alsa specifically, so this
            // union member is guaranteed to be the one actually populated.
            // ma_device_id is a UNION: reading .alsa when some OTHER
            // backend (e.g. WASAPI's wchar_t GUID) actually wrote the ID
            // would reinterpret those bytes as narrow chars — garbage, not
            // an empty string — so this must stay compile-time-gated to
            // platforms where we know ALSA is the active backend, not a
            // runtime "is it empty" check.
            info.native_id = infos[i].id.alsa;
#endif
            info.is_default = infos[i].isDefault != 0;
            cache.push_back(infos[i].id);
            result.push_back(std::move(info));
        }
    }

    ma_context_uninit(&context);
    return result;
}

bool AudioDevice::supportsFormatNatively(AudioDeviceKind kind, const std::string& device_id,
                                          int sample_rate, int /*channels*/) {
    auto& cache = (kind == AudioDeviceKind::Capture) ? g_capture_ids : g_playback_ids;
    int idx = -1;
    try {
        idx = std::stoi(device_id);
    } catch (...) {
        return false;
    }
    if (idx < 0 || static_cast<size_t>(idx) >= cache.size()) return false;

    ma_context context;
    if (InitContext(&context) != MA_SUCCESS) return false;

    ma_device_info detailed{};
    ma_device_type dtype = (kind == AudioDeviceKind::Capture) ? ma_device_type_capture : ma_device_type_playback;
    ma_result r = ma_context_get_device_info(&context, dtype, &cache[idx], &detailed);
    ma_context_uninit(&context);
    if (r != MA_SUCCESS) return false;

    // Only sample rate is checked — see the header doc comment for why
    // channel count and bit depth are deliberately not required to match.
    //
    // Deliberately NOT falling back to a trial-open for WinMM (MME) when
    // this metadata query comes up empty at 192kHz, even though that query
    // is known to be structurally incapable of reporting rates above 96kHz
    // (see PreferredAudioBackend's doc comment). Tried exactly that and
    // verified with a real loopback tone test
    // (tests/probe_mme_resample_check.cpp): a trial ma_device_init() at
    // 192kHz reports success even on hardware confirmed via WASAPI to be
    // 48kHz-only, and a 90kHz test tone played through it vanished
    // completely on capture — Windows' modern WinMM compatibility shim
    // silently resamples/filters regardless of what you requested, with
    // no way to detect this from open() succeeding. So for MME, "not
    // reported as natively supported" simply stays a hard no here — there
    // is no reliable ground truth available for this backend at this rate.
    for (ma_uint32 i = 0; i < detailed.nativeDataFormatCount; ++i) {
        const auto& f = detailed.nativeDataFormats[i];
        if (f.sampleRate == static_cast<ma_uint32>(sample_rate) || f.sampleRate == 0) return true;
    }
    return false;
}

std::vector<NativeFormat> AudioDevice::listNativeFormats(AudioDeviceKind kind, const std::string& device_id) {
    std::vector<NativeFormat> result;

    auto& cache = (kind == AudioDeviceKind::Capture) ? g_capture_ids : g_playback_ids;
    int idx = -1;
    try {
        idx = std::stoi(device_id);
    } catch (...) {
        return result;
    }
    if (idx < 0 || static_cast<size_t>(idx) >= cache.size()) return result;

    ma_context context;
    if (InitContext(&context) != MA_SUCCESS) return result;

    ma_device_info detailed{};
    ma_device_type dtype = (kind == AudioDeviceKind::Capture) ? ma_device_type_capture : ma_device_type_playback;
    ma_result r = ma_context_get_device_info(&context, dtype, &cache[idx], &detailed);
    ma_context_uninit(&context);
    if (r != MA_SUCCESS) return result;

    for (ma_uint32 i = 0; i < detailed.nativeDataFormatCount; ++i) {
        const auto& f = detailed.nativeDataFormats[i];
        NativeFormat nf;
        nf.sample_rate = static_cast<int>(f.sampleRate);
        nf.channels = static_cast<int>(f.channels);
        nf.bits_per_sample = (f.format == ma_format_unknown) ? 0 : static_cast<int>(ma_get_bytes_per_sample(f.format) * 8);
        result.push_back(nf);
    }
    return result;
}

bool AudioDevice::openCapture(const std::string& device_id, int sample_rate, int channels, CaptureCallback cb) {
    close();

    int idx = -1;
    try { idx = std::stoi(device_id); } catch (...) {}
    if (idx < 0 || static_cast<size_t>(idx) >= g_capture_ids.size()) {
        last_error_ = "invalid capture device id";
        return false;
    }

    if (InitContext(&impl_->context) != MA_SUCCESS) {
        last_error_ = "ma_context_init failed";
        return false;
    }
    impl_->context_initialized = true;

    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.pDeviceID = &g_capture_ids[idx];
    config.capture.format = ma_format_s16;
    config.capture.channels = static_cast<ma_uint32>(channels);
    config.capture.shareMode = ma_share_mode_shared;
    config.sampleRate = static_cast<ma_uint32>(sample_rate);
    config.dataCallback = &DataCallback;
    config.pUserData = impl_;
    // Belt-and-suspenders: we already gate on supportsFormatNatively() before
    // calling this, but explicitly disable backend-level auto-resampling too.
    config.wasapi.noAutoConvertSRC = MA_TRUE;
    config.wasapi.noDefaultQualitySRC = MA_TRUE;
    config.alsa.noAutoResample = MA_TRUE;

    if (ma_device_init(&impl_->context, &config, &impl_->device) != MA_SUCCESS) {
        last_error_ = "ma_device_init failed (device may not support the requested format natively)";
        ma_context_uninit(&impl_->context);
        impl_->context_initialized = false;
        return false;
    }

    impl_->device_initialized = true;
    impl_->channels = channels;
    impl_->capture_cb = std::move(cb);
    return true;
}

bool AudioDevice::openPlayback(const std::string& device_id, int sample_rate, int channels, PlaybackCallback cb) {
    close();

    int idx = -1;
    try { idx = std::stoi(device_id); } catch (...) {}
    if (idx < 0 || static_cast<size_t>(idx) >= g_playback_ids.size()) {
        last_error_ = "invalid playback device id";
        return false;
    }

    if (InitContext(&impl_->context) != MA_SUCCESS) {
        last_error_ = "ma_context_init failed";
        return false;
    }
    impl_->context_initialized = true;

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.pDeviceID = &g_playback_ids[idx];
    config.playback.format = ma_format_s16;
    config.playback.channels = static_cast<ma_uint32>(channels);
    config.playback.shareMode = ma_share_mode_shared;
    config.sampleRate = static_cast<ma_uint32>(sample_rate);
    config.dataCallback = &DataCallback;
    config.pUserData = impl_;
    config.wasapi.noAutoConvertSRC = MA_TRUE;
    config.wasapi.noDefaultQualitySRC = MA_TRUE;
    config.alsa.noAutoResample = MA_TRUE;

    if (ma_device_init(&impl_->context, &config, &impl_->device) != MA_SUCCESS) {
        last_error_ = "ma_device_init failed (device may not support the requested format natively)";
        ma_context_uninit(&impl_->context);
        impl_->context_initialized = false;
        return false;
    }

    impl_->device_initialized = true;
    impl_->channels = channels;
    impl_->playback_cb = std::move(cb);
    return true;
}

bool AudioDevice::start() {
    if (!impl_->device_initialized) {
        last_error_ = "device not opened";
        return false;
    }
    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        last_error_ = "ma_device_start failed";
        return false;
    }
    return true;
}

void AudioDevice::stop() {
    if (impl_->device_initialized) {
        ma_device_stop(&impl_->device);
    }
}

void AudioDevice::close() {
    if (impl_->device_initialized) {
        ma_device_uninit(&impl_->device);
        impl_->device_initialized = false;
    }
    if (impl_->context_initialized) {
        ma_context_uninit(&impl_->context);
        impl_->context_initialized = false;
    }
    impl_->capture_cb = nullptr;
    impl_->playback_cb = nullptr;
}

} // namespace mpx
