#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace mpx {

// Bounded queue of discrete byte blocks (as opposed to ByteRingBuffer's flat
// stream) — used specifically for the sender's encoder->network handoff,
// where UDP fragmentation needs WavPack block boundaries preserved. Same
// drop-oldest-on-overflow bounded-latency policy as ByteRingBuffer (see
// byte_ring_buffer.h).
class BlockQueue {
public:
    explicit BlockQueue(size_t max_blocks) : max_blocks_(max_blocks) {}

    void push(std::vector<uint8_t> block) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(block));
            while (queue_.size() > max_blocks_) queue_.pop_front();
        }
        cv_.notify_all();
    }

    // Returns an empty vector on timeout or stop.
    std::vector<uint8_t> popBlocking(int timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        cv_.wait_until(lock, deadline, [this] { return stop_ || !queue_.empty(); });
        if (stop_ || queue_.empty()) return {};
        std::vector<uint8_t> block = std::move(queue_.front());
        queue_.pop_front();
        return block;
    }

    void requestStop() {
        { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; }
        cv_.notify_all();
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        stop_ = false;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::vector<uint8_t>> queue_;
    size_t max_blocks_;
    bool stop_ = false;
};

} // namespace mpx
