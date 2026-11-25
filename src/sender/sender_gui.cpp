#include "sender_gui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

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
} // namespace

SenderGui::SenderGui() {
    refreshDevices();
}

void SenderGui::refreshDevices() {
    devices_ = SenderApp::listInputDevices();
    selected_device_index_ = -1;
    for (size_t i = 0; i < devices_.size(); ++i) {
        if (devices_[i].is_default) { selected_device_index_ = static_cast<int>(i); break; }
    }
    if (selected_device_index_ < 0 && !devices_.empty()) selected_device_index_ = 0;
    if (!selected_device_name_.empty()) resolveDeviceByName();
    device_checked_ = false;
    device_valid_ = false;
}

void SenderGui::resolveDeviceByName() {
    for (size_t i = 0; i < devices_.size(); ++i) {
        if (PersistedIdentity(devices_[i]) == selected_device_name_) {
            selected_device_index_ = static_cast<int>(i);
            return;
        }
    }
    // Not found (unplugged, renamed, or first run with no saved config) —
    // leave whatever refreshDevices() already picked (default/first device).
}

void SenderGui::refreshValidation() {
    device_checked_ = true;
    device_valid_ = false;
    if (selected_device_index_ >= 0 && selected_device_index_ < static_cast<int>(devices_.size())) {
        device_valid_ = SenderApp::validateDevice(devices_[selected_device_index_].id);
    }
}

void SenderGui::draw(SenderApp& app) {
    ImGui::Begin("MPXpack Sender");

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
        // *capture* direction silently resampled on the one device tested
        // (its playback direction was genuinely honest) — this is the
        // sender, i.e. capture, so validation below is likely to (correctly)
        // fail. No override exists for this like MME's, since it isn't a
        // structural blind spot the way MME's capability query is — the
        // check already reflects what was actually measured.
        ImGui::TextDisabled("Capture under DirectSound was found to silently resample on the one\n"
                             "device tested \xe2\x80\x94 WASAPI remains the recommended choice here.");
    }
#endif

    ImGui::SeparatorText("Input device");
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) refreshDevices();

    std::string preview = (selected_device_index_ >= 0 && selected_device_index_ < static_cast<int>(devices_.size()))
        ? devices_[selected_device_index_].displayLabel() : "(no device selected)";
    if (ImGui::BeginCombo("Capture device", preview.c_str())) {
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

    ImGui::RadioButton("Both channels (mix to mono)", &channel_select_index_, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Left only", &channel_select_index_, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Right only", &channel_select_index_, 2);

    ImGui::SeparatorText("Encoding");
    ImGui::RadioButton("Lossy (hybrid)", &encode_mode_index_, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Lossless", &encode_mode_index_, 1);
    const bool lossless = (encode_mode_index_ == 1);

    ImGui::RadioButton("128 kHz (lowest bitrate)", &sample_rate_index_, 0);
    ImGui::RadioButton("144 kHz (intermediate)", &sample_rate_index_, 1);
    ImGui::RadioButton("192 kHz (full bandwidth \xe2\x80\x94 for DARC/RDS2 above 57kHz)", &sample_rate_index_, 2);
    int encode_sample_rate = kEncodeSampleRateLow;
    switch (sample_rate_index_) {
        case 1: encode_sample_rate = kEncodeSampleRateMid; break;
        case 2: encode_sample_rate = kEncodeSampleRateHigh; break;
        default: encode_sample_rate = kEncodeSampleRateLow; break;
    }

    if (lossless) {
        ImGui::TextDisabled("Lossless \xe2\x80\x94 bitrate varies with content, no target to set. "
                             "Best for LAN links where bandwidth isn't the constraint.");
    } else {
        // Slider itself is continuous; snap the result to 0.1 steps rather
        // than trusting ImGui's drag granularity (SliderFloat has no native
        // step parameter).
        if (ImGui::SliderFloat("Bits/sample (hybrid)", &bits_per_sample_, kBpsMin, kBpsMax, "%.1f")) {
            bits_per_sample_ = std::clamp(std::round(bits_per_sample_ * 10.0f) / 10.0f, kBpsMin, kBpsMax);
        }
        ImGui::Text("~= %d kbps", BpsToKbps(bits_per_sample_, encode_sample_rate));
    }
    ImGui::SliderInt("Max latency (ms)", &max_latency_ms_, 40, 1000);

    ImGui::SeparatorText("Output");
    ImGui::RadioButton("Icecast", &transport_index_, 0); ImGui::SameLine();
    ImGui::RadioButton("Direct UDP", &transport_index_, 1);

    if (transport_index_ == 0) {
        ImGui::InputText("Host", icecast_host_, sizeof(icecast_host_));
        ImGui::InputInt("Port", &icecast_port_);
        ImGui::InputText("Mount", icecast_mount_, sizeof(icecast_mount_));
        ImGui::InputText("Source user", icecast_user_, sizeof(icecast_user_));
        ImGui::InputText("Source password", icecast_password_, sizeof(icecast_password_), ImGuiInputTextFlags_Password);
    } else {
        ImGui::InputText("Destination IP", udp_address_, sizeof(udp_address_));
        ImGui::InputInt("Destination port", &udp_port_);
    }

    ImGui::EndDisabled();

    ImGui::Spacing();
#if defined(_WIN32)
    bool effective_device_valid = device_valid_ || (audio_backend_index_ == 2 && mme_trust_override_);
#else
    bool effective_device_valid = device_valid_;
#endif
    bool can_start = !running && effective_device_valid && selected_device_index_ >= 0;
    if (transport_index_ == 0) can_start = can_start && icecast_host_[0] != '\0';
    else can_start = can_start && udp_address_[0] != '\0';

    if (!running) {
        ImGui::BeginDisabled(!can_start);
        if (ImGui::Button("Start", ImVec2(120, 0))) {
            SenderConfig cfg;
            cfg.input_device_id = devices_[selected_device_index_].id;
            cfg.bits_per_sample = bits_per_sample_;
            cfg.encode_sample_rate = encode_sample_rate;
            cfg.lossless = lossless;
            cfg.channel_select = static_cast<ChannelSelect>(channel_select_index_);
            cfg.max_latency_ms = max_latency_ms_;
            cfg.transport = (transport_index_ == 0) ? TransportMode::Icecast : TransportMode::Udp;
            cfg.icecast.host = icecast_host_;
            cfg.icecast.port = icecast_port_;
            cfg.icecast.mount = icecast_mount_;
            cfg.icecast.user = icecast_user_;
            cfg.icecast.password = icecast_password_;
            cfg.udp.address = udp_address_;
            cfg.udp.port = udp_port_;
            app.start(cfg);
            if (!config_path_.empty()) saveConfig(config_path_); // crash-safety: persist as soon as settings are used
        }
        ImGui::EndDisabled();
    } else {
        if (ImGui::Button("Stop", ImVec2(120, 0))) {
            app.stop();
        }
    }

    ImGui::SeparatorText("Status");
    SenderApp::Status status = app.status();
    ImGui::Text("State: %s", status.state.c_str());
    if (!status.last_error.empty()) {
        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "Last error: %s", status.last_error.c_str());
    }
    ImGui::Text("Input level");
    ImGui::ProgressBar(static_cast<float>(status.input_peak), ImVec2(-1, 0));
    ImGui::Text("Outgoing bitrate: %.0f kbps", status.outgoing_kbps);
    ImGui::Text("Queue depth: %d ms", status.queue_depth_ms);

    ImGui::End();
}

