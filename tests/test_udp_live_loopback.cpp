// Live, hardware-dependent end-to-end check that SenderApp -> UDP
// (127.0.0.1) -> ReceiverApp reports near-zero packet loss over localhost.
// Written to confirm the fix to receiver_app.cpp's loss-rate math: it used
// to assume every stream ran at the fixed 192kHz hardware capture rate, so
// a stream encoded at a lower rate (the 128kHz default) reported ~33%
// "loss" with zero packets actually dropped (128000/192000 = 0.667, so
// 1 - 0.667 = 33%). See kFallbackPacketsPerSec in receiver_app.cpp.
//
// Not registered with add_test(): needs a capture+playback device pair
// that both support 192kHz natively (e.g. the VB-Audio Hi-Fi Cable virtual
// device) — run probe_audio_devices first to check what's available.

#include <chrono>
#include <cstdio>
#include <thread>

#include "net_common.h"
#include "receiver_app.h"
#include "sender_app.h"

using namespace mpx;

namespace {
bool FindHiFiCableDevice(const std::vector<AudioDeviceInfo>& devices, std::string* out_id, std::string* out_name) {
    for (const auto& d : devices) {
        if (d.name.find("Hi-Fi Cable") != std::string::npos) {
            *out_id = d.id;
            *out_name = d.name;
            return true;
        }
    }
    return false;
}
} // namespace

int main() {
    NetworkInit net_init; // WSAStartup on Windows — required before any socket call

    auto capture_devices = SenderApp::listInputDevices();
    auto playback_devices = ReceiverApp::listOutputDevices();

    std::string capture_id, capture_name, playback_id, playback_name;
    if (!FindHiFiCableDevice(capture_devices, &capture_id, &capture_name) ||
        !FindHiFiCableDevice(playback_devices, &playback_id, &playback_name)) {
        std::printf("SKIPPED: no VB-Audio Hi-Fi Cable capture+playback device pair found "
                    "(this test needs a 192kHz-native loopback device; see probe_audio_devices).\n");
        return 0;
    }
    std::printf("Using capture='%s' playback='%s'\n", capture_name.c_str(), playback_name.c_str());

    ReceiverConfig rcfg;
    rcfg.output_device_id = playback_id;
    rcfg.transport = TransportMode::Udp;
    rcfg.udp.address = "127.0.0.1"; // bind address
    rcfg.udp.port = 47110;

    ReceiverApp receiver;
    if (!receiver.start(rcfg)) {
        std::printf("FAILED: receiver.start() failed: %s\n", receiver.status().last_error.c_str());
        return 1;
    }

    SenderConfig scfg;
    scfg.input_device_id = capture_id;
    scfg.transport = TransportMode::Udp;
    scfg.udp.address = "127.0.0.1";
    scfg.udp.port = 47110;

    SenderApp sender;
    if (!sender.start(scfg)) {
        std::printf("FAILED: sender.start() failed: %s\n", sender.status().last_error.c_str());
        receiver.stop();
        return 1;
    }

    // Let several 1-second loss-rate windows settle (see
    // receiver_app.cpp's networkThreadUdp).
    std::this_thread::sleep_for(std::chrono::seconds(5));

    auto rstatus = receiver.status();
    std::printf("Receiver state: %s\n", rstatus.state.c_str());
    std::printf("Detected stream rate: %d Hz\n", rstatus.stream_sample_rate);
    std::printf("Reported UDP loss rate: %.1f%%\n", rstatus.udp_loss_rate * 100.0);

    sender.stop();
    receiver.stop();

    bool all_ok = true;
    if (rstatus.state != "Streaming") {
        std::printf("FAILED: receiver never reached Streaming state\n");
        all_ok = false;
    }
    if (rstatus.udp_loss_rate > 0.05) {
        std::printf("FAILED: loss rate %.1f%% is too high for a localhost loopback with no real loss\n",
                    rstatus.udp_loss_rate * 100.0);
        all_ok = false;
    }
    if (!all_ok) {
        std::printf("FAILED\n");
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
