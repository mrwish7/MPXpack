#include "receiver_gui.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "config_store.h"
#include "imgui.h"

namespace mpx {

namespace {
void CopyToBuffer(char* buf, size_t buf_size, const std::string& s) {
    std::snprintf(buf, buf_size, "%s", s.c_str());
}
// The identity used for matching/persisting device selection across runs.
// Prefer native_id (e.g. the raw ALSA PCM name) since `name` alone can be
// identical across several distinct devices (see audio_device.h).
std::string PersistedIdentity(const AudioDeviceInfo& d) {
    return d.native_id.empty() ? d.name : d.native_id;
}
constexpr double kAutoStartRetrySec = 3.0;
} // namespace

ReceiverGui::ReceiverGui() {
    refreshDevices();
}

void ReceiverGui::refreshDevices() {
    devices_ = ReceiverApp::listOutputDevices();
    selected_device_index_ = -1;
    for (size_t i = 0; i < devices_.size(); ++i) {
        if (devices_[i].is_default) { selected_device_index_ = static_cast<int>(i); break; }
    }
    if (selected_device_index_ < 0 && !devices_.empty()) selected_device_index_ = 0;
    if (!selected_device_name_.empty()) resolveDeviceByName();
    device_checked_ = false;
    device_valid_ = false;
}

void ReceiverGui::resolveDeviceByName() {
    for (size_t i = 0; i < devices_.size(); ++i) {
        if (PersistedIdentity(devices_[i]) == selected_device_name_) {
            selected_device_index_ = static_cast<int>(i);
            return;
        }
    }
}

void ReceiverGui::refreshValidation() {
    device_checked_ = true;
    device_valid_ = false;
    if (selected_device_index_ >= 0 && selected_device_index_ < static_cast<int>(devices_.size())) {
        device_valid_ = ReceiverApp::validateDevice(devices_[selected_device_index_].id);
    }
}

void ReceiverGui::draw(ReceiverApp& app) {
    ImGui::Begin("MPXpack Receiver");

    bool running = app.isRunning();
    ImGui::BeginDisabled(running);

#if defined(_WIN32)
    ImGui::SeparatorText("Audio API (Windows)");
    int prev_backend_index = audio_backend_index_;
    ImGui::RadioButton("Default", &audio_backend_index_, 0); ImGui::SameLine();
    ImGui::RadioButton("WASAPI", &audio_backend_index_, 1); ImGui::SameLine();
    ImGui::RadioButton("MME", &audio_backend_index_, 2); ImGui::SameLine();
    ImGui::RadioButton("DirectSound", &audio_backend_index_, 3);
    if (audio_backend_index_ != prev_backend_index) {
        AudioDevice::setPreferredBackend(static_cast<PreferredAudioBackend>(audio_backend_index_));
        refreshDevices(); // device ids are only valid against the enumerate() call that produced
                           // them, and switching backends can change the device list entirely
    }
    if (audio_backend_index_ == 3) {
        // Verified with tests/probe_mme_resample_check.cpp: DirectSound's
        // *playback* direction was genuinely honest at 192kHz on the one
        // device tested (a real, non-silently-resampled passthrough) — this
        // is the receiver, i.e. playback, so this is the direction that
        // actually checked out. Worth trying if WASAPI's shared-mode mixer
        // is suspected in ongoing glitches, without exclusive mode's
        // downside of locking the device from other processes.
        ImGui::TextDisabled("Playback under DirectSound verified as a genuine, honest 192kHz\n"
                             "passthrough on the device tested \xe2\x80\x94 a real alternative to WASAPI here.");
    }
#endif

    ImGui::SeparatorText("Input");
    ImGui::RadioButton("Icecast", &transport_index_, 0); ImGui::SameLine();
    ImGui::RadioButton("Direct UDP", &transport_index_, 1);

    if (transport_index_ == 0) {
        ImGui::InputText("Host", icecast_host_, sizeof(icecast_host_));
        ImGui::InputInt("Port", &icecast_port_);
        ImGui::InputText("Mount", icecast_mount_, sizeof(icecast_mount_));
    } else {
        ImGui::InputText("Listen address", udp_listen_address_, sizeof(udp_listen_address_));
        ImGui::InputInt("Listen port", &udp_listen_port_);
    }
    ImGui::SliderInt("Max latency (ms)", &max_latency_ms_, 40, 1000);

    ImGui::SeparatorText("Output device");
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) refreshDevices();

