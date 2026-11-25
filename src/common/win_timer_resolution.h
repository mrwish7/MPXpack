#pragma once

namespace mpx {

// RAII wrapper requesting the OS's finest available system timer
// resolution (1ms) for the life of the process. No-op on non-Windows.
//
// Windows' default multimedia timer resolution is commonly ~15.6ms unless
// some other app on the system has already raised it — not something this
// app can rely on. That default sits uncomfortably close to this project's
// block cadence at some encode rates (see app_config.h): at the 128kHz
// default, one WavPack block is 12ms of audio (1536 samples / 128000 Hz),
// close enough to ~15.6ms that condition-variable timed waits in the
// decode/encode threads (byte_ring_buffer.cpp) can get coalesced or
// delayed inconsistently — producing intermittent stutter that doesn't
// show up at 144kHz (10.67ms/block) or 192kHz (8ms/block, no resampling
// needed at all), and doesn't show up on Linux, which doesn't share this
// particular default-timer-granularity quirk.
//
// Construct one instance near the start of main(), for the whole process
// lifetime.
class WinTimerResolution {
public:
    WinTimerResolution();
    ~WinTimerResolution();
    WinTimerResolution(const WinTimerResolution&) = delete;
    WinTimerResolution& operator=(const WinTimerResolution&) = delete;

private:
    bool active_ = false;
};

} // namespace mpx
