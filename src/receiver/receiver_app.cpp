#include "receiver_app.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace mpx {

namespace {
constexpr int kUdpReceiveTimeoutMs = 200;
constexpr int kNoSignalThresholdMs = 2000;
constexpr int kDecodeReadTimeoutMs = 300;
constexpr int kIcecastReadTimeoutMs = 300;
constexpr int kReconnectBackoffStartMs = 1000;
constexpr int kReconnectBackoffMaxMs = 30000;
constexpr int kDecoderOpenRetryDelayMs = 250;
// Decode chunk size — independent of whatever period size miniaudio's
// playback callback asks for; the two are decoupled by playback_ring_.
constexpr int kPlaybackChunkFrames = 480;
} // namespace

ReceiverApp::ReceiverApp() = default;

ReceiverApp::~ReceiverApp() {
    stop();
}

std::vector<AudioDeviceInfo> ReceiverApp::listOutputDevices() {
    return AudioDevice::enumerate(AudioDeviceKind::Playback);
}

bool ReceiverApp::validateDevice(const std::string& device_id) {
    return AudioDevice::supportsFormatNatively(AudioDeviceKind::Playback, device_id, kSampleRate, kChannels);
}

void ReceiverApp::setState(const std::string& state, const std::string& error) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.state = state;
    if (!error.empty()) status_.last_error = error;
}

ReceiverApp::Status ReceiverApp::status() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}

bool ReceiverApp::start(const ReceiverConfig& cfg) {
    if (running_.load()) return false;
    cfg_ = cfg;

    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_ = Status{};
        status_.state = "Connecting";
    }

    resampling_enabled_ = false;
    detected_sample_rate_ = 0;
    decoder_needs_reopen_ = false;
    resampler_.close();

    const size_t raw_bytes_per_sec = static_cast<size_t>(kSampleRate) * kChannels * sizeof(int16_t);

    // Network ring holds compressed bytes. The receiver doesn't know in
    // advance which encode rate/bps/lossless setting the sender picked
    // (all sender-side config, auto-detected here — see decodeThreadFunc),
    // so size conservatively off raw PCM bytes/sec at the fixed hardware
    // rate: hybrid mode at any bps is always well under this, and even
    // lossless mode's worst case (highly incompressible content) only
    // approaches, but shouldn't exceed, the raw bit-depth-equivalent rate.
    size_t network_capacity = raw_bytes_per_sec * static_cast<size_t>(cfg_.max_latency_ms) / 1000;
    network_ring_ = std::make_unique<ByteRingBuffer>(std::max<size_t>(network_capacity, 16 * 1024));

    // Playback ring holds decoded raw PCM, sized off the same latency budget.
    size_t playback_capacity = std::max<size_t>(
        raw_bytes_per_sec * static_cast<size_t>(cfg_.max_latency_ms) / 1000, raw_bytes_per_sec / 10);
    playback_ring_ = std::make_unique<ByteRingBuffer>(playback_capacity);

    // Deliberately prebuffer half the configured latency budget before the
    // callback below drains anything at all — without this, the ring's
    // *capacity* can be as generous as you like, but its actual
    // steady-state depth never approaches it (nothing ever builds toward
    // it), so raising max_latency_ms alone does nothing for jitter that
    // shows up as an ordinary, brief scheduling delay rather than sustained
    // loss. This trades a proportional bit of startup delay for a real,
    // ongoing cushion against exactly that.
    playback_primed_ = false;
    playback_prebuffer_bytes_ = playback_capacity / 2;

    // Deliberately NOT opening the decoder here: decoder_.open() needs the
    // first WavPack block's header bytes available within a short timeout,
    // but network_thread_ (the only thing that ever supplies bytes into
    // network_ring_) hasn't started yet at this point — that would fail
    // 100% of the time, not just under bad timing. The decode thread opens
    // it lazily instead, retrying until bytes actually start arriving (see
    // decodeThreadFunc).

    // "Both" keeps the existing behavior (open the device mono; miniaudio's
    // own channel converter duplicates it to L+R). L/R selection instead
    // opens the device in real stereo and injects the signal into just the
    // chosen channel, silencing the other outright — the output-side
    // mirror of the sender's capture-side channel extraction.
    bool injecting_channel = (cfg_.channel_select != ChannelSelect::Both);
    if (injecting_channel) {
        // A device with no native stereo/multichannel format has no real
        // "left" or "right" to select. Rather than depend on however
        // miniaudio's channel converter happens to downmix a fabricated
        // 2-channel request to the device's true mono format on a given
        // platform/driver (untested — the proven direction is the reverse,
        // mono->duplicate, used by the "Both" path below), just fall back
        // to Both outright: a mono-only device gets the same well-proven
        // behavior regardless of this setting.
        auto formats = AudioDevice::listNativeFormats(AudioDeviceKind::Playback, cfg_.output_device_id);
        bool supports_stereo = false;
        for (const auto& f : formats) {
            if (f.channels == 0 || f.channels >= 2) { supports_stereo = true; break; }
        }
        if (!supports_stereo) injecting_channel = false;
    }
    const int playback_channels = injecting_channel ? 2 : kChannels;
    const size_t channel_offset = (cfg_.channel_select == ChannelSelect::Right) ? 1 : 0;

    bool playback_opened = audio_.openPlayback(
        cfg_.output_device_id, kSampleRate, playback_channels,
        [this, injecting_channel, channel_offset](int16_t* pcm, size_t frame_count) -> size_t {
            const int pcm_channels = injecting_channel ? 2 : kChannels;
            if (!playback_primed_.load()) {
                if (playback_ring_->availableBytes() >= playback_prebuffer_bytes_) {
                    playback_primed_ = true;
                } else {
                    // Still priming: emit silence without touching the ring
                    // at all, so a real cushion actually builds up before
                    // anything starts eating into it.
                    std::memset(pcm, 0, frame_count * static_cast<size_t>(pcm_channels) * sizeof(int16_t));
                    return frame_count;
                }
            }

            size_t got_frames;
            if (injecting_channel) {
                if (channel_inject_buf_.size() < frame_count) channel_inject_buf_.resize(frame_count);
                size_t want_bytes = frame_count * kChannels * sizeof(int16_t); // mono bytes wanted
                size_t got_bytes = playback_ring_->popBlocking(
                    reinterpret_cast<uint8_t*>(channel_inject_buf_.data()), want_bytes, 0);
                got_frames = got_bytes / (kChannels * sizeof(int16_t));
                for (size_t i = 0; i < got_frames; ++i) {
                    pcm[i * 2 + channel_offset] = channel_inject_buf_[i];
                    pcm[i * 2 + (1 - channel_offset)] = 0;
                }
            } else {
                size_t want_bytes = frame_count * kChannels * sizeof(int16_t);
                size_t got_bytes = playback_ring_->popBlocking(reinterpret_cast<uint8_t*>(pcm), want_bytes, 0);
                got_frames = got_bytes / (kChannels * sizeof(int16_t));
            }

            double peak = 0.0;
            for (size_t i = 0; i < got_frames * static_cast<size_t>(pcm_channels); ++i) {
                peak = std::max(peak, std::abs(static_cast<int>(pcm[i])) / 32768.0);
            }
            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                status_.output_peak = std::max(peak, status_.output_peak * 0.7);
            }
            return got_frames;
        });
    if (!playback_opened) {
        setState("Error", "audio device: " + audio_.lastError());
        return false;
    }

    if (!audio_.start()) {
        setState("Error", "audio device: " + audio_.lastError());
        audio_.close();
        return false;
    }

    running_ = true;
    decode_thread_ = std::thread(&ReceiverApp::decodeThreadFunc, this);
    network_thread_ = std::thread(&ReceiverApp::networkThreadFunc, this);

    return true;
}

