#include "udp_framing.h"

#include <algorithm>
#include <cstring>

#include "byte_ring_buffer.h"

namespace mpx {

std::vector<std::vector<uint8_t>> FragmentBlock(const uint8_t* data, size_t len, uint32_t seq) {
    std::vector<std::vector<uint8_t>> packets;

    size_t fragment_cnt = (len + kMaxUdpFragmentPayload - 1) / kMaxUdpFragmentPayload;
    if (fragment_cnt == 0) fragment_cnt = 1; // a zero-length block still gets one (empty) packet
    if (fragment_cnt > 0xFFFF) fragment_cnt = 0xFFFF; // pathological; header field is 16-bit

    size_t offset = 0;
    for (size_t i = 0; i < fragment_cnt; ++i) {
        size_t chunk = std::min(kMaxUdpFragmentPayload, len - offset);

        UdpPacketHeader hdr{};
        hdr.magic = kUdpMagic;
        hdr.seq = seq;
        hdr.fragment_idx = static_cast<uint16_t>(i);
        hdr.fragment_cnt = static_cast<uint16_t>(fragment_cnt);

        std::vector<uint8_t> packet(sizeof(hdr) + chunk);
        std::memcpy(packet.data(), &hdr, sizeof(hdr));
        if (chunk > 0) {
            std::memcpy(packet.data() + sizeof(hdr), data + offset, chunk);
        }
        packets.push_back(std::move(packet));
        offset += chunk;
    }

    return packets;
}

UdpReassembler::UdpReassembler(size_t window) : window_(window) {}

void UdpReassembler::onPacketReceived(const uint8_t* packet, size_t len, ByteRingBuffer& out_ring) {
    if (len < sizeof(UdpPacketHeader)) return;

    UdpPacketHeader hdr{};
    std::memcpy(&hdr, packet, sizeof(hdr));
    if (hdr.magic != kUdpMagic) return; // stray traffic on this port
    if (hdr.fragment_cnt == 0 || hdr.fragment_idx >= hdr.fragment_cnt) return;

    ++received_packets_;

    if (!initialized_) {
        next_expected_seq_ = hdr.seq;
        initialized_ = true;
    }

    // Too old to matter (already delivered or already given up on) — unless
    // this is the kResyncThreshold-th one in a row, in which case treat it
    // as a resync instead (see the comment on kResyncThreshold).
    int32_t age = static_cast<int32_t>(hdr.seq - next_expected_seq_);
    if (age < 0) {
        if (++consecutive_stale_ < kResyncThreshold) return;

        pending_.clear();
        next_expected_seq_ = hdr.seq;
        ++resync_count_;
        consecutive_stale_ = 0;
        age = 0;
    } else {
        consecutive_stale_ = 0;
    }

    // Too far ahead for our window: give up on everything before it rather
    // than growing the pending map unboundedly.
    if (static_cast<size_t>(age) >= window_) {
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->first < hdr.seq - static_cast<uint32_t>(window_) + 1) {
                ++dropped_blocks_;
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
        next_expected_seq_ = hdr.seq - static_cast<uint32_t>(window_) + 1;
    }

    PartialBlock& pb = pending_[hdr.seq];
    if (pb.fragments.empty()) {
        pb.fragments.resize(hdr.fragment_cnt);
        pb.got.assign(hdr.fragment_cnt, false);
        pb.fragment_cnt = hdr.fragment_cnt;
    }
    if (!pb.got[hdr.fragment_idx]) {
        pb.got[hdr.fragment_idx] = true;
        ++pb.received;
    }
    pb.fragments[hdr.fragment_idx].assign(packet + sizeof(hdr), packet + len);

    drainReady(out_ring);
}

void UdpReassembler::drainReady(ByteRingBuffer& out_ring) {
    for (;;) {
        auto it = pending_.find(next_expected_seq_);
        if (it == pending_.end()) break;
        if (!it->second.complete()) break;

        for (const auto& frag : it->second.fragments) {
            if (!frag.empty()) out_ring.push(frag.data(), frag.size());
        }
        pending_.erase(it);
        ++next_expected_seq_;
    }
}

} // namespace mpx
