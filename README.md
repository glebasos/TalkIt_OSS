# TalkIt_OSS

A portable C reconstruction of the SoftVoice speech engine (`TIBASE32.DLL` plus
the `TIENG32.DLL` / `TISPAN32.DLL` language modules) shipped with Microsoft
Talk It!. It builds `libtispeech` (`.dylib` / `.so` / `.dll`) for macOS, Linux
and Windows, with no emulator and without running any Windows code.

It reconstructs the English and Spanish text front ends and the full
phoneme-to-PCM pipeline. Output is original-voice 8-bit mono PCM at 11025 Hz,
compared sample for sample against the original engine in both languages,
together with its word, syllable, phoneme and mouth-shape events. SoftVoice
user dictionaries (`SVXF`) are supported.

The .NET bindings live in [TiSpeech](https://github.com/glebasos/TiSpeech).

## You need the original DLLs

The rule tables and synthesis coefficients belong to SoftVoice, Inc. and are
**not** in this repository or in the prebuilt binaries. The library reads them
from copies of the DLLs you already own, from an installed Talk It!:

| DLL | Enables |
|---|---|
| `TIBASE32.DLL` | synthesis (with a language) |
| `TIENG32.DLL` | English |
| `TISPAN32.DLL` | Spanish (needs `TIENG32.DLL` too) |

The DLLs are parsed as files. They are never loaded or executed.

## Prebuilt binaries

Download `tispeech-<platform>` from
[Releases](https://github.com/glebasos/TalkIt_OSS/releases): `osx-arm64`,
`osx-x64`, `linux-x64`, `linux-arm64` or `win-x64`. Each has the library,
`svsay` and `include/tispeech/capi.h`.

```sh
./svsay -d /path/to/dlls -t "Hello, world." hello.wav     # -s for Spanish
```

From C, or any language with a C FFI:

```c
#include "tispeech/capi.h"

tispeech_load_dlls("/path/to/dlls");          /* once, before anything else */

char phonemes[4096];
uint8_t *pcm;
int32_t count, rate;
tispeech_text_to_phonemes(TISPEECH_LANG_ENGLISH, "Hello, world.", phonemes, sizeof phonemes);
tispeech_synthesize(TISPEECH_LANG_ENGLISH, phonemes, &pcm, &count, &rate);
/* pcm: `count` unsigned 8-bit mono samples at `rate` Hz */
tispeech_free_samples(pcm);
```

`tispeech_capabilities()` and `tispeech_synthesis_languages()` report what the
loaded DLLs enable. `capi.h` documents every call: voice options, events and
user dictionaries.

## Build from source

Needs CMake 3.20+ and a C11 compiler:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target tispeech_shared svsay
```

That build reads the DLLs at run time, like the releases.
`tools/build_release.sh <platform>` produces the release archives, cross-compiling
Linux and Windows with `zig cc`.

To compile the tables in, or to run the differential tests against the original
engine, give the DLLs at configure time. Extraction then needs Python 3.

```sh
cmake -S . -B build -DTISPEECH_BASE_DLL=/path/TIBASE32.DLL \
  -DTISPEECH_ENG_DLL=/path/TIENG32.DLL -DTISPEECH_SPAN_DLL=/path/TISPAN32.DLL
```

Input is limited to 514 Latin-1 characters per call. See
[REVERSING.md](REVERSING.md) for reconstructed addresses, build options and
differential verification against the original engine.