void ReceiverApp::stop() {
    if (!running_.load()) return;
    running_ = false;

    if (network_ring_) network_ring_->requestStop();
    if (playback_ring_) playback_ring_->requestStop();

    if (network_thread_.joinable()) network_thread_.join();
    if (decode_thread_.joinable()) decode_thread_.join();

    audio_.stop();
    audio_.close();
    decoder_.close();
    resampler_.close();

    setState("Idle");
}

bool ReceiverApp::openDecoderWithRetry() {
    // Retries until network_thread_ has actually delivered the first
    // WavPack block header — this can legitimately take a while (Icecast
    // connect/HTTP round trip, or just waiting for the sender to start), so
    // this is a real retry loop, not a fixed-timeout gamble. Deliberately
    // doesn't call setState() here: the network thread already reports
    // Connecting/Reconnecting/No signal, and layering a second status
    // source on top of it would just make the GUI flicker between messages.
    while (running_.load() && !decoder_.open(network_ring_.get(), kDecodeReadTimeoutMs)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kDecoderOpenRetryDelayMs));
    }
    return running_.load();
}

void ReceiverApp::decodeThreadFunc() {
    std::vector<int32_t> decode_buf(kPlaybackChunkFrames * kChannels);
    std::vector<int16_t> narrow_buf(kPlaybackChunkFrames * kChannels);
    std::vector<int16_t> resampled;

    if (!openDecoderWithRetry()) return; // stopped before a decoder connection was ever established

    while (running_.load()) {
        if (decoder_needs_reopen_.exchange(false)) {
            // Flagged by the network thread after a detected drop/reconnect
            // — close and reopen rather than trust WavPack's block-sync to
            // resync, so a sender restart with a *different*
            // encode_sample_rate (app_config.h) is picked up correctly.
            decoder_.close();
            detected_sample_rate_ = 0;
            resampling_enabled_ = false;
            resampler_.close();
            if (!openDecoderWithRetry()) break;
            continue;
        }

        uint32_t got = decoder_.decode(decode_buf.data(), kPlaybackChunkFrames);
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.decoder_error_count = decoder_.lastErrorCount();
            status_.buffer_depth_ms = static_cast<int>(
                playback_ring_->availableBytes() * 1000.0 /
                (kSampleRate * kChannels * sizeof(int16_t)));
        }
        if (got == 0) continue; // transient stall on the network ring; just retry

        // The stream's actual sample rate is only known once real samples
        // have been decoded (WavPack self-describes it in the block
        // header) — re-checked every chunk (cheap: a struct-field read)
        // rather than decided once, so a rate change picked up by a fresh
        // decoder context (see decoder_needs_reopen_ above) takes effect
        // immediately instead of leaving playback pitched wrong for the
        // rest of the session. The sender has the rate selector
        // (app_config.h); the receiver just adapts.
        //
        // Deliberately NOT resampled at all when stream_rate == kSampleRate
        // (a true, exact passthrough — no filter, no ratio, zero risk of
        // disturbing carrier frequencies) rather than routing everything
        // through a nominal-1.0 resampler. A previous version did the
        // latter specifically to give a clock-drift-correction loop a
        // ratio to nudge in every mode — that correction was removed after
        // it was found to visibly wobble the RDS/stereo subcarriers in a
        // real SDR capture: changing a resampler's ratio shifts every
        // frequency component passing through it, and recalculating that
        // adjustment every couple of seconds is a continuously *changing*
        // shift — i.e. genuine frequency modulation of the very carriers
        // this app exists to relay intact. Inaudible-to-a-human-ear was the
        // wrong bar for a signal whose entire purpose is carrying
        // frequency-critical subcarriers to a PLL-based FM receiver.
        int stream_rate = decoder_.sampleRate();
        if (stream_rate != 0 && stream_rate != detected_sample_rate_) {
            resampling_enabled_ = (stream_rate != kSampleRate);
            resampler_.close();
            if (resampling_enabled_) {
                resampler_.open(stream_rate, kSampleRate, kChannels);
            }
            detected_sample_rate_ = stream_rate;
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.stream_sample_rate = stream_rate;
        }

        for (uint32_t i = 0; i < got * kChannels; ++i) {
            narrow_buf[i] = static_cast<int16_t>(decode_buf[i]);
        }

        if (resampling_enabled_) {
            resampled.clear();
            if (resampler_.process(narrow_buf.data(), got, resampled) && !resampled.empty()) {
                playback_ring_->push(reinterpret_cast<const uint8_t*>(resampled.data()),
                                      resampled.size() * sizeof(int16_t));
            }
        } else {
            playback_ring_->push(reinterpret_cast<const uint8_t*>(narrow_buf.data()),
                                  got * kChannels * sizeof(int16_t));
        }
    }
}

