# MPXpack

Sender/receiver pair for streaming a 192kHz mono FM MPX composite signal (with
RDS) over the internet, using WavPack hybrid lossy compression to cut the
raw ~3072kbps PCM down to a configurable, much lower bitrate. Transport is
either a direct UDP stream or an Icecast mount. See `docs/phase2-plan.md` for
the full design rationale.

Two executables share one static library (`mpx_common`):

- **`mpx_sender`** — captures from a sound device, resamples (optionally,
  see below), encodes, and transmits.
- **`mpx_receiver`** — receives, decodes, resamples back up if needed, and
  plays out to a sound device.

## Project layout

```
CMakeLists.txt          Top-level superproject
cmake/FetchDeps.cmake   Fetches GLFW, Dear ImGui, miniaudio, libsamplerate
                        (WavPack is fetched, pinned to a commit, in CMakeLists.txt)
src/common/             Shared code: WavPack wrappers, resampler, audio I/O,
                        UDP/Icecast networking, config persistence
src/sender/             mpx_sender GUI app
src/receiver/           mpx_receiver GUI app
tests/                  Test suite + standalone diagnostic tools (see below)
```

## Prerequisites

### Windows

- Visual Studio 2019+ Build Tools (or full Visual Studio) with the **Desktop
  development with C++** workload. CMake and Ninja bundled with VS work
  fine — you don't need a separate CMake install:
  `<VS install path>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`
- Git (for CMake's `FetchContent` to pull GLFW/Dear ImGui/miniaudio/libsamplerate)

### Linux

- A C/C++ toolchain, CMake ≥ 3.20, and Git:
  ```bash
  sudo apt-get install build-essential cmake git
  ```
- X11 + OpenGL development headers, for GLFW (build-time only):
  ```bash
  sudo apt-get install libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev
  ```
- **No ALSA development package is required.** miniaudio's ALSA backend
  loads `libasound.so.2` dynamically at runtime via `dlopen`, so nothing
  beyond the ALSA runtime library (`libasound2`, present on virtually every
  desktop/audio-capable Linux system already) is needed to build or run.

## Building

### Windows

From a regular terminal (the CMake Visual Studio generator finds MSVC on
its own — no need to manually enter a "Developer Command Prompt"):

```powershell
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

& $cmake -G "Visual Studio 16 2019" -A x64 -B build
& $cmake --build build --config RelWithDebInfo
```

(Adjust the generator name/version and the path to `cmake.exe` to match
your actual Visual Studio install.)

Executables land at:
```
build\src\sender\RelWithDebInfo\mpx_sender.exe
build\src\receiver\RelWithDebInfo\mpx_receiver.exe
```

They're statically linked against the MSVC runtime (`CMAKE_MSVC_RUNTIME_LIBRARY`
is set in the top-level `CMakeLists.txt`), so the built `.exe` files are
fully self-contained — no Visual C++ Redistributable needed to run them on
another Windows machine.

To build a single target (faster while iterating), e.g. just the receiver:
```powershell
& $cmake --build build --config RelWithDebInfo --target mpx_receiver
```

### Linux

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)
```

Executables land at `build/src/sender/mpx_sender` and
`build/src/receiver/mpx_receiver`.

GLFW is built X11-only (`GLFW_BUILD_WAYLAND` is off in
`cmake/FetchDeps.cmake`) to avoid needing `wayland-scanner`/
`wayland-protocols` on the build machine — X11 apps still run fine on
Wayland desktops via XWayland, so this doesn't limit where the built
binaries can run.

## Running

Both apps look for a settings file next to the executable
(`sender_config.ini` / `receiver_config.ini`) on startup, and save to it
automatically. Pass `--config <path>` to point at a different file — useful
for running multiple instances side by side, each with its own saved
profile:

```
mpx_sender.exe --config sender-backup-link.ini
```

The receiver has an **Auto-start on launch** checkbox (on by default). When
a saved config file exists, it starts receiving with those settings as soon
as the app opens, so an unattended machine with the app set to open at
login resumes after a reboot. If the saved output device isn't available
yet, it retries every few seconds rather than falling back to a different
device. It never auto-starts on a first run with no config file.

### ALSA device selection (Linux)

miniaudio's ALSA backend is configured with `useVerboseDeviceEnumeration`
enabled (see `src/common/audio_device.cpp`), so the device dropdown lists
every registered ALSA PCM name separately rather than collapsing them —
this is what lets you pick `hw:CARD=Loopback,DEV=0` (direct hardware
access, no ALSA-side resampling) versus `plughw:CARD=Loopback,DEV=0`
(goes through ALSA's software "plug" conversion layer) as distinct,
explicit choices.

To actually have `hw:Loopback`/`plughw:Loopback` devices available, the
kernel's `snd-aloop` module needs to be loaded:
```bash
sudo modprobe snd-aloop
```
Note: this module is **not present** in Microsoft's stock WSL2 kernel —
WSL is fine for compiling and running the non-audio-hardware test suite,
but real loopback-device testing needs an actual Linux machine (or a
custom WSL2 kernel build).

## Testing

Core pass/fail test suite (all safe to run with no audio hardware, no
network, no display):

```
test_block_sizing              # validates block_samples sizing across the bps range
test_encode_decode_roundtrip   # WavPack encode/decode fidelity at both encode rates
test_loopback_udp              # UDP framing/reassembly under simulated packet loss
test_resampler_passband        # confirms 192k<->144k resampling preserves the RDS band
test_decoder_late_start        # regression test for the receiver's decoder-retry logic
```

Run them directly, or via CTest:
```
ctest --test-dir build -C RelWithDebInfo   # Windows
ctest --test-dir build                     # Linux
```

Diagnostic tools (not pass/fail — print measurements for you to read):

```
probe_audio_devices    # lists every audio device and its native formats
test_rds_snr            # measures RDS SNR after lossy compression at each bps/rate
test_quality_flags      # measures CPU cost + RDS SNR effect of WavPack's optional
                        # quality flags (high/very-high, extra mode, noise shaping)
test_icecast_live       # exercises the Icecast PUT/GET path against a REAL server —
                        # needs args: <host> <port> <mount> <user> <password> [duration_sec]
```

## Third-party components

Fetched automatically by CMake at configure time (see `cmake/FetchDeps.cmake`
and the top-level `CMakeLists.txt`) — check each project's own repository
for license terms:

- [WavPack](https://github.com/dbry/WavPack) — hybrid lossy/lossless audio codec
- [GLFW](https://github.com/glfw/glfw) — windowing/OpenGL context
- [Dear ImGui](https://github.com/ocornut/imgui) — GUI
- [miniaudio](https://github.com/mackron/miniaudio) — cross-platform audio I/O
- [libsamplerate](https://github.com/libsndfile/libsamplerate) — sample rate conversion
