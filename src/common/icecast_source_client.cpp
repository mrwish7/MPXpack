#include "icecast_source_client.h"

#include <cstdio>

#include "base64.h"

namespace mpx {

bool IcecastSourceClient::connect(const IcecastConfig& cfg) {
    close();

    if (!socket_.connectTo(cfg.host, cfg.port)) {
        last_error_ = socket_.lastError();
        return false;
    }

    std::string mount = cfg.mount;
    if (mount.empty() || mount[0] != '/') mount = "/" + mount;

    std::string auth = Base64Encode(cfg.user + ":" + cfg.password);

    std::string request;
    request += "PUT " + mount + " HTTP/1.1\r\n";
    request += "Host: " + cfg.host + ":" + std::to_string(cfg.port) + "\r\n";
    request += "Authorization: Basic " + auth + "\r\n";
    request += "User-Agent: MPXpack-Sender/1.0\r\n";
    // application/octet-stream avoids Icecast's built-in codec-specific
    // stream sniffers (MP3/Ogg/WebM) misinterpreting our custom payload.
    request += "Content-Type: application/octet-stream\r\n";
    request += "Ice-Public: 0\r\n"; // not a real streaming codec; keep it out of YP directories
    request += "Expect: 100-continue\r\n";
    request += "\r\n";

    if (!socket_.sendAll(request)) {
        last_error_ = socket_.lastError();
        close();
        return false;
    }

    // Icecast's PUT source handler typically does not respond until the
    // stream ends or an error occurs, so absence of a reply within this
    // window is the expected success case, not a failure (verified in
    // testing against a deployed Icecast server — see
    // tests/test_icecast_live.cpp).
    uint8_t buf[512];
    int n = socket_.recvSome(buf, sizeof(buf) - 1, 1500);
    if (n > 0) {
        buf[n] = 0;
        std::string response(reinterpret_cast<char*>(buf));
        int code = 0;
        if (sscanf(response.c_str(), "HTTP/%*d.%*d %d", &code) == 1 && code >= 400) {
            last_error_ = "Icecast rejected source connection: " + response.substr(0, response.find("\r\n"));
            close();
            return false;
        }
    }

    return true;
}

bool IcecastSourceClient::sendBlock(const uint8_t* data, size_t len) {
    if (!socket_.sendAll(data, len)) {
        last_error_ = socket_.lastError();
        return false;
    }
    return true;
}

void IcecastSourceClient::close() {
    socket_.close();
}

} // namespace mpx
