#pragma once

#include <string>
#include <vector>

#include "app_config.h"
#include "audio_device.h"
#include "receiver_app.h"

namespace mpx {

// Holds the ImGui widget state that has to persist across frames — mirrors
// SenderGui.
class ReceiverGui {
public:
    ReceiverGui();

    void draw(ReceiverApp& app);

    // Config file is a flat key=value file (see ConfigStore) — load() is a
    // no-op if the file doesn't exist yet (first run), so callers can
    // always call it unconditionally at startup.
    void loadConfig(const std::string& path);
    void saveConfig(const std::string& path) const;

private:
    void refreshDevices();
    void refreshValidation();
    void resolveDeviceByName();
    bool startWithCurrentSettings(ReceiverApp& app);
    void tryAutoStart(ReceiverApp& app, bool settings_complete, bool device_ready);

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
    std::string selected_device_name_;
    bool device_valid_ = false;
    bool device_checked_ = false;

    int transport_index_ = 1; // 0 = Icecast, 1 = UDP
    int output_channel_select_index_ = 0; // 0 = Both, 1 = Left, 2 = Right — matches ChannelSelect enum order

    char icecast_host_[128] = "";
    int icecast_port_ = 8000;
    char icecast_mount_[128] = "/mpx.wv";

    char udp_listen_address_[64] = "0.0.0.0";
    int udp_listen_port_ = 9000;

    int max_latency_ms_ = kDefaultMaxLatencyMs;

    // Start the saved stream automatically at launch, so an unattended
    // machine (app set to open at login) resumes after a reboot. Only ever
    // acted on when a config file was actually loaded — first run with no
    // saved settings never auto-starts, regardless of this flag. Retries
    // while the saved output device isn't enumerable/openable yet, since
    // audio devices can show up a few seconds after login.
    bool auto_start_ = true;
    bool auto_start_pending_ = false; // armed by loadConfig(), cleared once started/cancelled
    double auto_start_next_attempt_ = 0.0; // ImGui::GetTime() of the next retry
    std::string auto_start_status_;

    std::string config_path_; // set by loadConfig(); draw() auto-saves here on Start for crash-safety
};

} // namespace mpx
