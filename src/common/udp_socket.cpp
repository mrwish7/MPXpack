#include "udp_socket.h"

#include <cstring>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mpx {

UdpSocket::~UdpSocket() {
    close();
}

bool UdpSocket::openForSend(const std::string& dest_address, int dest_port) {
    close();
    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == kInvalidSocket) {
        last_error_ = LastSocketError();
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(dest_port));
    if (inet_pton(AF_INET, dest_address.c_str(), &addr.sin_addr) != 1) {
        last_error_ = "invalid destination address: " + dest_address;
        close();
        return false;
    }

    // UDP connect() just fixes the default peer for send()/recv(); no
    // handshake occurs.
    if (::connect(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        last_error_ = LastSocketError();
        close();
        return false;
    }
    return true;
}

bool UdpSocket::openForReceive(const std::string& listen_address, int listen_port) {
    close();
    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == kInvalidSocket) {
        last_error_ = LastSocketError();
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(listen_port));
    const std::string& bind_addr = listen_address.empty() ? std::string("0.0.0.0") : listen_address;
    if (inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
        last_error_ = "invalid listen address: " + bind_addr;
        close();
        return false;
    }

    if (::bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        last_error_ = LastSocketError();
        close();
        return false;
    }
    return true;
}

bool UdpSocket::send(const uint8_t* data, size_t len) {
    if (sock_ == kInvalidSocket) return false;
    auto sent = ::send(sock_, reinterpret_cast<const char*>(data), static_cast<int>(len), 0);
    if (sent < 0 || static_cast<size_t>(sent) != len) {
        last_error_ = LastSocketError();
        return false;
    }
    return true;
}

int UdpSocket::receive(uint8_t* buffer, size_t buffer_len, int timeout_ms) {
    if (sock_ == kInvalidSocket) return -1;
    SetSocketReceiveTimeout(sock_, timeout_ms);

    auto n = ::recv(sock_, reinterpret_cast<char*>(buffer), static_cast<int>(buffer_len), 0);
    if (n < 0) {
#if defined(_WIN32)
        if (WSAGetLastError() == WSAETIMEDOUT) return 0;
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
#endif
        last_error_ = LastSocketError();
        return -1;
    }
    return static_cast<int>(n);
}

void UdpSocket::close() {
    if (sock_ != kInvalidSocket) {
        CloseSocket(sock_);
        sock_ = kInvalidSocket;
    }
}

} // namespace mpx
