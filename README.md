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
```

Output: `build/<config>/bin/libsoloud.a`.

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
