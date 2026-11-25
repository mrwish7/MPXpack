#pragma once

#include <cstdint>
#include <string>

namespace mpx {

// The soundcard itself is always operated at this rate — both capture and
// playback. kSampleRate is kept as the name used throughout the audio
// device / GUI validation code (see audio_device.h).
constexpr int kSampleRate = 192000;
constexpr int kChannels = 1;
constexpr int kBitsPerSampleSource = 16;

// The *encoded/transmitted* sample rate is separately selectable on the
// sender: FM MPX with RDS only needs content up to ~62kHz (RDS centered at
// 57kHz), so 128kHz (Nyquist 64kHz, ~2kHz anti-alias filter margin above
// the RDS content edge) cuts bitrate by 33% relative to passing the full
// 192kHz through unchanged. That margin is tight — verified empirically
// against libsamplerate's SRC_SINC_BEST_QUALITY filter using a real
// captured MPX+RDS recording (tests/probe_real_mpx_resample.cpp): zero
// clipped samples and no measurable RDS-band or pilot-tone attenuation
// down to 128kHz on that file. A different capture with content closer to
// the Nyquist edge could behave differently — re-run that probe against a
// suspect recording before trusting this rate on it. 192kHz remains
// selectable for signals carrying additional subcarriers above the RDS
// band (e.g. DARC, RDS2 groups) that 128kHz would filter out. The receiver
// auto-detects whichever rate the incoming stream actually uses (via
// WavpackDecoder::sampleRate()) and resamples back up to kSampleRate for
// playback only if needed — it has no separate rate selector of its own.
constexpr int kEncodeSampleRateLow = 128000;
constexpr int kEncodeSampleRateMid = 144000;  // intermediate option — more filter/prediction
                                               // margin than 128kHz, less bitrate saved than it
constexpr int kEncodeSampleRateHigh = kSampleRate;
constexpr int kEncodeSampleRateDefault = kEncodeSampleRateLow;

// Hybrid bits-per-sample range exposed in the GUI (see kBlockSamples below
// and tests/test_block_sizing.cpp for how block size relates to it). The GUI
// snaps to 0.1 steps within this range (see sender_gui.cpp) — the range
// itself has no such granularity requirement.
constexpr float kBpsMin = 2.2f;
constexpr float kBpsMax = 8.0f;
constexpr float kBpsDefault = 4.0f;

// 192000 / 1536 = 125 blocks/sec exactly (no drift), ~8ms/block latency.
// At the top of the hybrid bps range (or in lossless mode, whose block
// size can approach raw-PCM-equivalent for highly incompressible content —
// see SenderConfig::lossless) a block exceeds the ~1200 byte UDP payload
// budget; UdpFraming's fragmentation (a real, exercised path, not a
// theoretical fallback — see tests/test_block_sizing.cpp) handles that.
constexpr int32_t kBlockSamples = 1536;

constexpr int kDefaultMaxLatencyMs = 200;

enum class TransportMode {
    Icecast,
    Udp,
};

// Which channel of the capture device actually carries the MPX signal.
// "Both" is the existing default: the device's native channel count is
// mixed down to mono by miniaudio's own channel converter (a plain
// sum/average — fine for a genuine mono source, or a stereo source where
// both channels carry the *same* signal). Left/Right instead capture the
// device in real stereo and discard the other channel entirely — needed
// when L and R carry two unrelated services (e.g. two different MPX feeds
// on a line-in device), where averaging them would garble both.
enum class ChannelSelect {
    Both,
    Left,
    Right,
};

struct IcecastConfig {
    std::string host;
    int port = 8000;
    std::string mount;      // e.g. "/mpx.wv"
    std::string password;
    std::string user = "source";
};

struct UdpEndpointConfig {
    std::string address;    // destination (sender) or bind (receiver) address
    int port = 0;
};

struct SenderConfig {
    std::string input_device_id;
    float bits_per_sample = kBpsDefault; // ignored when lossless == true
    int encode_sample_rate = kEncodeSampleRateDefault; // kEncodeSampleRateLow/Mid/High
    ChannelSelect channel_select = ChannelSelect::Both;
    // Plain WavPack lossless compression instead of hybrid/lossy — no
    // bits_per_sample target, bitrate varies with content. Intended for
    // LAN-style links where bandwidth isn't the constraint. All three
    // encode_sample_rate options remain meaningful in lossless mode too
    // (rate still trades content bandwidth for lower CPU/bitrate; it's
    // orthogonal to lossless vs hybrid).
    bool lossless = false;
    TransportMode transport = TransportMode::Udp;
    IcecastConfig icecast;
    UdpEndpointConfig udp;
    int max_latency_ms = kDefaultMaxLatencyMs;
};

struct ReceiverConfig {
    std::string output_device_id;
    TransportMode transport = TransportMode::Udp;
    IcecastConfig icecast;      // host/port/mount used to build the GET URL
    UdpEndpointConfig udp;      // bind address/port to listen on
    int max_latency_ms = kDefaultMaxLatencyMs;
    // Which channel of a stereo output device gets the (mono) decoded MPX
    // signal. "Both" is the existing default: the device is opened mono and
    // miniaudio's own channel converter duplicates it to L+R. Left/Right
    // instead open the device in real stereo and inject the signal into
    // just the one channel, with the other left silent — the output-side
    // mirror of SenderConfig::channel_select, for feeding two independent
    // downstream services (or just routing to a labeled single channel).
    ChannelSelect channel_select = ChannelSelect::Both;
};

inline int BpsToKbps(float bps, int sample_rate) {
    return static_cast<int>(bps * sample_rate * kChannels / 1000.0f + 0.5f);
}

} // namespace mpx
