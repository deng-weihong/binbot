#!/usr/bin/env python3
"""Collect attributed dictionary audio and verify offline playback files."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile
import time
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import Request, urlopen

CATALOGS = Path(__file__).resolve().parents[1] / "src/binbot_server/catalogs"
OUTPUT = Path(__file__).resolve().parents[2] / "build/vocabulary-audio"
MAXIMUM = 4 * 1024 * 1024


def download(url: str) -> bytes:
    parsed = urlsplit(url)
    if parsed.scheme != "https" or parsed.hostname not in {
        "api.dictionaryapi.dev", "upload.wikimedia.org", "dict.youdao.com",
    }:
        raise ValueError("recording URL must use a reviewed HTTPS source")
    for attempt in range(3):
        try:
            request = Request(url, headers={"User-Agent": "BinBotVocabulary/1.0 (educational pronunciation collection)"})
            with urlopen(request, timeout=25) as response:
                content = response.read(MAXIMUM + 1)
            if not content or len(content) > MAXIMUM:
                raise ValueError("empty or oversized recording")
            return content
        except HTTPError as exc:
            if exc.code == 429:
                raise RuntimeError("source rate limit; retry after " + exc.headers.get("Retry-After", "later")) from exc
            if exc.code not in {408, 500, 502, 503, 504} or attempt == 2:
                raise
            time.sleep(2 * (attempt + 1))
        except (URLError, TimeoutError):
            if attempt == 2:
                raise
            time.sleep(2 * (attempt + 1))
    raise RuntimeError("recording download failed")


def inspect_audio(path: Path) -> dict:
    probe = subprocess.run([
        "ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries",
        "stream=codec_name,sample_rate,channels:format=duration", "-of", "json", str(path),
    ], check=True, capture_output=True, timeout=15)
    info = json.loads(probe.stdout)
    stream = info["streams"][0]
    duration = float(info["format"]["duration"])
    if stream["codec_name"] != "mp3" or stream["channels"] != 1 or int(stream["sample_rate"]) != 24000:
        raise ValueError("recording must be mono 24 kHz MP3")
    if not .15 <= duration <= 8:
        raise ValueError("recording duration is outside the single-word limit")
    decoded = subprocess.run([
        "ffmpeg", "-v", "error", "-i", str(path), "-f", "s16le", "-acodec", "pcm_s16le", "-",
    ], check=True, capture_output=True, timeout=15).stdout
    # Decode every recording, reject HTML responses, corrupt files and silent clips.
    import array
    samples = array.array("h", decoded)
    if not samples or max(abs(sample) for sample in samples) < 100:
        raise ValueError("recording has no audible signal")
    return {"durationMs": round(duration * 1000), "sampleRate": 24000, "channels": 1}


def collect(source: dict, root: Path, previous: dict, verify_only: bool) -> dict:
    word = source["word"]
    if not re.fullmatch(r"[a-z]+(?:[-'][a-z]+)*", word) or len(word) > 64:
        raise ValueError("invalid recording word")
    for key in ("author", "sourceUrl", "license", "licenseUrl", "downloadUrl", "accent"):
        if not isinstance(source.get(key), str) or not source[key]:
            raise ValueError("missing source attribution: " + key)
    if source["accent"] not in {"en-US", "en-GB"}:
        raise ValueError("unsupported pronunciation accent")
    identity = word + ("-us" if source["accent"] == "en-US" else "-uk")
    target = root / (identity + ".mp3")
    prior = previous.get(identity)
    same_source = prior and all(prior.get(key) == value for key, value in source.items())
    if target.is_file() and same_source and hashlib.sha256(target.read_bytes()).hexdigest() == prior["sha256"]:
        return {**prior, **inspect_audio(target)}
    if verify_only:
        raise ValueError("missing, changed or unverified audio: " + identity)
    original = download(source["downloadUrl"])
    with tempfile.TemporaryDirectory(prefix="binbot-word-audio-") as temporary:
        incoming, encoded = Path(temporary) / "source", Path(temporary) / "audio.mp3"
        incoming.write_bytes(original)
        subprocess.run([
            "ffmpeg", "-nostdin", "-v", "error", "-y", "-i", str(incoming),
            "-map", "0:a:0", "-map_metadata", "-1", "-ac", "1", "-ar", "24000",
            "-codec:a", "libmp3lame", "-b:a", "64k", "-write_xing", "0", "-id3v2_version", "0",
            str(encoded),
        ], check=True, capture_output=True, timeout=30)
        info = inspect_audio(encoded)
        data = encoded.read_bytes()
    incoming_path = target.with_suffix(".incoming")
    incoming_path.write_bytes(data)
    incoming_path.replace(target)
    return {
        **source, "id": identity, "file": target.name, **info, "bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "sourceSha256": hashlib.sha256(original).hexdigest(),
        "changes": "Downloaded dictionary audio; transcoded to mono 24 kHz, 64 kbit/s MP3.",
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", type=Path, default=CATALOGS / "vocabulary.json")
    parser.add_argument("--sources", type=Path, default=CATALOGS / "vocabulary-audio-sources.json")
    parser.add_argument("--output", type=Path, default=OUTPUT)
    parser.add_argument("--verify", action="store_true", help="verify all assets without network access")
    parser.add_argument("--bundle", type=Path, help="write a portable collection including attribution")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    sources = json.loads(args.sources.read_text())["entries"]
    expected = {row["word"].strip().lower() for row in json.loads(args.catalog.read_text())}
    wanted = {(word, accent) for word in expected for accent in ("en-US", "en-GB")}
    if {(source["word"], source["accent"]) for source in sources} != wanted or len(sources) != len(wanted):
        raise ValueError("reviewed sources must cover both accents of the exact vocabulary catalog")
    manifest_path = args.output / "manifest.json"
    previous = json.loads(manifest_path.read_text()) if manifest_path.is_file() else {}
    known = {row["id"]: row for row in previous.get("entries", [])}
    results, failures = [], []

    def run(source: dict) -> tuple[dict, str | None]:
        try:
            return collect(source, args.output, known, args.verify), None
        except Exception as exc:
            return source, str(exc)

    with ThreadPoolExecutor(max_workers=2) as pool:
        for entry, error in pool.map(run, sources):
            if error:
                failures.append({"word": entry["word"], "accent": entry["accent"], "error": error})
                print(json.dumps(failures[-1], ensure_ascii=False), flush=True)
            else:
                results.append(entry)
                print(json.dumps({"word": entry["word"], "verified": True, "accent": entry["accent"]}), flush=True)
    if failures:
        # Preserve the last complete manifest; partial downloads never appear as a complete release.
        raise SystemExit("Missing or invalid recordings: " + ", ".join(item["word"] for item in failures))
    manifest = {
        "version": 1, "collectedAt": previous.get("collectedAt", datetime.now(timezone.utc).isoformat()),
        "defaultAccent": "en-US", "entries": results,
    }
    temporary = manifest_path.with_suffix(".incoming")
    temporary.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
    temporary.replace(manifest_path)
    attribution = [
        "# Vocabulary pronunciation recordings", "",
        "Downloaded dictionary pronunciation audio; see each original source below.",
        "No open redistribution license was supplied by Youdao. Source terms apply.",
        "These runtime cache files are not covered by the repository's code license and are not committed to source.",
        "Changes: transcoded to mono 24 kHz, 64 kbit/s MP3.", "",
        "| Word | Accent | Recording author | Source | License |", "| --- | --- | --- | --- | --- |",
    ]
    for entry in results:
        attribution.append(
            f"| {entry['word']} | {entry['accent']} | {entry['author']} | "
            f"[Original recording]({entry['sourceUrl']}) | [{entry['license']}]({entry['licenseUrl']}) |"
        )
    (args.output / "ATTRIBUTION.md").write_text("\n".join(attribution) + "\n")
    if args.bundle:
        args.bundle.parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(args.bundle, "w:gz") as archive:
            for name in [row["file"] for row in results] + ["manifest.json", "ATTRIBUTION.md"]:
                data = (args.output / name).read_bytes()
                info = tarfile.TarInfo(name)
                info.size, info.mode = len(data), 0o644
                archive.addfile(info, io.BytesIO(data))
    print(json.dumps({"status": "verified", "words": len(expected), "recordings": len(results), "bytes": sum(row["bytes"] for row in results)}))


if __name__ == "__main__":
    main()
