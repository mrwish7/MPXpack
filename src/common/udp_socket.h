#pragma once

#include <cstdint>
#include <string>

#include "net_common.h"

namespace mpx {

// Thin cross-platform UDP socket wrapper covering exactly what the sender
// (connect+send) and receiver (bind+recv) need.
class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // Sender side: fixes the destination for subsequent send().
    bool openForSend(const std::string& dest_address, int dest_port);

    // Receiver side: binds to listen_address (e.g. "0.0.0.0") : listen_port.
    bool openForReceive(const std::string& listen_address, int listen_port);

    bool send(const uint8_t* data, size_t len);

    // Blocks up to timeout_ms for a datagram. Returns the number of bytes
    // received (0 on timeout), or -1 on a real socket error.
    int receive(uint8_t* buffer, size_t buffer_len, int timeout_ms);

    void close();
    bool isOpen() const { return sock_ != kInvalidSocket; }
    const std::string& lastError() const { return last_error_; }

private:
    socket_t sock_ = kInvalidSocket;
    std::string last_error_;
};

} // namespace mpx
