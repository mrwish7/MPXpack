// Regression test for a real bug: ReceiverApp::start() used to call
// WavpackDecoder::open() synchronously before network_thread_ (the only
// thing that ever supplies bytes into the ring) had even started —
// guaranteeing "can't read all of WavPack file!" on every single receiver
// session, not just under bad timing. The fix (see
// ReceiverApp::decodeThreadFunc) retries decoder.open() in a loop until
// data actually starts flowing. This test validates that the underlying
// primitives actually support that recovery: a decoder repeatedly attempts
// to open against a byte source that stays empty for a while before a
// producer thread starts feeding it valid WavPack blocks, simulating a
// receiver started before its sender (or before Icecast has delivered
// anything yet).

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "app_config.h"
#include "byte_ring_buffer.h"
#include "synthetic_signal.h"
#include "wavpack_decoder.h"
#include "wavpack_encoder.h"

using namespace mpx;

int main() {
    ByteRingBuffer ring(256 * 1024);
    WavpackDecoder decoder;

    // First attempt, with nothing in the ring yet, must fail (quickly) —
    // this reproduces the exact bug scenario: calling open() with no
    // producer running yet.
    auto attempt_start = std::chrono::steady_clock::now();
    bool first_attempt_ok = decoder.open(&ring, /*read_timeout_ms=*/300);
    double first_attempt_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - attempt_start).count();
    std::printf("First open() attempt (empty ring): %s after %.2fs\n",
                first_attempt_ok ? "succeeded (unexpected)" : "failed as expected", first_attempt_sec);
    if (first_attempt_ok) {
        std::printf("FAILED: first attempt should not have succeeded against an empty ring.\n");
        return 1;
    }

    // Simulate the sender starting late: after a delay, a producer thread
    // begins encoding a synthetic signal into the ring.
    std::atomic<bool> stop_producer{false};
    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(800)); // sender "starts late"

        WavpackEncoder encoder;
        WavpackEncoder::Config cfg;
        cfg.sample_rate = kSampleRate;
        cfg.channels = kChannels;
        cfg.bits_per_sample = kBpsDefault;
        cfg.block_samples = kBlockSamples;
        encoder.open(cfg, [&](const uint8_t* data, size_t len) { ring.push(data, len); });

        std::vector<int16_t> signal = GenerateSineSweep(kSampleRate * 2, kSampleRate, 50.0, 90000.0);
        size_t offset = 0;
        while (!stop_producer.load() && offset + kBlockSamples <= signal.size()) {
            encoder.pushSamples(signal.data() + offset, kBlockSamples);
            offset += kBlockSamples;
            std::this_thread::sleep_for(std::chrono::milliseconds(8)); // pace ~real-time
        }
        encoder.flush();
        encoder.close();
    });

    // This mirrors the retry loop now in ReceiverApp::decodeThreadFunc.
    bool opened = false;
    auto retry_start = std::chrono::steady_clock::now();
    for (int attempts = 0; attempts < 20; ++attempts) {
        if (decoder.open(&ring, 300)) { opened = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    double retry_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - retry_start).count();

    stop_producer = true;
    producer.join();

    if (!opened) {
        std::printf("FAILED: decoder never opened even after the producer started sending data.\n");
        return 1;
    }
    std::printf("Retry loop succeeded after %.2fs once the producer started.\n", retry_sec);

    // Confirm it actually decodes real audio after recovering, not just
    // that open() returned true — any bytes consumed and discarded during
    // the earlier failed attempts must not prevent WavPack's block-sync
    // scan from resyncing on the next valid header.
    std::vector<int32_t> buf(kBlockSamples * kChannels);
    size_t total_decoded = 0;
    auto decode_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < decode_deadline &&
           total_decoded < static_cast<size_t>(kSampleRate) / 2) {
        uint32_t got = decoder.decode(buf.data(), kBlockSamples);
        total_decoded += got;
    }
    int errors = decoder.lastErrorCount();
    decoder.close();

    std::printf("Decoded %zu frames after recovery (decoder error count: %d).\n", total_decoded, errors);
    if (total_decoded == 0) {
        std::printf("FAILED: decoder opened but never produced any audio.\n");
        return 1;
    }

    std::printf("PASS\n");
    return 0;
}
