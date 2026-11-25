#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app_config.h"
#include "audio_device.h"
#include "block_queue.h"
#include "byte_ring_buffer.h"
#include "icecast_source_client.h"
#include "resampler.h"
#include "udp_socket.h"
#include "wavpack_encoder.h"

namespace mpx {

// Orchestrates the sender pipeline: miniaudio capture -> encode thread ->
// network thread, per the threading model in docs/phase2-plan.md section 3.
// (Simplification vs. the original plan: all cross-thread handoffs here use
// mutex+condvar queues, not lock-free SPSC rings — at this data rate
// (<=384kB/s raw PCM, <=120kB/s encoded) uncontended mutex overhead is
// negligible next to the audio callback period, so the added complexity of
// a lock-free ring wasn't worth it.)
class SenderApp {
public:
    struct Status {
        std::string state = "Idle"; // Idle, Connecting, Streaming, Reconnecting, Error
        std::string last_error;
        double input_peak = 0.0;   // 0..1
        double input_rms = 0.0;    // 0..1
        double outgoing_kbps = 0.0;
        int queue_depth_ms = 0;
    };

    SenderApp();
    ~SenderApp();

    static std::vector<AudioDeviceInfo> listInputDevices();
    static bool validateDevice(const std::string& device_id);

    bool start(const SenderConfig& cfg);
    void stop();
    bool isRunning() const { return running_.load(); }

    Status status() const;

private:
    void encodeThreadFunc();
    void networkThreadFunc();
    void networkThreadUdp();
    void networkThreadIcecast();
    void updateLevel(const int16_t* pcm, size_t n);
    void setState(const std::string& state, const std::string& error = "");

    SenderConfig cfg_;
    AudioDevice audio_;
    WavpackEncoder encoder_;
    Resampler resampler_; // 192kHz hardware rate -> cfg_.encode_sample_rate, when they differ
    bool resampling_enabled_ = false;
    std::unique_ptr<ByteRingBuffer> capture_ring_;
    std::unique_ptr<BlockQueue> block_queue_;
    std::vector<int16_t> channel_extract_buf_; // scratch space for de-interleaving L/R selection

    std::atomic<bool> running_{false};
    std::thread encode_thread_;
    std::thread network_thread_;

    mutable std::mutex status_mutex_;
    Status status_;

    std::atomic<uint64_t> bytes_sent_window_{0};
    uint32_t udp_seq_ = 0;
};

} // namespace mpx
