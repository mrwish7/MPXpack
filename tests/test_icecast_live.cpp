// Exercises IcecastSourceClient (PUT) and IcecastRelayClient (GET) against a
// REAL, user-supplied Icecast server — no audio hardware needed, uses the
// synthetic signal generator. Validates the assumptions flagged in
// docs/phase2-plan.md 2.5/2.6 that could only be confirmed empirically:
// unchunked streaming PUT body acceptance, Content-Type handling, and the
// relay client's block-sync resync on a GET response that starts mid-block.
//
// Usage: test_icecast_live.exe <host> <port> <mount> <user> <password> [duration_sec]
// Credentials are intentionally NOT hardcoded here — pass them on the
// command line so they never end up committed to source.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "app_config.h"
#include "byte_ring_buffer.h"
#include "icecast_relay_client.h"
#include "icecast_source_client.h"
#include "net_common.h"
#include "synthetic_signal.h"
#include "wavpack_decoder.h"
#include "wavpack_encoder.h"

using namespace mpx;

int main(int argc, char** argv) {
    if (argc < 6) {
        std::printf("Usage: %s <host> <port> <mount> <user> <password> [duration_sec]\n", argv[0]);
        return 2;
    }

    NetworkInit net_init;

    IcecastConfig cfg;
    cfg.host = argv[1];
    cfg.port = std::atoi(argv[2]);
    cfg.mount = argv[3];
    cfg.user = argv[4];
    cfg.password = argv[5];
    const int kDurationSec = argc > 6 ? std::atoi(argv[6]) : 8;

    std::printf("Testing Icecast at %s:%d%s for %ds\n", cfg.host.c_str(), cfg.port, cfg.mount.c_str(), kDurationSec);

    const int kNumFrames = kSampleRate * kDurationSec;
    std::vector<int16_t> signal = GenerateSineSweep(kNumFrames, kSampleRate, 50.0, 90000.0);

    std::atomic<bool> sender_done{false};
    std::atomic<bool> sender_failed{false};
    std::string sender_error;

    std::thread sender_thread([&] {
        IcecastSourceClient client;
        if (!client.connect(cfg)) {
            sender_failed = true;
            sender_error = "connect: " + client.lastError();
            sender_done = true;
            return;
        }
        std::printf("[sender] connected to Icecast source\n");

        WavpackEncoder encoder;
        WavpackEncoder::Config enc_cfg;
        enc_cfg.sample_rate = kSampleRate;
        enc_cfg.channels = kChannels;
        enc_cfg.bits_per_sample = kBpsDefault;
        enc_cfg.block_samples = kBlockSamples;

        bool send_failed = false;
        encoder.open(enc_cfg, [&](const uint8_t* data, size_t len) {
            if (send_failed) return;
            if (!client.sendBlock(data, len)) {
                send_failed = true;
                sender_error = "sendBlock: " + client.lastError();
            }
        });

        size_t offset = 0;
        const int block_ms = static_cast<int>(1000.0 * kBlockSamples / kSampleRate);
        while (offset + kBlockSamples <= signal.size() && !send_failed) {
            encoder.pushSamples(signal.data() + offset, kBlockSamples);
            offset += kBlockSamples;
            // Pace roughly real-time so the mount behaves like a live stream
            // rather than bursting the whole clip instantly.
            std::this_thread::sleep_for(std::chrono::milliseconds(block_ms));
        }
        encoder.flush();
        encoder.close();
        client.close();

        sender_failed = send_failed;
        sender_done = true;
        std::printf("[sender] finished (%s)\n", send_failed ? "failed" : "ok");
    });

    // Give the source a moment to register the mount before the relay
    // client tries to GET it.
    std::this_thread::sleep_for(std::chrono::milliseconds(750));

    IcecastRelayClient relay;
    if (!relay.connect(cfg)) {
        std::printf("FAILED: relay connect: %s\n", relay.lastError().c_str());
        sender_thread.join();
        return 1;
    }
    std::printf("[receiver] connected to Icecast mount\n");

    ByteRingBuffer ring(512 * 1024);
    std::atomic<bool> receiver_stop{false};
    std::thread receiver_net_thread([&] {
        uint8_t buf[4096];
        while (!receiver_stop.load()) {
            int n = relay.readBody(buf, sizeof(buf), 500);
            if (n > 0) ring.push(buf, static_cast<size_t>(n));
            else if (n == 0) break; // connection ended
        }
    });

    WavpackDecoder decoder;
    if (!decoder.open(&ring, 500)) {
        std::printf("FAILED: decoder.open(): %s\n", decoder.lastError().c_str());
        receiver_stop = true;
        receiver_net_thread.join();
        sender_thread.join();
        relay.close();
        return 1;
    }

    std::vector<int32_t> decode_buf(kBlockSamples * kChannels);
    size_t decoded_frames = 0;
    int idle_after_done = 0;
    auto start = std::chrono::steady_clock::now();
    const double max_wait_sec = kDurationSec + 5.0;

    for (;;) {
        double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (elapsed > max_wait_sec) break;

        uint32_t got = decoder.decode(decode_buf.data(), kBlockSamples);
        decoded_frames += got;

        if (sender_done.load() && got == 0) {
            if (++idle_after_done > 5) break;
        } else if (got > 0) {
            idle_after_done = 0;
        }
    }

    receiver_stop = true;
    receiver_net_thread.join();
    sender_thread.join();
    relay.close();

    std::printf("Decoded %zu frames out of %d sent (errors reported by libwavpack: %d), sender %s.\n",
                decoded_frames, kNumFrames, decoder.lastErrorCount(), sender_failed.load() ? "FAILED" : "OK");

    if (sender_failed.load()) {
        std::printf("Sender error: %s\n", sender_error.c_str());
        return 1;
    }
    if (decoded_frames < static_cast<size_t>(kNumFrames) / 2) {
        std::printf("FAILED: decoded too little audio via the real Icecast round trip.\n");
        return 1;
    }

    std::printf("PASS\n");
    return 0;
}