void SenderGui::loadConfig(const std::string& path) {
    config_path_ = path; // remembered even on first run, so auto-save-on-Start still works

    ConfigStore cs;
    if (!cs.load(path)) return; // no saved config yet (first run) — keep built-in defaults

    audio_backend_index_ = cs.getInt("audio_backend_index", 0);
#if defined(_WIN32)
    AudioDevice::setPreferredBackend(static_cast<PreferredAudioBackend>(audio_backend_index_));
    mme_trust_override_ = cs.getInt("mme_trust_override", 0) != 0;
#endif

    selected_device_name_ = cs.getString("input_device_name", "");
    refreshDevices(); // re-enumerate now that the backend preference (if any) is applied —
                       // this also resolves selected_device_name_ against the fresh list

    bits_per_sample_ = cs.getFloat("bits_per_sample", kBpsDefault);
    sample_rate_index_ = cs.getInt("sample_rate_index", 0);
    encode_mode_index_ = cs.getInt("encode_mode_index", 0);
    channel_select_index_ = cs.getInt("channel_select_index", 0);
    transport_index_ = cs.getInt("transport_index", 1);
    max_latency_ms_ = cs.getInt("max_latency_ms", kDefaultMaxLatencyMs);

    CopyToBuffer(icecast_host_, sizeof(icecast_host_), cs.getString("icecast_host", ""));
    icecast_port_ = cs.getInt("icecast_port", 8000);
    CopyToBuffer(icecast_mount_, sizeof(icecast_mount_), cs.getString("icecast_mount", "/mpx.wv"));
    CopyToBuffer(icecast_user_, sizeof(icecast_user_), cs.getString("icecast_user", "source"));
    CopyToBuffer(icecast_password_, sizeof(icecast_password_), cs.getString("icecast_password", ""));

    CopyToBuffer(udp_address_, sizeof(udp_address_), cs.getString("udp_address", ""));
    udp_port_ = cs.getInt("udp_port", 9000);
}

void SenderGui::saveConfig(const std::string& path) const {
    ConfigStore cs;

    std::string device_name = (selected_device_index_ >= 0 && selected_device_index_ < static_cast<int>(devices_.size()))
        ? PersistedIdentity(devices_[selected_device_index_]) : selected_device_name_;
    cs.setString("input_device_name", device_name);
    cs.setInt("audio_backend_index", audio_backend_index_);
    cs.setInt("mme_trust_override", mme_trust_override_ ? 1 : 0);

    cs.setFloat("bits_per_sample", bits_per_sample_);
    cs.setInt("sample_rate_index", sample_rate_index_);
    cs.setInt("encode_mode_index", encode_mode_index_);
    cs.setInt("channel_select_index", channel_select_index_);
    cs.setInt("transport_index", transport_index_);
    cs.setInt("max_latency_ms", max_latency_ms_);

    cs.setString("icecast_host", icecast_host_);
    cs.setInt("icecast_port", icecast_port_);
    cs.setString("icecast_mount", icecast_mount_);
    cs.setString("icecast_user", icecast_user_);
    cs.setString("icecast_password", icecast_password_);

    cs.setString("udp_address", udp_address_);
    cs.setInt("udp_port", udp_port_);

    cs.save(path);
}

} // namespace mpx
