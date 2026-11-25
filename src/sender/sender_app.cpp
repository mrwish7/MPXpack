#include "sender_app.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "udp_framing.h"

namespace mpx {

namespace {
constexpr int kCaptureChunkFrames = 480; // 2.5ms @ 192kHz, popped from the capture ring each iteration
constexpr int kEncodeReadTimeoutMs = 200;
constexpr int kNetworkPopTimeoutMs = 200;
constexpr int kReconnectBackoffStartMs = 1000;
constexpr int kReconnectBackoffMaxMs = 30000;
} // namespace

SenderApp::SenderApp() = default;

SenderApp::~SenderApp() {
    stop();
}

std::vector<AudioDeviceInfo> SenderApp::listInputDevices() {
    return AudioDevice::enumerate(AudioDeviceKind::Capture);
}

bool SenderApp::validateDevice(const std::string& device_id) {
    return AudioDevice::supportsFormatNatively(AudioDeviceKind::Capture, device_id, kSampleRate, kChannels);
}

void SenderApp::setState(const std::string& state, const std::string& error) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.state = state;
    if (!error.empty()) status_.last_error = error;
}

SenderApp::Status SenderApp::status() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}

bool SenderApp::start(const SenderConfig& cfg) {
    if (running_.load()) return false;
    cfg_ = cfg;
    udp_seq_ = 0;

    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_ = Status{};
        status_.state = "Connecting";
    }

    // Raw PCM at 192kHz/mono/16-bit is a fixed 384000 bytes/sec regardless
    // of the chosen hybrid bps (that only affects the *encoded* side); size
    // the pre-encode ring off that.
    const size_t raw_bytes_per_sec = static_cast<size_t>(kSampleRate) * kChannels * sizeof(int16_t);
    size_t capture_capacity = raw_bytes_per_sec * static_cast<size_t>(cfg_.max_latency_ms) / 1000;
    capture_ring_ = std::make_unique<ByteRingBuffer>(std::max<size_t>(capture_capacity, raw_bytes_per_sec / 10));

    // Block duration is in terms of encode-rate samples, not the hardware
    // rate — a block of kBlockSamples represents more wall-clock time at
    // 128kHz than at 192kHz.
    const double block_ms = 1000.0 * kBlockSamples / cfg_.encode_sample_rate;
    size_t max_blocks = std::max<size_t>(4, static_cast<size_t>(cfg_.max_latency_ms / block_ms));
    block_queue_ = std::make_unique<BlockQueue>(max_blocks);

    WavpackEncoder::Config enc_cfg;
    enc_cfg.sample_rate = cfg_.encode_sample_rate;
    enc_cfg.channels = kChannels;
    enc_cfg.bits_per_sample = cfg_.bits_per_sample;
    enc_cfg.block_samples = kBlockSamples;
    enc_cfg.lossless = cfg_.lossless;
    // Very-high quality mode: ~1.3x encode CPU cost for genuinely better
    // prediction (measured +2.9dB RDS SNR vs the plain default), with block
    // size still safely under the UDP budget (~1000 bytes vs the 1200 cap).
    // noise_shaping is deliberately left at its default (LibraryDefault) —
    // measurement showed forcing it either way doesn't reliably help, and
    // static shaping specifically hurts (see tests/test_noise_floor_spectrum.cpp).
    enc_cfg.very_high_quality = true;

    if (!encoder_.open(enc_cfg, [this](const uint8_t* data, size_t len) {
            block_queue_->push(std::vector<uint8_t>(data, data + len));
        })) {
        setState("Error", "encoder: " + encoder_.lastError());
        return false;
    }

    resampling_enabled_ = (cfg_.encode_sample_rate != kSampleRate);
    if (resampling_enabled_ && !resampler_.open(kSampleRate, cfg_.encode_sample_rate, kChannels)) {
        setState("Error", "resampler: " + resampler_.lastError());
        encoder_.close();
        return false;
    }

    // "Both" keeps the existing behavior (request mono directly; miniaudio's
    // own channel converter mixes the device's native channels down). L/R
    // selection instead captures true stereo — no mixing at all — so the
    // unwanted channel can be dropped outright rather than averaged into
    // the one we want (averaging would garble both when L/R carry two
    // unrelated services, which is exactly the case this exists for).
    bool extracting_channel = (cfg_.channel_select != ChannelSelect::Both);
    if (extracting_channel) {
        // A device with no native stereo/multichannel format has no real
        // "left" or "right" to extract — fall back to Both. Functionally
        // this direction (requesting 2 channels from a genuinely mono
        // device) is a mono->stereo upmix, which miniaudio should handle by
        // duplication rather than the summing/averaging that made the
        // output-side equivalent risky — so extracting either channel
        // would likely still land on the correct signal either way — but
        // that's an assumption about the converter's behavior this project
        // hasn't specifically tested, and the setting is meaningless on a
        // device with no separate channels regardless. See the matching
        // check in receiver_app.cpp.
        auto formats = AudioDevice::listNativeFormats(AudioDeviceKind::Capture, cfg_.input_device_id);
        bool supports_stereo = false;
        for (const auto& f : formats) {
            if (f.channels == 0 || f.channels >= 2) { supports_stereo = true; break; }
        }
        if (!supports_stereo) extracting_channel = false;
    }
    const int capture_channels = extracting_channel ? 2 : kChannels;
    const size_t channel_offset = (cfg_.channel_select == ChannelSelect::Right) ? 1 : 0;

    bool capture_opened = audio_.openCapture(
        cfg_.input_device_id, kSampleRate, capture_channels,
        [this, extracting_channel, channel_offset](const int16_t* pcm, size_t frame_count) {
            const int16_t* mono_pcm = pcm;
            if (extracting_channel) {
                if (channel_extract_buf_.size() < frame_count) channel_extract_buf_.resize(frame_count);
                for (size_t i = 0; i < frame_count; ++i) {
                    channel_extract_buf_[i] = pcm[i * 2 + channel_offset];
                }
                mono_pcm = channel_extract_buf_.data();
            }
            capture_ring_->push(reinterpret_cast<const uint8_t*>(mono_pcm),
                                 frame_count * kChannels * sizeof(int16_t));
            updateLevel(mono_pcm, frame_count * kChannels);
        });
    if (!capture_opened) {
        setState("Error", "audio device: " + audio_.lastError());
        if (resampling_enabled_) resampler_.close();
        encoder_.close();
        return false;
    }

    if (!audio_.start()) {
        setState("Error", "audio device: " + audio_.lastError());
        if (resampling_enabled_) resampler_.close();
        audio_.close();
        encoder_.close();
        return false;
    }

    running_ = true;
    encode_thread_ = std::thread(&SenderApp::encodeThreadFunc, this);
    network_thread_ = std::thread(&SenderApp::networkThreadFunc, this);

    return true;
}

