#!/usr/bin/env python3
"""Build a local App pronunciation bundle from verified bilingual audio collections."""

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import tarfile

ROOT = Path(__file__).resolve().parents[1]


def build(sources, destination):
    entries, catalog, files = {}, {}, {}
    for source in sources:
        index = json.loads((source / "index.json").read_text())
        for row in index:
            word = row["word"]
            if not re.fullmatch(r"[A-Za-z][A-Za-z'-]{0,63}", word):
                raise ValueError("unsupported audio filename: " + word)
            if word in catalog:
                raise ValueError("duplicate spelling in audio collections: " + word)
            catalog[word] = {key: row[key] for key in ("id", "word", "phonetic", "meaning")}
            for accent in ("us", "uk"):
                recording = row["audio"][accent]
                data = (source / recording["file"]).read_bytes()
                signature = hashlib.sha256(data).hexdigest()
                if signature != recording["sha256"] or not 4 <= len(data) <= 512 * 1024:
                    raise ValueError("invalid source recording: " + word)
                name = accent + "/" + word + ".mp3"
                files[name] = data
                entries[name] = {"word": word, "accent": accent, "catalogId": row["id"],
                                 "bytes": len(data), "sha256": signature,
                                 "voice": recording["voice"], "provider": recording.get("provider", "text-to-speech.cn")}
    identity = hashlib.sha256(json.dumps(entries, sort_keys=True, ensure_ascii=False).encode()).hexdigest()
    manifest = {"version": 1, "datasetId": identity, "words": len(catalog),
                "recordings": len(files), "files": entries}
    files["manifest.json"] = (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode()
    files["catalog.json"] = (json.dumps(list(catalog.values()), ensure_ascii=False, indent=2) + "\n").encode()
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(destination.suffix + ".incoming")
    with tarfile.open(temporary, "w:gz", compresslevel=1) as archive:
        for name, data in files.items():
            info = tarfile.TarInfo(name)
            info.size, info.mode = len(data), 0o644
            archive.addfile(info, io.BytesIO(data))
    temporary.chmod(0o600)
    temporary.replace(destination)
    return {"datasetId": identity, "words": len(catalog), "recordings": len(entries),
            "audioBytes": sum(entry["bytes"] for entry in entries.values()),
            "packageBytes": destination.stat().st_size,
            "packageSha256": hashlib.sha256(destination.read_bytes()).hexdigest(), "package": str(destination)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", action="append", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "build/vocabulary-audio.tar.gz")
    args = parser.parse_args()
    sources = args.source or [ROOT.parent / "binbot_platform/build/cet4-audio",
                             ROOT.parent / "binbot_platform/build/app-vocabulary-extra/generated"]
    print(json.dumps(build(sources, args.output), ensure_ascii=False))


if __name__ == "__main__":
    main()
