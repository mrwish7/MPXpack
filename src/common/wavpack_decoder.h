#pragma once

#include <cstdint>
#include <string>

#include "wavpack.h" // WavpackStreamReader64

struct WavpackContext;

namespace mpx {

class ByteRingBuffer;

// Wraps WavPack's OPEN_STREAMING decode mode (WavpackOpenFileInputEx64) to
// decode a continuous, headerless, non-seekable live feed of WavPack blocks
// pulled from a ByteRingBuffer that a network thread is filling. Corrupt or
// incomplete blocks are handled internally by the library — decode()
// simply returns fewer samples than
// requested when the source stalls; callers should treat that as "try
// again", not a fatal error.
class WavpackDecoder {
public:
    WavpackDecoder() = default;
    ~WavpackDecoder();
    WavpackDecoder(const WavpackDecoder&) = delete;
    WavpackDecoder& operator=(const WavpackDecoder&) = delete;

    // `source` must outlive this decoder. Not owned.
    bool open(ByteRingBuffer* source, int read_timeout_ms = 500);

    // Decodes up to max_samples composite samples (frames) into `out`
    // (interleaved int32_t, per WavPack's API). Returns the number actually
    // decoded; 0 can mean either a transient source stall or end-of-stream —
    // callers loop and check a stop flag rather than treating 0 as fatal.
    uint32_t decode(int32_t* out, uint32_t max_samples);

    void close();

    int lastErrorCount() const;
    int sampleRate() const;
    int numChannels() const;
    const std::string& lastError() const { return last_error_; }

private:
    static int32_t readBytes(void* id, void* data, int32_t bcount);
    static int32_t writeBytes(void* id, void* data, int32_t bcount);
    static int64_t getPos(void* id);
    static int setPosAbs(void* id, int64_t pos);
    static int setPosRel(void* id, int64_t delta, int mode);
    static int pushBackByte(void* id, int c);
    static int64_t getLength(void* id);
    static int canSeek(void* id);
    static int truncateHere(void* id);
    static int closeStream(void* id);

    ByteRingBuffer* source_ = nullptr;
    WavpackContext* ctx_ = nullptr;
    WavpackStreamReader64 reader_{};
    int64_t pos_ = 0;
    int pushback_ = -1; // -1 = empty pushback slot
    int read_timeout_ms_ = 500;
    std::string last_error_;
};

} // namespace mpx