    std::string preview = (selected_device_index_ >= 0 && selected_device_index_ < static_cast<int>(devices_.size()))
        ? devices_[selected_device_index_].displayLabel() : "(no device selected)";
    if (ImGui::BeginCombo("Playback device", preview.c_str())) {
        for (int i = 0; i < static_cast<int>(devices_.size()); ++i) {
            bool selected = (i == selected_device_index_);
            ImGui::PushID(i); // devices can share an identical display name (e.g. two ALSA
                              // loopback substreams both named "Loopback Analogue Stereo") —
                              // without this, ImGui's label-derived IDs collide.
            if (ImGui::Selectable(devices_[i].displayLabel().c_str(), selected)) {
                selected_device_index_ = i;
                selected_device_name_ = PersistedIdentity(devices_[i]);
                refreshValidation();
            }
            ImGui::PopID();
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (!device_checked_ && selected_device_index_ >= 0) refreshValidation();
    if (device_checked_) {
        if (device_valid_) {
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "Supports 192kHz mono 16-bit natively.");
        } else {
            ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f),
                                "Device does not natively support 192kHz mono 16-bit.");
        }
    }

#if defined(_WIN32)
    if (!device_valid_ && audio_backend_index_ == 2) {
        ImGui::Checkbox("Trust MME at 192kHz anyway (I've verified this device's setup myself)",
                         &mme_trust_override_);
        if (mme_trust_override_) {
            ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f),
                "Bypassing validation, which can never confirm this under MME (see tooltip/docs) \xe2\x80\x94\n"
                "a misconfigured device will silently resample near the RDS band with no warning.\n"
                "Verify first with probe_mme_resample_check if you haven't already.");
        }
    }
#endif

    ImGui::RadioButton("Both channels (duplicate to L+R)", &output_channel_select_index_, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Left only", &output_channel_select_index_, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Right only", &output_channel_select_index_, 2);

    ImGui::EndDisabled();

    ImGui::Spacing();
#if defined(_WIN32)
    bool effective_device_valid = device_valid_ || (audio_backend_index_ == 2 && mme_trust_override_);
#else
    bool effective_device_valid = device_valid_;
#endif
    bool settings_complete = (transport_index_ == 0) ? icecast_host_[0] != '\0' : udp_listen_port_ > 0;
    bool device_ready = effective_device_valid && selected_device_index_ >= 0;
    bool can_start = !running && device_ready && settings_complete;

    if (auto_start_pending_ && !running) {
        // refreshDevices() falls back to the system default when the saved
        // device isn't (yet) in the list — never auto-start on that
        // substitute, wait for the device that was actually configured.
        bool saved_device_selected = selected_device_name_.empty() ||
            (selected_device_index_ >= 0 && selected_device_index_ < static_cast<int>(devices_.size()) &&
             PersistedIdentity(devices_[selected_device_index_]) == selected_device_name_);
        tryAutoStart(app, settings_complete, device_ready && saved_device_selected);
        running = app.isRunning();
    }

    if (!running) {
        ImGui::BeginDisabled(!can_start);
        if (ImGui::Button("Start", ImVec2(120, 0))) {
            auto_start_pending_ = false;
            auto_start_status_.clear();
            startWithCurrentSettings(app);
        }
        ImGui::EndDisabled();
    } else {
        if (ImGui::Button("Stop", ImVec2(120, 0))) {
            app.stop();
        }
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Auto-start on launch", &auto_start_)) {
        if (!auto_start_) {
            auto_start_pending_ = false;
            auto_start_status_.clear();
        }
        if (!config_path_.empty()) saveConfig(config_path_);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Start receiving with the saved settings as soon as the app opens\n"
                          "(e.g. after the machine restarts). Has no effect until settings\n"
                          "have been saved, i.e. Start has been pressed at least once.");
    }
    if (!auto_start_status_.empty()) {
        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "%s", auto_start_status_.c_str());
    }

    ImGui::SeparatorText("Status");
    ReceiverApp::Status status = app.status();
    ImGui::Text("State: %s", status.state.c_str());
    if (!status.last_error.empty()) {
        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "Last error: %s", status.last_error.c_str());
    }
    if (transport_index_ == 1) {
        ImGui::Text("UDP loss rate (approx): %.1f%%", status.udp_loss_rate * 100.0);
        ImGui::Text("Blocks dropped (reassembly gave up): %llu",
                    static_cast<unsigned long long>(status.udp_dropped_blocks));
    }
    if (status.stream_sample_rate > 0) {
        ImGui::Text("Incoming stream rate: %d Hz%s", status.stream_sample_rate,
                    status.stream_sample_rate == kSampleRate ? "" : " (resampling to 192kHz)");
    }
    ImGui::Text("Decoder errors: %d", status.decoder_error_count);

    int depth = status.buffer_depth_ms;
    int target = max_latency_ms_;
    float frac = target > 0 ? std::min(1.0f, static_cast<float>(depth) / target) : 0.0f;
    ImVec4 color = (frac < 0.2f) ? ImVec4(0.9f, 0.6f, 0.2f, 1.0f) : ImVec4(0.3f, 0.9f, 0.3f, 1.0f);
    ImGui::TextColored(color, "Buffer depth: %d ms / %d ms target", depth, target);
    ImGui::ProgressBar(frac, ImVec2(-1, 0));

    ImGui::Text("Output level");
    ImGui::ProgressBar(static_cast<float>(status.output_peak), ImVec2(-1, 0));

    ImGui::End();
}