void ReceiverApp::networkThreadFunc() {
    if (cfg_.transport == TransportMode::Udp) {
        networkThreadUdp();
    } else {
        networkThreadIcecast();
    }
}

void ReceiverApp::networkThreadUdp() {
    UdpSocket sock;
    if (!sock.openForReceive(cfg_.udp.address, cfg_.udp.port)) {
        setState("Error", "UDP: " + sock.lastError());
        running_ = false;
        return;
    }

    // Fallback assumption for the brief window before the incoming
    // stream's actual encode rate has been detected (status_.stream_sample_
    // rate, populated by decodeThreadFunc) — 125 blocks/sec at
    // kBlockSamples=1536 / 192kHz, one packet per block (see
    // docs/phase2-plan.md block-size derivation). Using this as a
    // *permanent* assumption instead of the detected rate previously made
    // the loss-rate stat wildly wrong whenever the sender's encode rate is
    // below 192kHz: at the 128kHz default, 128000/1536 =~ 83.3 real
    // packets/sec against an assumed 125/sec computes as ~33% "loss" with
    // zero packets actually lost.
    constexpr double kFallbackPacketsPerSec = static_cast<double>(kSampleRate) / kBlockSamples;

    // UdpReassembler's reorder-tolerance window is a packet *count*,
    // completely independent of the byte-based ring buffers that
    // max_latency_ms otherwise sizes — raising max_latency_ms alone does
    // nothing for packets that arrive out of order beyond this window; they
    // still get treated as unrecoverable and dropped (see
    // UdpReassembler::onPacketReceived's "too far ahead" branch). Size it
    // off the same latency budget instead of the previous fixed default of
    // 4 (a mere ~32ms of reorder tolerance at 125 blocks/sec — far tighter
    // than the rest of the pipeline's own configured latency budget), using
    // the conservative highest-possible block rate so it stays a safe
    // (generous) estimate regardless of the sender's actual encode rate.
    constexpr size_t kMinReorderWindow = 4;
    size_t reorder_window = std::max<size_t>(
        kMinReorderWindow, static_cast<size_t>(cfg_.max_latency_ms * kFallbackPacketsPerSec / 1000.0));
    UdpReassembler reassembler(reorder_window);
    setState("No signal");

    auto last_packet_time = std::chrono::steady_clock::now();
    auto window_start = last_packet_time;
    uint64_t window_packets_start = 0;
    uint64_t last_resync_count = 0;

    uint8_t buf[2048];
    while (running_.load()) {
        int n = sock.receive(buf, sizeof(buf), kUdpReceiveTimeoutMs);
        auto now = std::chrono::steady_clock::now();

        if (n > 0) {
            if (status().state == "No signal") {
                // Packets resumed after a sustained gap — likely a sender
                // restart (possibly with a different encode_sample_rate,
                // see app_config.h) rather than ordinary packet loss/reorder.
                // Reset first so the fresh packet below isn't wiped by it.
                network_ring_->reset();
                decoder_needs_reopen_ = true;
                setState("Streaming");
            }
            reassembler.onPacketReceived(buf, static_cast<size_t>(n), *network_ring_);
            last_packet_time = now;

            // A sender restart resets its seq counter to 0 (see
            // SenderApp::start), which UdpReassembler can only recognize
            // *after* the fact (see UdpReassembler::kResyncThreshold) since
            // packets never stopped arriving — the "No signal" branch above
            // doesn't fire for this case. Treat a detected resync exactly
            // like a "No signal" recovery: purge stale bytes and force the
            // decoder to reopen so it re-detects the new stream's rate.
            uint64_t resyncs = reassembler.resyncCount();
            if (resyncs != last_resync_count) {
                last_resync_count = resyncs;
                network_ring_->reset();
                decoder_needs_reopen_ = true;
            }
        }

        double since_last_ms = std::chrono::duration<double, std::milli>(now - last_packet_time).count();
        if (since_last_ms > kNoSignalThresholdMs) {
            setState("No signal");
        }

        double window_elapsed = std::chrono::duration<double>(now - window_start).count();
        if (window_elapsed >= 1.0) {
            uint64_t received_this_window = reassembler.receivedPacketCount() - window_packets_start;
            int detected_rate = status().stream_sample_rate; // 0 until the decode thread detects it
            double packets_per_sec = (detected_rate > 0)
                ? static_cast<double>(detected_rate) / kBlockSamples
                : kFallbackPacketsPerSec;
            double expected = packets_per_sec * window_elapsed;
            double loss_rate = expected > 0 ? std::max(0.0, 1.0 - received_this_window / expected) : 0.0;

            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.udp_loss_rate = std::min(1.0, loss_rate);
            status_.udp_dropped_blocks = reassembler.droppedBlockCount();
            window_packets_start = reassembler.receivedPacketCount();
            window_start = now;
        }
    }
}

