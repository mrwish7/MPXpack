#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "app_config.h"
#include "tcp_socket.h"

namespace mpx {

// Minimal hand-rolled Icecast HTTP GET client: connects, sends the GET
// request, parses the response status line/headers, then treats the rest of
// the connection as a raw (or chunked, if the server says so) byte stream.
// We deliberately do not try to align on WavPack block boundaries here — a
// fresh GET response can start mid-block, and WavpackDecoder's internal
// block-sync scan (see docs/phase2-plan.md 2.6) is relied on to resync.
class IcecastRelayClient {
public:
    bool connect(const IcecastConfig& cfg, int connect_timeout_ms = 5000);

    // Returns bytes written to buf (0 = connection ended, needs reconnect),
    // or -1 on a genuine error/timeout.
    int readBody(uint8_t* buf, size_t len, int timeout_ms);

    void close();
    bool isOpen() const { return socket_.isOpen(); }
    const std::string& lastError() const { return last_error_; }

private:
    enum class ChunkState { ReadingSize, ReadingData, ReadingDataTrailerCRLF, Done };

    // Returns 1 = got a byte, 0 = connection closed, -1 = timeout/error.
    int nextByte(uint8_t& out, int timeout_ms);
    // Drains any leftover raw_buffer_ bytes first, then reads directly from
    // the socket. Used both for the plain (non-chunked) body path and as the
    // bulk-copy primitive inside readChunked()'s ReadingData state.
    int readRaw(uint8_t* buf, size_t len, int timeout_ms);
    int readChunked(uint8_t* buf, size_t len, int timeout_ms);

    TcpSocket socket_;
    std::string last_error_;
    bool chunked_ = false;

    // Small internal buffer so header parsing / chunk-size-line parsing can
    // work across recv() calls that split mid-line, and so any body bytes
    // read past the header boundary during header parsing aren't lost.
    std::vector<uint8_t> raw_buffer_;
    size_t raw_pos_ = 0;

    ChunkState chunk_state_ = ChunkState::ReadingSize;
    size_t chunk_remaining_ = 0;
    std::string chunk_line_partial_;
};

} // namespace mpx
