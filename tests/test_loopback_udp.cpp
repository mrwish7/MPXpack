// End-to-end UDP loopback test: encode -> fragment -> send over a real
// 127.0.0.1 UDP socket -> receive -> reassemble -> decode, with a synthetic
// packet-loss injector, to validate the "glitch and continue" resilience
// policy (see UdpReassembler in udp_framing.h) rather than a crash/stall.

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include "app_config.h"
#include "byte_ring_buffer.h"
#include "net_common.h"
#include "synthetic_signal.h"
#include "udp_framing.h"
#include "udp_socket.h"
#include "wavpack_decoder.h"
#include "wavpack_encoder.h"

using namespace mpx;

namespace {
constexpr int kListenPort = 47990;
constexpr int kDropEveryNth = 7; // simulate ~14% packet loss
} // namespace

int main() {
    NetworkInit net_init;

    const int kNumFrames = kSampleRate * 2; // 2 seconds
    std::vector<int16_t> original = GenerateSineSweep(kNumFrames, kSampleRate, 50.0, 90000.0);

    UdpSocket receiver_sock;
    if (!receiver_sock.openForReceive("127.0.0.1", kListenPort)) {
        std::printf("FAILED: receiver bind: %s\n", receiver_sock.lastError().c_str());
        return 1;
    }

    UdpSocket sender_sock;
    if (!sender_sock.openForSend("127.0.0.1", kListenPort)) {
        std::printf("FAILED: sender connect: %s\n", sender_sock.lastError().c_str());
        return 1;
    }

    ByteRingBuffer ring(256 * 1024);
    std::atomic<bool> encode_done{false};
    uint32_t seq = 0;
    size_t packets_sent = 0, packets_dropped = 0;

    WavpackEncoder encoder;
    WavpackEncoder::Config enc_cfg;
    enc_cfg.sample_rate = kSampleRate;
    enc_cfg.channels = kChannels;
    enc_cfg.bits_per_sample = kBpsDefault;
    enc_cfg.block_samples = kBlockSamples;

    bool opened = encoder.open(enc_cfg, [&](const uint8_t* data, size_t len) {
        auto packets = FragmentBlock(data, len, seq++);
        for (auto& p : packets) {
            packets_sent++;
            if (packets_sent % kDropEveryNth == 0) {
                packets_dropped++;
                continue; // simulate loss: never send it
            }
            sender_sock.send(p.data(), p.size());
        }
    });
    if (!opened) {
        std::printf("FAILED: encoder.open(): %s\n", encoder.lastError().c_str());
        return 1;
    }

    // Receiver thread: pulls datagrams off the socket, reassembles, feeds
    // the ring buffer — mirrors the real receiver app's network thread.
    UdpReassembler reassembler;
    std::thread receiver_thread([&] {
        uint8_t buf[2048];
        int idle_polls_after_done = 0;
        for (;;) {
            int n = receiver_sock.receive(buf, sizeof(buf), 100);
            if (n > 0) {
                reassembler.onPacketReceived(buf, static_cast<size_t>(n), ring);
            }
            if (encode_done.load()) {
                // Drain a little longer in case anything is still in flight,
                // then give up — this is a bounded test, not a live session.
                if (n <= 0 && ++idle_polls_after_done > 20) break;
            }
        }
    });

    size_t offset = 0;
    while (offset + kBlockSamples <= original.size()) {
        encoder.pushSamples(original.data() + offset, kBlockSamples);
        offset += kBlockSamples;
    }
    encoder.flush();
    encoder.close();
    encode_done = true;

    receiver_thread.join();

    std::printf("Sent %zu packets, simulated-dropped %zu (%.1f%%), reassembler saw %llu, dropped whole blocks %llu.\n",
                packets_sent, packets_dropped, 100.0 * packets_dropped / packets_sent,
                static_cast<unsigned long long>(reassembler.receivedPacketCount()),
                static_cast<unsigned long long>(reassembler.droppedBlockCount()));

    WavpackDecoder decoder;
    if (!decoder.open(&ring, /*read_timeout_ms=*/50)) {
        std::printf("FAILED: decoder.open(): %s\n", decoder.lastError().c_str());
        return 1;
    }

    std::vector<int32_t> decoded_buf(kBlockSamples * kChannels);
    size_t decoded_frames = 0;
    int consecutive_empty = 0;
    for (;;) {
        uint32_t got = decoder.decode(decoded_buf.data(), kBlockSamples);
        if (got == 0) {
            if (++consecutive_empty > 5) break; // end of the (finite, pre-populated) ring buffer
            continue;
        }
        consecutive_empty = 0;
        decoded_frames += got;
    }

    std::printf("Decoded %zu frames out of %d original (loss-tolerant; some gap is expected).\n",
                decoded_frames, kNumFrames);

    receiver_sock.close();
    sender_sock.close();

    // The whole point of OPEN_STREAMING + per-block independence is that
    // dropped packets cause glitches, not a stall — so we expect to have
    // decoded a majority of the frames despite ~14% simulated packet loss,
    // and critically, the process must reach here at all (no crash/hang).
    if (decoded_frames < original.size() / 2) {
        std::printf("FAILED: decoded far too little audio given the loss rate.\n");
        return 1;
    }

    std::printf("PASS\n");
    return 0;
}
