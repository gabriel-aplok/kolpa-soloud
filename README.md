# kolpa-soloud

SoLoud fork used by [Kolpa Engine](https://github.com/gabriel-aplok/kolpa-engine).
Trimmed to the C++ core and built with the kine build system.

## What changed from upstream

- Only C++ library code remains. The C API, glue bindings, demos, docs, tools, and
  packaged scripts are removed.
- Audio backends: miniaudio (default) and null (headless tests). SDL, OpenAL,
  PortAudio, WASAPI, WinMM, XAudio2, ALSA, JACK, OSS, CoreAudio, OpenSL ES, and
  Vita backends are removed.
- OpenMPT is removed. It needs libmodplug and a C DLL loader.
- Headers moved beside their sources under `src/` and renamed to `.hpp`.

Everything else is upstream SoLoud: core engine, buses, queues, 3D audio, 12
filters, and the WAV, WAV stream, noise, sfxr, speech, monotone, VIC, TED-SID,
AY, and Viznut audio sources.

## Build

Kine reads `project.json` from the repository root:

```
..\kolpa-engine\build\Kine.Build.exe build
..\kolpa-engine\build\Kine.Build.exe --target smoke test
```

The static library lands in `build/<config>/bin/libsoloud.a`.

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

On Windows miniaudio loads system audio APIs at runtime, so no extra link
libraries are needed. On Linux link `-lm -ldl -lpthread`.

The library does not compile `MINIAUDIO_IMPLEMENTATION`. The consumer owns it,
so a game binary never links two implementations. `tests/miniaudio_impl.cpp`
provides it for the fork smoke test only. The header version must match the
consumer's miniaudio header.

The null backend needs no device and is meant for tests:

```cpp
soloud.init(SoLoud::Soloud::CLIP_ROUNDOFF, SoLoud::Soloud::NULLDRIVER, 44100, 2048, 2);
```

## License

zlib/libpng, same as upstream SoLoud. Original copyright and license notices
stay in every file. Upstream project: https://soloud-audio.com

This fork is an altered version of SoLoud, as required by the license terms.