void SenderApp::stop() {
    if (!running_.load()) return;
    running_ = false;

    audio_.stop();
    audio_.close();

    if (capture_ring_) capture_ring_->requestStop();
    if (block_queue_) block_queue_->requestStop();

    if (encode_thread_.joinable()) encode_thread_.join();
    if (network_thread_.joinable()) network_thread_.join();

    encoder_.flush();
    encoder_.close();

    setState("Idle");
}

void SenderApp::updateLevel(const int16_t* pcm, size_t n) {
    if (n == 0) return;
    double peak = 0.0, sum_sq = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double v = std::abs(static_cast<int>(pcm[i])) / 32768.0;
        peak = std::max(peak, v);
        sum_sq += v * v;
    }
    double rms = std::sqrt(sum_sq / static_cast<double>(n));

    std::lock_guard<std::mutex> lock(status_mutex_);
    // Simple decay so the meter doesn't look frozen between GUI polls.
    status_.input_peak = std::max(peak, status_.input_peak * 0.7);
    status_.input_rms = rms;
}

void SenderApp::encodeThreadFunc() {
    std::vector<int16_t> chunk(kCaptureChunkFrames * kChannels);
    std::vector<int16_t> resampled;

    while (running_.load()) {
        size_t got_bytes = capture_ring_->popBlocking(reinterpret_cast<uint8_t*>(chunk.data()),
                                                        chunk.size() * sizeof(int16_t),
                                                        kEncodeReadTimeoutMs);
        size_t got_frames = got_bytes / (kChannels * sizeof(int16_t));
        if (got_frames == 0) continue;

        const int16_t* push_data = chunk.data();
        size_t push_frames = got_frames;

        if (resampling_enabled_) {
            resampled.clear();
            if (!resampler_.process(chunk.data(), got_frames, resampled)) {
                setState("Error", "resampler: " + resampler_.lastError());
                running_ = false;
                break;
            }
            push_data = resampled.data();
            push_frames = resampled.size() / kChannels;
            if (push_frames == 0) continue; // filter is still buffering; nothing to push yet
        }

        if (!encoder_.pushSamples(push_data, push_frames)) {
            setState("Error", "encoder: " + encoder_.lastError());
            running_ = false;
            break;
        }
    }

    if (resampling_enabled_) {
        std::vector<int16_t> tail;
        if (resampler_.flush(tail) && !tail.empty()) {
            encoder_.pushSamples(tail.data(), tail.size() / kChannels);
        }
        resampler_.close();
    }
}

