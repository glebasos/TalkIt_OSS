#!/usr/bin/env python3
"""Refresh all bundled speech data from the three original DLLs (Python stdlib only)."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dll_directory", type=Path)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] / "data")
    args = parser.parse_args()
    tools = Path(__file__).resolve().parent
    dlls = {}
    for name in ("TIBASE32.DLL", "TIENG32.DLL", "TISPAN32.DLL"):
        matches = [p for p in args.dll_directory.iterdir() if p.name.upper() == name and p.is_file()]
        if len(matches) != 1:
            parser.error(f"expected exactly one {name} in {args.dll_directory}")
        dlls[name] = matches[0].resolve()
    base, eng, span = (dlls[name] for name in dlls)
    jobs = [
        ("base_data.c", "extract_base.py", [base], ["--symbol", "sv_base_tables"]),
        ("base_image.c", "extract_images.py", [base], ["--symbol", "sv_base_image"]),
        ("eng_lang_data.c", "extract_lang.py", [eng], ["--symbol", "sv_lang_data_eng"]),
        ("eng_gen_data.c", "extract_generator.py", [eng], ["--language", "eng", "--symbol", "sv_gen_data_eng"]),
        ("eng_image.c", "extract_images.py", [eng], ["--symbol", "sv_eng_image", "--language",
            "--text-range", "1c205fb9:1c206780", "--text-range", "1c209c20:1c249310"]),
        ("span_lang_data.c", "extract_lang.py", [span], ["--language", "span", "--symbol", "sv_lang_data_span"]),
        ("span_desc_image.c", "extract_images.py", [span], ["--symbol", "sv_span_desc_image", "--language"]),
        ("span_image.c", "extract_span_frontend.py", [eng, span], ["--symbol", "sv_span_image"]),
    ]
    # Finish every extractor before replacing the existing data, so malformed
    # inputs cannot leave a mixture of old and new tables.
    with tempfile.TemporaryDirectory(prefix="tispeech-data-") as directory:
        staging = Path(directory)
        for name, script, inputs, options in jobs:
            subprocess.run([sys.executable, str(tools / script), *map(str, inputs),
                            str(staging / name), *options], check=True,
                           env={**os.environ, "PYTHONUTF8": "1"})
            # Normalize line endings so hashes also reproduce on Windows.
            source = (staging / name).read_text(encoding="utf-8")
            with (staging / name).open("w", encoding="utf-8", newline="\n") as output:
                output.write(source)
        manifest = {
            "format": 1,
            "sources": {name: {"size": path.stat().st_size, "sha256": digest(path)}
                        for name, path in dlls.items()},
            "outputs": {name: digest(staging / name) for name, *_ in jobs},
        }
        with (staging / "manifest.json").open("w", encoding="utf-8", newline="\n") as output:
            output.write(json.dumps(manifest, indent=2) + "\n")
        args.output.mkdir(parents=True, exist_ok=True)
        for path in staging.iterdir():
            shutil.copyfile(path, args.output / path.name)
    print(f"Extracted {len(jobs)} speech data files into {args.output}")


if __name__ == "__main__":
    main()