bool ReceiverGui::startWithCurrentSettings(ReceiverApp& app) {
    ReceiverConfig cfg;
    cfg.output_device_id = devices_[selected_device_index_].id;
    cfg.channel_select = static_cast<ChannelSelect>(output_channel_select_index_);
    cfg.max_latency_ms = max_latency_ms_;
    cfg.transport = (transport_index_ == 0) ? TransportMode::Icecast : TransportMode::Udp;
    cfg.icecast.host = icecast_host_;
    cfg.icecast.port = icecast_port_;
    cfg.icecast.mount = icecast_mount_;
    cfg.udp.address = udp_listen_address_;
    cfg.udp.port = udp_listen_port_;
    bool ok = app.start(cfg);
    if (!config_path_.empty()) saveConfig(config_path_); // crash-safety: persist as soon as settings are used
    return ok;
}

void ReceiverGui::tryAutoStart(ReceiverApp& app, bool settings_complete, bool device_ready) {
    if (!settings_complete) {
        // Nothing to retry for — the saved config itself is missing the
        // stream details, so leave it to the user.
        auto_start_pending_ = false;
        auto_start_status_ = "Auto-start skipped: saved settings are incomplete.";
        return;
    }

    double now = ImGui::GetTime();
    if (now < auto_start_next_attempt_) return;

    if (device_ready && startWithCurrentSettings(app)) {
        auto_start_pending_ = false;
        auto_start_status_.clear();
        return;
    }

    auto_start_status_ = device_ready
        ? "Auto-start: output device failed to open, retrying..."
        : "Auto-start: waiting for output device \"" + selected_device_name_ + "\"...";
    auto_start_next_attempt_ = now + kAutoStartRetrySec;
    refreshDevices(); // after a reboot the device may simply not be enumerated yet
}

void ReceiverGui::loadConfig(const std::string& path) {
    config_path_ = path; // remembered even on first run, so auto-save-on-Start still works

    ConfigStore cs;
    if (!cs.load(path)) return; // no saved config yet (first run) — keep built-in defaults

    audio_backend_index_ = cs.getInt("audio_backend_index", 0);
#if defined(_WIN32)
    AudioDevice::setPreferredBackend(static_cast<PreferredAudioBackend>(audio_backend_index_));
    mme_trust_override_ = cs.getInt("mme_trust_override", 0) != 0;
#endif

    selected_device_name_ = cs.getString("output_device_name", "");
    refreshDevices(); // re-enumerate now that the backend preference (if any) is applied —
                       // this also resolves selected_device_name_ against the fresh list

    transport_index_ = cs.getInt("transport_index", 1);
    output_channel_select_index_ = cs.getInt("output_channel_select_index", 0);
    max_latency_ms_ = cs.getInt("max_latency_ms", kDefaultMaxLatencyMs);

    CopyToBuffer(icecast_host_, sizeof(icecast_host_), cs.getString("icecast_host", ""));
    icecast_port_ = cs.getInt("icecast_port", 8000);
    CopyToBuffer(icecast_mount_, sizeof(icecast_mount_), cs.getString("icecast_mount", "/mpx.wv"));

    CopyToBuffer(udp_listen_address_, sizeof(udp_listen_address_), cs.getString("udp_listen_address", "0.0.0.0"));
    udp_listen_port_ = cs.getInt("udp_listen_port", 9000);

    // Defaults to on for config files that predate this setting too.
    auto_start_ = cs.getBool("auto_start", true);
    auto_start_pending_ = auto_start_;
}

void ReceiverGui::saveConfig(const std::string& path) const {
    ConfigStore cs;

    std::string device_name = (selected_device_index_ >= 0 && selected_device_index_ < static_cast<int>(devices_.size()))
        ? PersistedIdentity(devices_[selected_device_index_]) : selected_device_name_;
    cs.setString("output_device_name", device_name);
    cs.setInt("audio_backend_index", audio_backend_index_);
    cs.setInt("mme_trust_override", mme_trust_override_ ? 1 : 0);

    cs.setInt("transport_index", transport_index_);
    cs.setInt("output_channel_select_index", output_channel_select_index_);
    cs.setInt("max_latency_ms", max_latency_ms_);

    cs.setString("icecast_host", icecast_host_);
    cs.setInt("icecast_port", icecast_port_);
    cs.setString("icecast_mount", icecast_mount_);

    cs.setString("udp_listen_address", udp_listen_address_);
    cs.setInt("udp_listen_port", udp_listen_port_);

    cs.setBool("auto_start", auto_start_);

    cs.save(path);
}

} // namespace mpx
