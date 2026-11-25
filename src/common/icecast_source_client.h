#pragma once

#include <cstdint>
#include <string>

#include "app_config.h"
#include "tcp_socket.h"

namespace mpx {

// Minimal hand-rolled Icecast HTTP PUT source client: opens one persistent
// TCP connection, sends the PUT request/headers, then streams raw encoded
// bytes as the (unbounded, connection-close-delimited) request body. See
// docs/phase2-plan.md 2.5 for the Content-Type/chunking rationale.
class IcecastSourceClient {
public:
    bool connect(const IcecastConfig& cfg);
    bool sendBlock(const uint8_t* data, size_t len);
    void close();
    bool isOpen() const { return socket_.isOpen(); }
    const std::string& lastError() const { return last_error_; }

private:
    TcpSocket socket_;
    std::string last_error_;
};

} // namespace mpx
