#include "icecast_relay_client.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mpx {

bool IcecastRelayClient::connect(const IcecastConfig& cfg, int connect_timeout_ms) {
    close();
    chunked_ = false;
    chunk_state_ = ChunkState::ReadingSize;
    chunk_remaining_ = 0;
    chunk_line_partial_.clear();
    raw_buffer_.clear();
    raw_pos_ = 0;

    if (!socket_.connectTo(cfg.host, cfg.port, connect_timeout_ms)) {
        last_error_ = socket_.lastError();
        return false;
    }

    std::string mount = cfg.mount;
    if (mount.empty() || mount[0] != '/') mount = "/" + mount;

    std::string request;
    request += "GET " + mount + " HTTP/1.1\r\n";
    request += "Host: " + cfg.host + ":" + std::to_string(cfg.port) + "\r\n";
    request += "User-Agent: MPXpack-Receiver/1.0\r\n";
    request += "Icy-MetaData: 0\r\n";
    request += "Connection: keep-alive\r\n";
    request += "\r\n";

    if (!socket_.sendAll(request)) {
        last_error_ = socket_.lastError();
        close();
        return false;
    }

    // Parse the status line + headers, one line at a time, using the same
    // byte-level reader the chunked body parser uses — any body bytes that
    // arrive bundled with the header read stay in raw_buffer_ for the first
    // readBody() call.
    std::string line;
    bool first_line = true;
    int status_code = 0;

    for (;;) {
        uint8_t b;
        int r = nextByte(b, connect_timeout_ms);
        if (r <= 0) {
            last_error_ = (r == 0) ? "connection closed while reading response headers"
                                    : "timed out reading response headers";
            close();
            return false;
        }
        if (b == '\r') continue;
        if (b != '\n') {
            line += static_cast<char>(b);
            continue;
        }

        if (line.empty()) break; // blank line: end of headers

        if (first_line) {
            first_line = false;
            // Tolerate both "HTTP/1.1 200 OK" and ICY-style status lines.
            if (sscanf(line.c_str(), "HTTP/%*d.%*d %d", &status_code) != 1) {
                sscanf(line.c_str(), "ICY %d", &status_code);
            }
            if (status_code != 0 && status_code >= 400) {
                last_error_ = "Icecast GET rejected: " + line;
                close();
                return false;
            }
        } else {
            std::string lower = line;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower.find("transfer-encoding:") == 0 && lower.find("chunked") != std::string::npos) {
                chunked_ = true;
            }
        }
        line.clear();
    }

    return true;
}

int IcecastRelayClient::nextByte(uint8_t& out, int timeout_ms) {
    if (raw_pos_ >= raw_buffer_.size()) {
        raw_buffer_.clear();
        raw_pos_ = 0;
        uint8_t tmp[4096];
        int n = socket_.recvSome(tmp, sizeof(tmp), timeout_ms);
        if (n == 0) return 0;
        if (n < 0) return -1;
        raw_buffer_.assign(tmp, tmp + n);
    }
    out = raw_buffer_[raw_pos_++];
    return 1;
}

int IcecastRelayClient::readRaw(uint8_t* buf, size_t len, int timeout_ms) {
    if (raw_pos_ < raw_buffer_.size()) {
        size_t avail = raw_buffer_.size() - raw_pos_;
        size_t n = std::min(avail, len);
        std::memcpy(buf, raw_buffer_.data() + raw_pos_, n);
        raw_pos_ += n;
        if (raw_pos_ >= raw_buffer_.size()) {
            raw_buffer_.clear();
            raw_pos_ = 0;
        }
        return static_cast<int>(n);
    }

    int n = socket_.recvSome(buf, len, timeout_ms);
    if (n == 0) {
        last_error_ = "connection closed by server";
        return 0;
    }
    return n; // may be -1 (timeout/error) or a positive byte count
}

int IcecastRelayClient::readChunked(uint8_t* buf, size_t len, int timeout_ms) {
    size_t written = 0;

    while (written < len) {
        if (chunk_state_ == ChunkState::Done) {
            break;
        }

        if (chunk_state_ == ChunkState::ReadingSize) {
            uint8_t b;
            int r = nextByte(b, timeout_ms);
            if (r == 0) { chunk_state_ = ChunkState::Done; break; }
            if (r < 0) break; // timeout: return whatever was already written

            if (b == '\r') continue;
            if (b == '\n') {
                size_t semi = chunk_line_partial_.find(';');
                std::string size_str = (semi == std::string::npos)
                    ? chunk_line_partial_ : chunk_line_partial_.substr(0, semi);
                chunk_line_partial_.clear();
                unsigned long sz = strtoul(size_str.c_str(), nullptr, 16);
                chunk_remaining_ = sz;
                chunk_state_ = (sz == 0) ? ChunkState::Done : ChunkState::ReadingData;
                continue;
            }
            chunk_line_partial_ += static_cast<char>(b);
            continue;
        }

        if (chunk_state_ == ChunkState::ReadingData) {
            size_t want = std::min(chunk_remaining_, len - written);
            int n = readRaw(buf + written, want, timeout_ms);
            if (n == 0) { chunk_state_ = ChunkState::Done; break; }
            if (n < 0) break;
            written += static_cast<size_t>(n);
            chunk_remaining_ -= static_cast<size_t>(n);
            if (chunk_remaining_ == 0) chunk_state_ = ChunkState::ReadingDataTrailerCRLF;
            continue;
        }

        if (chunk_state_ == ChunkState::ReadingDataTrailerCRLF) {
            uint8_t b;
            int r = nextByte(b, timeout_ms);
            if (r == 0) { chunk_state_ = ChunkState::Done; break; }
            if (r < 0) break;
            if (b == '\n') chunk_state_ = ChunkState::ReadingSize;
            continue;
        }
    }

    return static_cast<int>(written);
}

int IcecastRelayClient::readBody(uint8_t* buf, size_t len, int timeout_ms) {
    if (chunked_) return readChunked(buf, len, timeout_ms);
    return readRaw(buf, len, timeout_ms);
}

void IcecastRelayClient::close() {
    socket_.close();
}

} // namespace mpx
