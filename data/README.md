# Extracted speech data

These C sources contain the English and Spanish data consumed by the portable
engine. They compile into `libtispeech` by default; no original DLLs, Python,
external data files, or runtime extraction are needed for normal use.

| Files | Contents |
|---|---|
| `base_data.c`, `base_image.c` | Resonator coefficients, amplitude/output curves, glottal waveforms, noise, phoneme names, voice presets and expression data |
| `eng_lang_data.c`, `span_lang_data.c` | Character classes and letter-to-sound rules |
| `eng_gen_data.c` | Phoneme definitions and interpolation rates |
| `eng_image.c` | English pronunciation exceptions, text normalization, duration, prosody and generator data |
| `span_desc_image.c`, `span_image.c` | Spanish descriptors, pronunciation exceptions, text normalization and relocated generator data |

The images preserve the original address layout expected by the reconstruction,
including zero-filled scratch space. They are data for portable C code, never
executed as Windows code. This does not change ownership of the extracted
SoftVoice data.

To refresh all eight files from the original DLLs:

```sh
python3 tools/extract_data.py /path/to/original/dlls
```

The script uses only the Python standard library, validates inputs through the
existing extractors, and stages all outputs before replacing them. `manifest.json`
records input sizes and SHA-256 hashes of both inputs and generated C sources;
regeneration is deterministic and does not record local paths or timestamps.

To compare bundled data and synthesized audio against the runtime DLL loader:

```sh
cmake -S . -B build-compare \
  -DTISPEECH_BASE_DLL=/path/to/TIBASE32.DLL \
  -DTISPEECH_ENG_DLL=/path/to/TIENG32.DLL \
  -DTISPEECH_SPAN_DLL=/path/to/TISPAN32.DLL
cmake --build build-compare
ctest --test-dir build-compare --output-on-failure
```

`dllload` compares the tables and images byte for byte. `capi_loaded` checks
English and Spanish phoneme conversion and sample-identical synthesized audio.
Use `-DTISPEECH_USE_EXTRACTED_DATA=OFF` with those paths to exercise the original
build-time extraction instead. With that option and no DLL paths, the library
contains no speech data and can load DLL tables explicitly at runtime.
