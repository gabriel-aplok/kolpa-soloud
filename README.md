# kolpa-soloud

SoLoud fork for [Kolpa Engine](https://github.com/gabriel-aplok/kolpa-engine).
C++ core only, built with kine.

## What changed from upstream

- The C API, bindings, demos, docs, tools, and packaged scripts are gone. Only
  the C++ library remains.
- Two backends: miniaudio for a real device, null for tests. SDL, OpenAL,
  PortAudio, WASAPI, WinMM, XAudio2, ALSA, JACK, OSS, CoreAudio, OpenSL ES, and
  the Vita backend are gone.
- OpenMPT is gone. It needs libmodplug and a C DLL loader.
- Headers moved next to their sources under `src/` and use the `.hpp`
  extension.

The rest is upstream SoLoud: core engine, buses, queues, 3D audio, 12 filters,
and the wav, wavstream, noise, sfxr, speech, monotone, vic, tedsid, ay, and
vizsn sources.

## Build

Kine reads `project.json` from the repository root:

```
..\kolpa-engine\build\Kine.Build.exe build
..\kolpa-engine\build\Kine.Build.exe --target smoke test
..\kolpa-engine\build\Kine.Build.exe --target lib release
..\kolpa-engine\build\Kine.Build.exe --target mix-benchmark release
build\release\bin\soloud_mix_benchmark.exe
```

Output: `build/<config>/bin/libsoloud.a`. The benchmark reports median mix time
per frame and its share of the 44.1 kHz real-time budget for mono and stereo
PCM, WAV streams, and per-voice or global biquad filters. Each case varies voice
count and block size. `--csv path` writes results for comparison. Pass
`--baseline path --max-regression-percent value` to fail when a case exceeds its
baseline by the chosen limit. Use a release build for timing comparisons.

## Use

Add `src` to the include paths and link `soloud`.

```cpp
#include "soloud.hpp"
#include "audiosource/wav/soloud_wav.hpp"

SoLoud::Soloud soloud;
soloud.init(SoLoud::Soloud::CLIP_ROUNDOFF, SoLoud::Soloud::MINIAUDIO, 44100, 2048, 2);

SoLoud::Wav wav;
wav.load("sound.wav");
soloud.play(wav);

soloud.deinit();
```

On Windows miniaudio loads system audio APIs at runtime, so no extra libraries
are needed. On Linux link `-lm -ldl -lpthread`.

Include `backend/miniaudio/soloud_miniaudio.hpp` to list playback devices and
select one before calling `init`:

```cpp
std::vector<SoLoud::MiniaudioDevice> devices;
if (SoLoud::getMiniaudioPlaybackDevices(devices) != SoLoud::SO_NO_ERROR || devices.empty()) return;
if (SoLoud::setPreferredMiniaudioDevice(&devices.front()) != SoLoud::SO_NO_ERROR) return;
if (SoLoud::setMiniaudioLatencyConfig(512, 3) != SoLoud::SO_NO_ERROR) return;
if (soloud.init(SoLoud::Soloud::CLIP_ROUNDOFF, SoLoud::Soloud::MINIAUDIO, 44100, 2048, 2) != SoLoud::SO_NO_ERROR) return;
```

Check each result. Device formats expose miniaudio's reported native formats.
`capabilitiesAvailable` is false when the active audio backend reports none.
Use `switchMiniaudioPlaybackDevice` to switch the running output without
reinitializing `Soloud`. `getMiniaudioLatencyInfo` returns the actual period,
period count, sample rate, and estimated buffered latency. Configure period
frames and count before initialization. On device loss, recovery retries the
selected device, then the default. `recoverMiniaudioPlaybackDevice` requests the
same recovery sequence. `getMiniaudioRecoveryStatus` reports outcomes, and
`setMiniaudioRecoveryCallback` reports status changes on the recovery thread.
Pass `nullptr` to `setPreferredMiniaudioDevice` to use the default device.

To check device enumeration and startup on a machine with an audio output:

```powershell
..\kolpa-engine\build\Kine.Build.exe --target miniaudio-device-smoke build
build\debug\bin\soloud_miniaudio_device_smoke.exe
```

This test repeats runtime switching and recovery, injects an unavailable
selected-device ID and verifies recovery opens the default device, checks
callbacks and latency, then shuts the worker down. `miniaudio-recovery-policy`
tests fallback order without hardware. Physical device-loss notification still
needs unplug testing.

The library builds the one `MINIAUDIO_IMPLEMENTATION`, version 0.11.25. Do not
build another one, and keep the consumer's miniaudio header on the same
version.

The null backend opens no device. It is for tests:

```cpp
soloud.init(SoLoud::Soloud::CLIP_ROUNDOFF, SoLoud::Soloud::NULLDRIVER, 44100, 2048, 2);
```

## License

zlib/libpng, same as upstream SoLoud. Original copyright and license notices
stay in every file. Upstream: https://soloud-audio.com

This fork is an altered version of SoLoud, as the license requires.
