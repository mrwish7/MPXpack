#include "win_timer_resolution.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

namespace mpx {

WinTimerResolution::WinTimerResolution() {
#if defined(_WIN32)
    active_ = (timeBeginPeriod(1) == TIMERR_NOERROR);
#endif
}

WinTimerResolution::~WinTimerResolution() {
#if defined(_WIN32)
    if (active_) timeEndPeriod(1);
#endif
}

} // namespace mpx
