# TalkIt_OSS

A portable C reconstruction of the SoftVoice speech engine (`TIBASE32.DLL` plus
the `TIENG32.DLL` / `TISPAN32.DLL` language modules) shipped with Microsoft
Talk It!. It builds `libtispeech` (`.dylib` / `.so` / `.dll`) for macOS, Linux
and Windows, with no emulator and no Windows DLLs at runtime.

It reconstructs the English and Spanish text front ends and the full
phoneme-to-PCM pipeline. Output is original-voice 8-bit mono PCM at 11025 Hz,
compared sample for sample against the original engine in both languages,
together with its word, syllable, phoneme and mouth-shape events. SoftVoice
user dictionaries (`SVXF`) are supported.

The .NET bindings live in [TiSpeech](https://github.com/glebasos/TiSpeech),
which builds this repository from a sibling checkout (`../TalkIt_OSS`).

## No proprietary data

The rule tables and coefficient data belong to SoftVoice, Inc. and are **not**
in this repository. They are extracted at build time from DLLs you already own
into the build tree; the DLLs are parsed as files, never loaded or executed.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DTISPEECH_BASE_DLL=/path/to/TIBASE32.DLL \
  -DTISPEECH_ENG_DLL=/path/to/TIENG32.DLL \
  -DTISPEECH_SPAN_DLL=/path/to/TISPAN32.DLL
cmake --build build --target tispeech_shared svsay
build/svsay -t "hello world" out.wav     # -s for Spanish
```

The Spanish front end needs `TIENG32.DLL` as well as `TISPAN32.DLL`. Input is
limited to 514 Latin-1 characters per call.

See [REVERSING.md](REVERSING.md) for reconstructed addresses, build options and
differential verification against the original engine.
