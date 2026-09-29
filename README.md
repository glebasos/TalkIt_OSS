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

## Speech data included

English and Spanish speech data is extracted into [`data/`](data/README.md)
and compiled directly into the library: pronunciation rules and dictionaries,
phoneme definitions, voice presets, prosody, waveforms and synthesis coefficients.
Normal builds and use need no original DLLs, Python or runtime extraction.
The native C library is still built for each target platform.

## Prebuilt binaries

Download `tispeech-<platform>` from
[Releases](https://github.com/glebasos/TalkIt_OSS/releases): `osx-arm64`,
`osx-x64`, `linux-x64`, `linux-arm64` or `win-x64`. Each has the library,
`svsay` and `include/tispeech/capi.h`.

```sh
./svsay -t "Hello, world." hello.wav     # -s for Spanish
```

From C, or any language with a C FFI:

```c
#include "tispeech/capi.h"

char phonemes[4096];
uint8_t *pcm;
int32_t count, rate;
tispeech_text_to_phonemes(TISPEECH_LANG_ENGLISH, "Hello, world.", phonemes, sizeof phonemes);
tispeech_synthesize(TISPEECH_LANG_ENGLISH, phonemes, &pcm, &count, &rate);
/* pcm: `count` unsigned 8-bit mono samples at `rate` Hz */
tispeech_free_samples(pcm);
```

`tispeech_capabilities()` and `tispeech_synthesis_languages()` report what the
compiled or explicitly loaded data enables. `capi.h` documents every call: voice options, events and
user dictionaries.

## Build from source

Needs CMake 3.20+ and a C11 compiler:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target tispeech_shared svsay
```

That build includes both languages, like releases built from this source.
`tools/build_release.sh <platform>` produces the release archives, cross-compiling
Linux and Windows with `zig cc`.

To refresh the bundled data, run `python3 tools/extract_data.py /path/to/dlls`.
See [data/README.md](data/README.md) for its inventory and reproducibility manifest.

Original DLLs are optional development inputs. Supplying them at configure time
adds byte-for-byte data and sample-for-sample audio comparisons against the
runtime loader. Add `-DTISPEECH_VERIFY_ORIGINAL=ON` for the Unicorn differential
tests (Python, Unicorn and pefile required).

```sh
cmake -S . -B build -DTISPEECH_BASE_DLL=/path/TIBASE32.DLL \
  -DTISPEECH_ENG_DLL=/path/TIENG32.DLL -DTISPEECH_SPAN_DLL=/path/TISPAN32.DLL
```

For a data-free library, configure with `-DTISPEECH_USE_EXTRACTED_DATA=OFF`
and no DLL paths. The optional `tispeech_load_dlls()` API and `svsay -d DIR`
remain available. With the same option and DLL paths, the original build-time
extraction is used instead (Python 3 required).

Input is limited to 514 Latin-1 characters per call. See
[REVERSING.md](REVERSING.md) for reconstructed addresses, build options and
differential verification against the original engine.
