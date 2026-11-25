#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace mpx {

class ByteRingBuffer;

#pragma pack(push, 1)
struct UdpPacketHeader {
    uint32_t magic;         // sanity tag so a listener ignores stray traffic on the port
    uint32_t seq;           // increases by 1 per WavPack block (shared across its fragments)
    uint16_t fragment_idx;  // 0-based index of this fragment within the block
    uint16_t fragment_cnt;  // total fragments the block was split into (1 in the common case)
};
#pragma pack(pop)

constexpr uint32_t kUdpMagic = 0x5850424Du; // 'MPBX' as a fixed sanity tag
constexpr size_t kMaxUdpPacketBytes = 1200;  // total datagram size budget, header included
constexpr size_t kMaxUdpFragmentPayload = kMaxUdpPacketBytes - sizeof(UdpPacketHeader);

// Splits one encoded WavPack block into one or more ready-to-send UDP
// datagrams (header + payload each). In the common case (see
// kBlockSamples in app_config.h) this returns a single packet;
// fragmentation is a real, exercised path for the rare oversized block.
std::vector<std::vector<uint8_t>> FragmentBlock(const uint8_t* data, size_t len, uint32_t seq);

// Reassembles fragments back into complete blocks and hands them to a
// ByteRingBuffer strictly in sequence order, holding a small look-ahead
// window for reordered packets. A block that never completes within the
// window is simply skipped (dropped) — this trades a brief audio glitch for
// bounded latency.
class UdpReassembler {
public:
    explicit UdpReassembler(size_t window = 4);

    // `packet` is one full received UDP payload starting at UdpPacketHeader.
    void onPacketReceived(const uint8_t* packet, size_t len, ByteRingBuffer& out_ring);

    uint64_t droppedBlockCount() const { return dropped_blocks_; }
    uint64_t receivedPacketCount() const { return received_packets_; }
    // Counts how many times onPacketReceived() detected a sustained run of
    // "too old" packets and resynced instead of dropping forever — see the
    // comment at kResyncThreshold. Callers can poll this to notice a
    // sender restart (which resets its seq counter to 0 — SenderApp::start)
    // even though packets never technically stopped arriving.
    uint64_t resyncCount() const { return resync_count_; }

private:
    struct PartialBlock {
        std::vector<std::vector<uint8_t>> fragments;
        std::vector<bool> got;
        uint16_t fragment_cnt = 0;
        uint16_t received = 0;
        bool complete() const { return fragment_cnt != 0 && received == fragment_cnt; }
    };

    void drainReady(ByteRingBuffer& out_ring);

    // A restarted sender's fresh seq numbering (starting back at 0, far
    // "behind" whatever next_expected_seq_ climbed to before) looks
    // identical to an ordinary too-old/duplicate packet by the age<0 check
    // alone, and would otherwise be dropped forever with no way to
    // self-heal. Genuine reordering only ever rejects a handful of packets
    // before drainReady() lets next_expected_seq_ catch up again, so a long
    // unbroken run of rejections is instead treated as "our expectation is
    // simply stale" and triggers a resync.
    static constexpr uint32_t kResyncThreshold = 50;

    std::map<uint32_t, PartialBlock> pending_;
    uint32_t next_expected_seq_ = 0;
    bool initialized_ = false;
    size_t window_;
    uint64_t dropped_blocks_ = 0;
    uint64_t received_packets_ = 0;
    uint32_t consecutive_stale_ = 0;
    uint64_t resync_count_ = 0;
};

} // namespace mpx
