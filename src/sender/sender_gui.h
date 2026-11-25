#pragma once

#include <string>
#include <vector>

#include "app_config.h"
#include "audio_device.h"
#include "sender_app.h"

namespace mpx {

// Holds the ImGui widget state (text buffers, combo selection, etc.) that
// has to persist across frames, since ImGui itself is immediate-mode and
// doesn't own any state on its own. See docs/phase2-plan.md section 4 for
// the layout this implements.
class SenderGui {
public:
    SenderGui();

    // Call once per frame from the GLFW/ImGui render loop.
    void draw(SenderApp& app);

    // Config file is a flat key=value file (see ConfigStore) — load() is a
    // no-op if the file doesn't exist yet (first run), so callers can
    // always call it unconditionally at startup.
    void loadConfig(const std::string& path);
    void saveConfig(const std::string& path) const;

private:
    void refreshDevices();
    void refreshValidation();
    void resolveDeviceByName(); // matches selected_device_name_ against devices_ after a refresh/load

    std::vector<AudioDeviceInfo> devices_;
    int audio_backend_index_ = 0; // 0 = Default, 1 = WASAPI, 2 = MME (Windows only — see PreferredAudioBackend)
    // Deliberate, explicit user override: AudioDevice::supportsFormatNatively()
    // is permanently unable to confirm 192kHz under MME (the legacy capability
    // bitmask it's built on has no bit for anything above 96kHz — see
    // PreferredAudioBackend's doc comment), so validation always reports "not
    // supported" for MME regardless of whether the device is genuinely,
    // correctly configured. This lets a user who has independently verified
    // their own setup (e.g. Windows' per-device "Default Format" set to
    // 192kHz, confirmed with tests/probe_mme_resample_check.cpp) proceed
    // anyway — off by default, and only ever consulted while MME is selected.
    bool mme_trust_override_ = false;
    int selected_device_index_ = -1;
    std::string selected_device_name_; // persisted identity — device ids are only valid per-enumeration
    bool device_valid_ = false;
    bool device_checked_ = false;

    float bits_per_sample_ = kBpsDefault;
    int sample_rate_index_ = 0; // 0 = 128kHz (default), 1 = 144kHz, 2 = 192kHz
    int encode_mode_index_ = 0; // 0 = Lossy (hybrid), 1 = Lossless
    int channel_select_index_ = 0; // 0 = Both, 1 = Left, 2 = Right — matches ChannelSelect enum order
    int transport_index_ = 1; // 0 = Icecast, 1 = UDP (matches TransportMode enum order below)

    char icecast_host_[128] = "";
    int icecast_port_ = 8000;
    char icecast_mount_[128] = "/mpx.wv";
    char icecast_password_[128] = "";
    char icecast_user_[64] = "source";

    char udp_address_[64] = "";
    int udp_port_ = 9000;

    int max_latency_ms_ = kDefaultMaxLatencyMs;

    std::string config_path_; // set by loadConfig(); draw() auto-saves here on Start for crash-safety
};

} // namespace mpx