void ReceiverApp::networkThreadIcecast() {
    IcecastRelayClient client;
    int backoff_ms = kReconnectBackoffStartMs;
    uint8_t buf[4096];
    auto last_data_time = std::chrono::steady_clock::now();

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
            last_data_time = std::chrono::steady_clock::now();
        }

        int n = client.readBody(buf, sizeof(buf), kIcecastReadTimeoutMs);
        auto now = std::chrono::steady_clock::now();

        if (n > 0) {
            network_ring_->push(buf, static_cast<size_t>(n));
            last_data_time = now;
            continue;
        }

        if (n == 0) {
            setState("Reconnecting", "Icecast: connection ended");
        } else {
            // n < 0: timeout. Some Icecast configs don't proactively close
            // a listener's connection when the source drops (e.g. a
            // sender restart), so the socket can sit "open" indefinitely
            // with no bytes ever arriving again — detect that by elapsed
            // time since the last byte, not just a closed socket, and
            // force a reconnect either way.
            double since_data_ms = std::chrono::duration<double, std::milli>(now - last_data_time).count();
            if (since_data_ms <= kNoSignalThresholdMs) continue;
            setState("Reconnecting", "Icecast: no data received, reconnecting");
        }

        // A fresh connection can start mid-block; WavpackDecoder's internal
        // block-sync scan would resolve that on its own (see
        // docs/phase2-plan.md 2.6), but closing/reopening the decoder
        // (decoder_needs_reopen_) is more direct and also lets a sender
        // restart's new encode_sample_rate (app_config.h) be re-detected.
        client.close();
        network_ring_->reset();
        decoder_needs_reopen_ = true;
    }

    client.close();
}

} // namespace mpx
