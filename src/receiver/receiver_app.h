#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app_config.h"
#include "audio_device.h"
#include "byte_ring_buffer.h"
#include "icecast_relay_client.h"
#include "resampler.h"
#include "udp_framing.h"
#include "udp_socket.h"
#include "wavpack_decoder.h"

namespace mpx {

// Orchestrates the receiver pipeline: network thread -> decode thread ->
// miniaudio playback. Same
// mutex+condvar-everywhere simplification noted in sender_app.h.
class ReceiverApp {
public:
    struct Status {
        std::string state = "Idle"; // Idle, Connecting, Streaming, Reconnecting, No signal, Error
        std::string last_error;
        double udp_loss_rate = 0.0; // 0..1, approximate, UDP mode only
        uint64_t udp_dropped_blocks = 0; // UDP mode only — reassembler gave up (see UdpReassembler)
        int decoder_error_count = 0;
        int buffer_depth_ms = 0;
        double output_peak = 0.0;
        int stream_sample_rate = 0; // 0 until detected from the incoming stream
    };

    ReceiverApp();
    ~ReceiverApp();

    static std::vector<AudioDeviceInfo> listOutputDevices();
    static bool validateDevice(const std::string& device_id);

    bool start(const ReceiverConfig& cfg);
    void stop();
    bool isRunning() const { return running_.load(); }

    Status status() const;

private:
    void networkThreadFunc();
    void networkThreadUdp();
    void networkThreadIcecast();
    void decodeThreadFunc();
    // Loops decoder_.open() until it succeeds or running_ goes false. Used
    // both for the initial open and for re-opening after a network-detected
    // drop/reconnect (see decoder_needs_reopen_).
    bool openDecoderWithRetry();
    void setState(const std::string& state, const std::string& error = "");

    ReceiverConfig cfg_;
    AudioDevice audio_;
    WavpackDecoder decoder_;
    // Auto-detected from the incoming stream's actual sample rate (see
    // decodeThreadFunc) rather than a receiver-side config option — the
    // sender is the one with a rate selector (app_config.h), the receiver
    // just adapts to whatever arrives. Only opened (and only ever routed
    // through) when stream_rate != kSampleRate — see decodeThreadFunc for
    // why a true, unconditional passthrough matters here.
    Resampler resampler_;
    bool resampling_enabled_ = false;
    int detected_sample_rate_ = 0; // 0 = not yet detected from the current decoder context
    // Set by the network thread when it detects a dropped/re-established
    // connection (Icecast reconnect, or UDP resuming after "No signal") so
    // the decode thread closes and reopens decoder_ instead of trusting
    // WavPack's own block-sync to resync — needed because
    // WavpackGetSampleRate() is latched from the first block a context
    // sees and never updates, so a sender restart with a different
    // encode_sample_rate (app_config.h) would otherwise go undetected.
    std::atomic<bool> decoder_needs_reopen_{false};
    std::unique_ptr<ByteRingBuffer> network_ring_;  // network thread -> decoder
    std::unique_ptr<ByteRingBuffer> playback_ring_; // decoder -> audio callback (raw int16 bytes)
    std::vector<int16_t> channel_inject_buf_; // scratch space for L/R-only output injection
    // Pre-buffering gate for the playback callback: with no prebuffering,
    // playback starts draining playback_ring_ the instant the device opens,
    // so its steady-state depth just tracks however far ahead decode
    // happens to be — often only a few ms — leaving no cushion against
    // ordinary scheduling jitter (see the callback in start()). Set once at
    // the start of a session, never re-armed — this targets a thin ongoing
    // margin, not recovery from a genuine later stall (the existing
    // reconnect/resync machinery handles that separately).
    std::atomic<bool> playback_primed_{false};
    size_t playback_prebuffer_bytes_ = 0;

    std::atomic<bool> running_{false};
    std::thread network_thread_;
    std::thread decode_thread_;

    mutable std::mutex status_mutex_;
    Status status_;
};

} // namespace mpx
