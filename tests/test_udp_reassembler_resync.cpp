// Verifies UdpReassembler recovers when a sender restarts mid-session.
// SenderApp::start() resets its seq counter to 0 on every restart
// (sender_app.cpp), which — without the resync logic in udp_framing.cpp —
// would make every post-restart packet look like an ancient duplicate
// relative to whatever next_expected_seq_ had climbed to, and get silently
// dropped forever with no way to self-heal. See UdpReassembler::
// kResyncThreshold in udp_framing.h for the detection approach.

#include <cstdio>
#include <vector>

#include "byte_ring_buffer.h"
#include "udp_framing.h"

using namespace mpx;

namespace {
std::vector<uint8_t> MakeBlock(size_t n, uint8_t fill) {
    return std::vector<uint8_t>(n, fill);
}
} // namespace

int main() {
    ByteRingBuffer ring(1 << 20);
    UdpReassembler reassembler;
    bool all_ok = true;

    // "Session A": a normal run of 1000 in-order blocks, pushing
    // next_expected_seq_ well past zero.
    const uint32_t kSessionABlocks = 1000;
    for (uint32_t seq = 0; seq < kSessionABlocks; ++seq) {
        auto block = MakeBlock(16, static_cast<uint8_t>(seq));
        for (auto& pkt : FragmentBlock(block.data(), block.size(), seq)) {
            reassembler.onPacketReceived(pkt.data(), pkt.size(), ring);
        }
    }

    size_t bytes_after_a = ring.availableBytes();
    if (bytes_after_a != static_cast<size_t>(kSessionABlocks) * 16) {
        std::printf("FAILED: expected %zu bytes after session A, got %zu\n",
                    static_cast<size_t>(kSessionABlocks) * 16, bytes_after_a);
        all_ok = false;
    }
    std::vector<uint8_t> drain(bytes_after_a);
    ring.popBlocking(drain.data(), drain.size(), 0);

    // "Session B": sender restarted, seq resets to 0 — every one of these
    // is behind next_expected_seq_=1000 and looks like a stale duplicate.
    uint64_t resyncs_before = reassembler.resyncCount();
    const uint32_t kSessionBBlocks = 60;
    for (uint32_t seq = 0; seq < kSessionBBlocks; ++seq) {
        auto block = MakeBlock(16, static_cast<uint8_t>(0x80 + seq));
        for (auto& pkt : FragmentBlock(block.data(), block.size(), seq)) {
            reassembler.onPacketReceived(pkt.data(), pkt.size(), ring);
        }
    }
    uint64_t resyncs_after = reassembler.resyncCount();

    std::printf("resyncs: %llu -> %llu\n",
                static_cast<unsigned long long>(resyncs_before), static_cast<unsigned long long>(resyncs_after));
    if (resyncs_after != resyncs_before + 1) {
        std::printf("FAILED: expected exactly 1 resync during session B, got %llu\n",
                     static_cast<unsigned long long>(resyncs_after - resyncs_before));
        all_ok = false;
    }

    size_t bytes_after_b = ring.availableBytes();
    std::printf("recovered %zu bytes from session B (sent %zu)\n", bytes_after_b,
                static_cast<size_t>(kSessionBBlocks) * 16);

    // The whole point of the fix: session B must NOT be dropped forever
    // (the pre-fix behavior would leave this at exactly 0).
    if (bytes_after_b == 0) {
        std::printf("FAILED: session B was dropped entirely — resync did not recover playback\n");
        all_ok = false;
    }
    if (bytes_after_b > static_cast<size_t>(kSessionBBlocks) * 16) {
        std::printf("FAILED: recovered more bytes than session B ever sent\n");
        all_ok = false;
    }

    if (!all_ok) {
        std::printf("FAILED\n");
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