void SenderApp::networkThreadFunc() {
    if (cfg_.transport == TransportMode::Udp) {
        networkThreadUdp();
    } else {
        networkThreadIcecast();
    }
}

void SenderApp::networkThreadUdp() {
    UdpSocket sock;
    if (!sock.openForSend(cfg_.udp.address, cfg_.udp.port)) {
        setState("Error", "UDP: " + sock.lastError());
        running_ = false;
        return;
    }
    setState("Streaming");

    auto window_start = std::chrono::steady_clock::now();
    uint64_t window_bytes = 0;

    while (running_.load()) {
        auto block = block_queue_->popBlocking(kNetworkPopTimeoutMs);
        if (block.empty()) continue;

        auto packets = FragmentBlock(block.data(), block.size(), udp_seq_++);
        for (auto& p : packets) {
            if (sock.send(p.data(), p.size())) {
                window_bytes += p.size();
            }
        }

        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - window_start).count();
        if (elapsed >= 1.0) {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.outgoing_kbps = window_bytes * 8.0 / 1000.0 / elapsed;
            // Block duration is in terms of encode-rate samples, not the
            // fixed hardware rate (kSampleRate) — a block represents more
            // wall-clock time at a lower encode_sample_rate.
            double block_ms = 1000.0 * kBlockSamples / cfg_.encode_sample_rate;
            status_.queue_depth_ms = static_cast<int>(block_queue_->size() * block_ms);
            window_bytes = 0;
            window_start = now;
        }
    }
}

void SenderApp::networkThreadIcecast() {
    IcecastSourceClient client;
    int backoff_ms = kReconnectBackoffStartMs;

    auto window_start = std::chrono::steady_clock::now();
    uint64_t window_bytes = 0;

    while (running_.load()) {
        if (!client.isOpen()) {
            setState(backoff_ms == kReconnectBackoffStartMs ? "Connecting" : "Reconnecting");
            if (!client.connect(cfg_.icecast)) {
                setState("Reconnecting", "Icecast: " + client.lastError());
                for (int waited = 0; waited < backoff_ms && running_.load(); waited += 100) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                backoff_ms = std::min(backoff_ms * 2, kReconnectBackoffMaxMs);
                continue;
            }
            backoff_ms = kReconnectBackoffStartMs;
            setState("Streaming");
        }

        auto block = block_queue_->popBlocking(kNetworkPopTimeoutMs);
        if (block.empty()) continue;

        if (!client.sendBlock(block.data(), block.size())) {
            setState("Reconnecting", "Icecast: " + client.lastError());
            client.close();
            continue;
        }
        window_bytes += block.size();

        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - window_start).count();
        if (elapsed >= 1.0) {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.outgoing_kbps = window_bytes * 8.0 / 1000.0 / elapsed;
            // Block duration is in terms of encode-rate samples, not the
            // fixed hardware rate (kSampleRate) — a block represents more
            // wall-clock time at a lower encode_sample_rate.
            double block_ms = 1000.0 * kBlockSamples / cfg_.encode_sample_rate;
            status_.queue_depth_ms = static_cast<int>(block_queue_->size() * block_ms);
            window_bytes = 0;
            window_start = now;
        }
    }

    client.close();
}

} // namespace mpx
