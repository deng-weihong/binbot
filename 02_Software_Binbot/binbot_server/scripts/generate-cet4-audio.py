#!/usr/bin/env python3
"""Generate a resumable, verified US/UK pronunciation collection from the CET-4 SQL dump."""

from __future__ import annotations

import argparse
import array
import asyncio
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
from datetime import datetime, timezone
import fcntl
import hashlib
from html import escape, unescape
import json
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import tarfile
import tempfile
import threading
import time
from urllib.error import HTTPError, URLError
import wave

from text_to_speech_cn import SynthesisOptions, TextToSpeechClient, TtsError, TtsRequestError

ROOT = Path(__file__).resolve().parents[1]
PROFILES = {
    "us": {"language": "English (United States)", "locale": "en-US", "voice": "en-US-JennyNeural"},
    "uk": {"language": "English (United Kingdom)", "locale": "en-GB", "voice": "en-GB-SoniaNeural"},
}
INSERT = re.compile(
    r"^INSERT INTO `cet4zx` VALUES \((\d+), '((?:\\.|''|[^'\\])*)', "
    r"'((?:\\.|''|[^'\\])*)', '((?:\\.|''|[^'\\])*)'\);\s*$", re.MULTILINE
)


def utc() -> str:
    return datetime.now(timezone.utc).isoformat()


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def atomic_json(path: Path, value: dict | list) -> None:
    temporary = path.with_suffix(path.suffix + ".incoming")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")
    temporary.replace(path)


def mysql_string(value: str) -> str:
    substitutions = {"0": "\0", "n": "\n", "r": "\r", "t": "\t", "Z": "\x1a"}
    return re.sub(r"\\(.)", lambda match: substitutions.get(match[1], match[1]), value).replace("''", "'")


def load_catalog(path: Path) -> list[dict]:
    text = path.read_text(encoding="utf-8-sig")
    if path.suffix.lower() == ".json":
        rows = json.loads(text)
    else:
        matches = list(INSERT.finditer(text))
        if len(matches) != len(re.findall(r"^INSERT INTO `cet4zx`", text, re.MULTILINE)):
            raise ValueError("SQL contains an unparsed vocabulary row; no SQL is executed")
        rows = [{"id": int(m[1]), "word": mysql_string(m[2]),
                 "phonetic": mysql_string(m[3]), "meaning": mysql_string(m[4])} for m in matches]
    if not isinstance(rows, list) or not rows:
        raise ValueError("vocabulary must contain word records")
    if len({row["id"] for row in rows}) != len(rows):
        raise ValueError("vocabulary IDs must be unique")
    for row in rows:
        if (not isinstance(row["id"], int) or not 1 <= row["id"] <= 1_000_000
                or not isinstance(row["word"], str) or not re.fullmatch(r"[A-Za-z]{1,64}", row["word"])):
            raise ValueError("unexpected CET-4 word record")
    return sorted(rows, key=lambda row: row["id"])


class SplitMismatch(ValueError):
    pass


class GenerationBlocked(RuntimeError):
    pass


def read_wave(path: Path) -> tuple[array.array, int]:
    with wave.open(str(path), "rb") as recording:
        if recording.getnchannels() != 1 or recording.getsampwidth() != 2 or recording.getcomptype() != "NONE":
            raise ValueError("TTS must return mono PCM16 WAV")
        rate = recording.getframerate()
        if rate != 16000 or recording.getnframes() > 16_000 * 900:
            raise ValueError("unexpected TTS sample rate or duration")
        samples = array.array("h", recording.readframes(recording.getnframes()))
    if not samples or max(map(abs, samples)) < 100:
        raise ValueError("TTS returned silent or empty audio")
    return samples, rate


def word_boundaries(samples: array.array, rate: int, count: int) -> tuple[float, float, list[float], list[int]]:
    """Find only the explicit >= 500 ms inter-word pauses, retaining speech tails."""
    step = rate // 100
    peaks = [max(map(abs, samples[i:i + step]), default=0) for i in range(0, len(samples), step)]
    speech = [i for i, value in enumerate(peaks) if value >= 50]
    if not speech:
        raise SplitMismatch("empty speech")
    first, last = speech[0], speech[-1]
    start, end = max(0., first * .01 - .07), min(len(samples) / rate, (last + 1) * .01 + .07)
    pauses, begin = [], None
    for i in range(first, last + 2):
        value = peaks[i] if i <= last else 10000
        if value < 50 and begin is None:
            begin = i
        elif value >= 50 and begin is not None:
            if i - begin >= 35:
                pauses.append((begin + i) * .005)
            begin = None
    if count == 1:
        pauses = []
    if len(pauses) + 1 != count:
        raise SplitMismatch(f"expected {count} words, found {len(pauses) + 1} speech regions")
    points = [start] + pauses + [end]
    signal_peaks = []
    for left, right in zip(points, points[1:]):
        peak = max(peaks[max(0, int(left * 100)):min(len(peaks), int(right * 100) + 1)], default=0)
        if not .12 <= right - left <= 8 or peak < 100:
            raise SplitMismatch("word clip is silent, too short or too long")
        signal_peaks.append(peak)
    return start, end, [point - start for point in pauses], signal_peaks


def mp3_info(data: bytes) -> dict:
    """Validate every independent MPEG-2/Layer-III frame produced for the device."""
    cursor, frames = 0, 0
    while cursor < len(data):
        if cursor + 4 > len(data):
            raise ValueError("truncated MP3 header")
        header = int.from_bytes(data[cursor:cursor + 4], "big")
        if ((header >> 21) != 0x7FF or (header >> 19) & 3 != 2
                or (header >> 17) & 3 != 1 or (header >> 12) & 15 != 8
                or (header >> 10) & 3 != 1 or (header >> 6) & 3 != 3):
            raise ValueError("expected 24 kHz / mono / 64 kbit/s MP3 without tags")
        length = 192 + ((header >> 9) & 1)
        if cursor + length > len(data):
            raise ValueError("truncated MP3 frame")
        side = cursor + 4 + (0 if (header >> 16) & 1 else 2)
        if data[side] != 0:
            raise ValueError("MP3 frame depends on another clip's bit reservoir")
        cursor += length
        frames += 1
    if frames < 5:
        raise ValueError("MP3 clip is too short")
    return {"durationMs": round(frames * 576 / 24000 * 1000), "sampleRate": 24000,
            "channels": 1, "bitrate": 64000, "frames": frames}


def encode_clips(source: Path, rows: list[dict], accent: str, output: Path,
                 start: float, end: float, boundaries: list[float], peaks: list[int]) -> list[dict]:
    profile = PROFILES[accent]
    destination = output / "mp3" / accent
    destination.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="encode-", dir=output / "batches") as directory:
        command = [
            "ffmpeg", "-nostdin", "-v", "error", "-y", "-i", str(source), "-map", "0:a:0",
            "-af", f"atrim=start={start:.5f}:end={end:.5f},asetpts=PTS-STARTPTS", "-map_metadata", "-1",
            "-ac", "1", "-ar", "24000", "-c:a", "libmp3lame", "-b:a", "64k", "-reservoir", "0",
            "-f", "segment", "-segment_format", "mp3", "-reset_timestamps", "1",
            "-segment_format_options", "write_xing=0:id3v2_version=0",
        ]
        if boundaries:
            command += ["-segment_times", ",".join(f"{point:.5f}" for point in boundaries)]
        else:
            command += ["-segment_time", "1000"]
        command += [str(Path(directory) / "%04d.mp3")]
        subprocess.run(command, check=True, capture_output=True, timeout=90)
        clips = sorted(Path(directory).glob("*.mp3"))
        if len(clips) != len(rows):
            raise SplitMismatch("encoder returned the wrong number of clips")
        results = []
        for row, clip, peak in zip(rows, clips, peaks):
            content = clip.read_bytes()
            info = mp3_info(content)
            target = destination / f"{row['id']:04d}-{row['word']}.mp3"
            incoming = target.with_suffix(".incoming")
            incoming.write_bytes(content)
            incoming.replace(target)
            results.append({
                "id": row["id"], "word": row["word"], "accent": accent,
                "locale": profile["locale"], "voice": profile["voice"],
                "file": str(target.relative_to(output)), "bytes": len(content), "sha256": digest(content),
                **info, "peak": peak, "batch": source.name, "generatedAt": utc(),
            })
        return results


def build_ssml(rows: list[dict], accent: str) -> str:
    profile = PROFILES[accent]
    body = '<break time="500ms"/>' + "".join(escape(row["word"]) + '<break time="500ms"/>' for row in rows)
    return (f'<speak version="1.0" xmlns="http://www.w3.org/2001/10/synthesis" xml:lang="{profile["locale"]}">'
            f'<voice name="{profile["voice"]}">{body}</voice></speak>')


class Generator:
    def __init__(self, output: Path, timeout: float):
        self.output, self.timeout = output, timeout
        self.lock = threading.Lock()
        self.requests = 0
        self.fallbacks = 0

    def run(self, rows: list[dict], accent: str) -> list[dict]:
        profile = PROFILES[accent]
        ssml = build_ssml(rows, accent)
        if len(ssml) > 9500:
            raise ValueError("SSML batch exceeds safe input size")
        key = digest(ssml.encode())[:24]
        source = self.output / "batches" / f"{accent}-{key}.wav"
        if not source.is_file():
            for attempt in range(3):
                try:
                    client = TextToSpeechClient(timeout=self.timeout)
                    with self.lock:
                        self.requests += 1
                    url = client.synthesize(SynthesisOptions(
                        text=ssml, language=profile["language"], voice=profile["voice"],
                        style="general", ssml=True,
                    ))
                    client.download(url, source)
                    break
                except HTTPError as exc:
                    if exc.code in {401, 402, 403, 429}:
                        raise GenerationBlocked(f"source HTTP {exc.code}; Retry-After={exc.headers.get('Retry-After', 'unspecified')}") from exc
                    if attempt == 2:
                        raise
                    time.sleep(5 * (attempt + 1))
                except TtsRequestError as exc:
                    if exc.code in {401, 402, "401", "402"} or any(term in exc.message for term in ("次数", "额度", "频繁", "并发", "登录")):
                        raise GenerationBlocked(f"source request {exc.code}: {exc.message[:160]}") from exc
                    if attempt == 2:
                        raise
                    time.sleep(5 * (attempt + 1))
                except (URLError, TimeoutError, TtsError):
                    if attempt == 2:
                        raise
                    time.sleep(5 * (attempt + 1))
        samples, rate = read_wave(source)
        try:
            start, end, boundaries, peaks = word_boundaries(samples, rate, len(rows))
            return encode_clips(source, rows, accent, self.output, start, end, boundaries, peaks)
        except SplitMismatch:
            if len(rows) == 1:
                raise
            with self.lock:
                self.fallbacks += 1
            middle = len(rows) // 2
            return self.run(rows[:middle], accent) + self.run(rows[middle:], accent)


def timestamp_regions(rows: list[dict], events: list[dict], samples: array.array, rate: int) -> list[tuple[int, int]]:
    if len(events) != len(rows):
        raise SplitMismatch("TTS word timestamps do not cover the input words")
    regions = []
    for i, (row, event) in enumerate(zip(rows, events)):
        spoken = re.sub(r"[^A-Za-z]", "", unescape(event["text"]))
        if spoken.casefold() != row["word"].casefold():
            raise SplitMismatch("TTS word timestamps are not aligned with the source vocabulary")
        start, end = event["offset"] / 1e7, (event["offset"] + event["duration"]) / 1e7
        previous = ((events[i - 1]["offset"] + events[i - 1]["duration"]) / 1e7) if i else 0.
        following = events[i + 1]["offset"] / 1e7 if i + 1 < len(events) else len(samples) / rate
        if len(rows) > 1 and i + 1 < len(events) and following - end < .2:
            raise SplitMismatch("words are too close to split safely; use a smaller batch")
        left, right = max(0., start - .10, (previous + start) / 2 if i else 0.), min(len(samples) / rate, end + .10, (end + following) / 2)
        if not .12 <= right - left <= 8:
            raise SplitMismatch("invalid word timestamps")
        regions.append((round(left * rate), round(right * rate)))
    return regions


class EdgeGenerator(Generator):
    """Use the normal ReadAloud stream and its word timestamps, with matching voices."""

    async def fetch(self, rows: list[dict], accent: str, audio: Path, metadata: Path) -> None:
        import edge_tts
        profile = PROFILES[accent]
        # Semicolons give single-letter words a pause without treating them as abbreviations.
        text = ";\n".join(row["word"] for row in rows) + "."
        speech = edge_tts.Communicate(text, voice=profile["voice"], boundary="WordBoundary",
                                     connect_timeout=20, receive_timeout=self.timeout)
        content, events = bytearray(), []
        with self.lock:
            self.requests += 1
        async for event in speech.stream():
            if event["type"] == "audio":
                content.extend(event["data"])
                if len(content) > 32 * 1024 * 1024:
                    raise ValueError("TTS stream exceeds the batch audio limit")
            elif event["type"] == "WordBoundary":
                events.append(event)
        if not content:
            raise ValueError("TTS stream returned no audio")
        temporary = audio.with_suffix(".incoming")
        temporary.write_bytes(content)
        temporary.replace(audio)
        atomic_json(metadata, events)

    def run(self, rows: list[dict], accent: str) -> list[dict]:
        profile = PROFILES[accent]
        key = digest(json.dumps([profile["voice"], [row["word"] for row in rows], "semicolon-v1"]).encode())[:24]
        stem = self.output / "batches" / f"edge-{accent}-{key}"
        audio, metadata, decoded, source = (stem.with_suffix(suffix) for suffix in (".mp3", ".json", ".decoded.wav", ".wav"))
        if not audio.is_file() or not metadata.is_file():
            for attempt in range(3):
                try:
                    asyncio.run(asyncio.wait_for(self.fetch(rows, accent, audio, metadata), timeout=self.timeout + 30))
                    break
                except Exception as exc:
                    if getattr(exc, "status", None) in {401, 402, 403, 429}:
                        raise GenerationBlocked(f"ReadAloud source HTTP {exc.status}") from exc
                    if attempt == 2:
                        raise
                    time.sleep(5 * (attempt + 1))
        if not decoded.is_file():
            temporary = decoded.with_suffix(".incoming.wav")
            subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-y", "-i", str(audio),
                            "-ac", "1", "-ar", "16000", "-c:a", "pcm_s16le", str(temporary)],
                           check=True, capture_output=True, timeout=90)
            temporary.replace(decoded)
        samples, rate = read_wave(decoded)
        events = json.loads(metadata.read_text())
        try:
            regions = timestamp_regions(rows, events, samples, rate)
        except SplitMismatch:
            if len(rows) == 1:
                # A single source word has no neighbouring word to mix up. Keep
                # its complete speech even if the provider splits its spelling.
                regions = [(0, len(samples))]
            else:
                with self.lock:
                    self.fallbacks += 1
                middle = len(rows) // 2
                return self.run(rows[:middle], accent) + self.run(rows[middle:], accent)
        joined, boundaries, peaks = array.array("h", [0] * (rate // 10)), [], []
        for i, (left, right) in enumerate(regions):
            clip = samples[left:right]
            peak = max(map(abs, clip), default=0)
            if peak < 100:
                raise ValueError("TTS word clip is silent")
            joined.extend(clip)
            peaks.append(peak)
            if i + 1 < len(regions):
                boundaries.append((len(joined) + rate // 4) / rate)
                joined.extend(array.array("h", [0] * (rate // 2)))
        joined.extend(array.array("h", [0] * (rate // 10)))
        with wave.open(str(source), "wb") as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(rate)
            output.writeframes(joined.tobytes())
        records = encode_clips(source, rows, accent, self.output, 0., len(joined) / rate, boundaries, peaks)
        for record in records:
            record["provider"] = "Microsoft Edge ReadAloud"
        return records


def connect(output: Path, rows: list[dict], source: Path) -> sqlite3.Connection:
    database = output / "progress.sqlite3"
    db = sqlite3.connect(database)
    database.chmod(0o600)
    db.execute("PRAGMA journal_mode=WAL")
    db.execute("CREATE TABLE IF NOT EXISTS metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL)")
    db.execute("CREATE TABLE IF NOT EXISTS audio (word_id INTEGER, accent TEXT, value TEXT NOT NULL, PRIMARY KEY(word_id,accent))")
    config = {"version": 1, "sourceSha256": digest(source.read_bytes()), "catalogSha256": digest(json.dumps(rows, ensure_ascii=False, sort_keys=True).encode()),
              "profiles": PROFILES, "format": "mono-24000hz-64k-mp3", "pauseMs": 500}
    previous = db.execute("SELECT value FROM metadata WHERE key='config'").fetchone()
    if previous and json.loads(previous[0]) != config:
        raise ValueError("output belongs to a different source or voice configuration; use another directory")
    db.execute("INSERT OR IGNORE INTO metadata VALUES ('config',?)", (json.dumps(config),))
    db.execute("INSERT OR IGNORE INTO metadata VALUES ('createdAt',?)", (json.dumps(utc()),))
    db.commit()
    atomic_json(output / "catalog.json", rows)
    return db


def saved_entries(db: sqlite3.Connection, output: Path) -> dict[tuple[int, str], dict]:
    result = {}
    invalid = []
    for word_id, accent, value in db.execute("SELECT word_id,accent,value FROM audio"):
        entry = json.loads(value)
        entry.setdefault("provider", "text-to-speech.cn")
        path = output / entry["file"]
        if path.is_file() and digest(path.read_bytes()) == entry["sha256"]:
            result[word_id, accent] = entry
        else:
            invalid.append((word_id, accent))
    if invalid:
        with db:
            db.executemany("DELETE FROM audio WHERE word_id=? AND accent=?", invalid)
    return result


def status(db: sqlite3.Connection, output: Path, rows: list[dict], accents: list[str], state: str,
           generator: Generator | None = None, error: str | None = None) -> dict:
    ids = {row["id"] for row in rows}
    entries = [json.loads(value) for word_id, accent, value in db.execute("SELECT word_id,accent,value FROM audio") if word_id in ids and accent in accents]
    counts = {accent: sum(entry["accent"] == accent for entry in entries) for accent in accents}
    result = {"status": state, "updatedAt": utc(), "words": len(rows), "expectedRecordings": len(rows) * len(accents),
              "completed": len(entries), "remaining": len(rows) * len(accents) - len(entries), "accents": counts,
              "bytes": sum(entry["bytes"] for entry in entries)}
    if generator:
        result.update({"requestsThisRun": generator.requests, "splitFallbacksThisRun": generator.fallbacks})
    if error:
        result["error"] = re.sub(r"https?://\S+", "[URL]", error)[:400]
    atomic_json(output / "progress.json", result)
    print(json.dumps(result, ensure_ascii=False), flush=True)
    return result


def finalize(db: sqlite3.Connection, output: Path, rows: list[dict], accents: list[str], bundle: Path | None) -> None:
    records = saved_entries(db, output)
    entries = [records[(row["id"], accent)] for row in rows for accent in accents]
    for entry in entries:
        info = mp3_info((output / entry["file"]).read_bytes())
        if info["durationMs"] != entry["durationMs"]:
            raise ValueError("recording metadata does not match its file")
    # One decoder process checks the entire collection instead of spawning one per word.
    playlist = output / "verification.ffconcat"
    playlist.write_text("ffconcat version 1.0\n" + "".join("file '" + entry["file"] + "'\n" for entry in entries))
    subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-xerror", "-err_detect", "explode",
                    "-f", "concat", "-safe", "0", "-i", str(playlist), "-f", "null", "-"],
                   check=True, capture_output=True, timeout=300)
    config = json.loads(db.execute("SELECT value FROM metadata WHERE key='config'").fetchone()[0])
    manifest = {"version": 1, "sources": sorted({entry["provider"] for entry in entries}), "kind": "synthetic speech", "verifiedAt": utc(),
                "sourceSha256": config["sourceSha256"], "words": len(rows), "recordings": len(entries), "profiles": PROFILES,
                "entries": entries}
    atomic_json(output / "manifest.json", manifest)
    atomic_json(output / "index.json", [{**row, "audio": {accent: records[(row["id"], accent)] for accent in accents}} for row in rows])
    (output / "README.md").write_text(
        f"# CET-4 pronunciation audio\n\n{len(rows)} word records, {len(entries)} generated recordings.\n\n"
        "Source vocabulary: the project's cet4zx SQL export; IDs and capitalization are preserved.\n"
        "TTS sources are recorded per file in manifest.json; synthetic speech.\n"
        "US voice: en-US-JennyNeural. UK voice: en-GB-SoniaNeural.\n"
        "Files: mp3/us/ and mp3/uk/; mono, 24 kHz, 64 kbit/s MP3.\n"
        "index.json maps the original word IDs to the audio files. manifest.json records every SHA-256.\n"
        "All files passed frame checks and FFmpeg decoding. Segmentation must match the number of input words.\n"
        "Existing pauses are retained between clips; ambiguous batches fall back to smaller batches.\n"
        "Single-word synthesis uses a common dictionary reading; a word with several pronunciations has one reading per accent.\n"
        "The original vocabulary, application configuration and service credentials are not modified.\n"
        "No transient TTS credentials or expiring download URLs are included.\n"
    )
    if bundle:
        bundle.parent.mkdir(parents=True, exist_ok=True)
        temporary = bundle.with_suffix(bundle.suffix + ".incoming")
        with tarfile.open(temporary, "w:gz", compresslevel=1) as archive:
            for name in ["catalog.json", "index.json", "manifest.json", "README.md"] + [entry["file"] for entry in entries]:
                archive.add(output / name, arcname=name, recursive=False)
        temporary.replace(bundle)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "src/binbot_server/catalogs/cet4.json")
    parser.add_argument("--output", type=Path, default=ROOT / "runtime/cet4-audio")
    parser.add_argument("--accents", choices=("both", "us", "uk"), default="both")
    parser.add_argument("--workers", type=int, choices=(1, 2, 3, 4), default=2)
    parser.add_argument("--batch-size", type=int, default=64)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--provider", choices=("tts-cn", "edge"), default="tts-cn")
    parser.add_argument("--limit", type=int, default=0, help="generate the first N entries for a pilot")
    parser.add_argument("--bundle", type=Path)
    parser.add_argument("--status", action="store_true")
    parser.add_argument("--verify", action="store_true", help="verify a complete collection without network requests")
    args = parser.parse_args()
    if not 1 <= args.batch_size <= 200 or args.limit < 0:
        parser.error("batch size must be 1..200; limit must be nonnegative")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "batches").mkdir(exist_ok=True)
    all_rows = load_catalog(args.source)
    rows = all_rows[:args.limit] if args.limit else all_rows
    accents = ["us", "uk"] if args.accents == "both" else [args.accents]
    if args.status:
        db = sqlite3.connect(args.output / "progress.sqlite3")
        try:
            existing = args.output / "progress.json"
            print(existing.read_text() if existing.is_file() else "{}")
        finally:
            db.close()
        return 0
    with (args.output / "generation.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise SystemExit("another generator is already using this output directory")
        db = connect(args.output, all_rows, args.source)
        try:
            if args.verify:
                status(db, args.output, rows, accents, "verifying")
                finalize(db, args.output, rows, accents, args.bundle)
                status(db, args.output, rows, accents, "complete")
                return 0
            existing = saved_entries(db, args.output)
            accent_jobs = {}
            for accent in accents:
                pending = [row for row in rows if (row["id"], accent) not in existing]
                accent_jobs[accent] = [(pending[i:i + args.batch_size], accent) for i in range(0, len(pending), args.batch_size)]
            jobs = [accent_jobs[accent][i] for i in range(max((len(items) for items in accent_jobs.values()), default=0))
                    for accent in accents if i < len(accent_jobs[accent])]
            generator = (EdgeGenerator if args.provider == "edge" else Generator)(args.output, args.timeout)
            status(db, args.output, rows, accents, "generating", generator)
            failures, blocked = [], None
            atomic_json(args.output / "failures.json", [])
            iterator = iter(jobs)
            with ThreadPoolExecutor(max_workers=args.workers) as pool:
                active = {}

                def fill() -> None:
                    while not blocked and len(active) < args.workers:
                        job = next(iterator, None)
                        if job is None:
                            return
                        active[pool.submit(generator.run, *job)] = job

                fill()
                while active:
                    completed, _ = wait(active, timeout=25, return_when=FIRST_COMPLETED)
                    for future in completed:
                        job = active.pop(future)
                        try:
                            records = future.result()
                            with db:
                                for record in records:
                                    db.execute("INSERT OR REPLACE INTO audio VALUES (?,?,?)",
                                               (record["id"], record["accent"], json.dumps(record, ensure_ascii=False)))
                        except GenerationBlocked as exc:
                            blocked = str(exc)
                            failures.append({"ids": [row["id"] for row in job[0]], "accent": job[1], "error": blocked})
                        except Exception as exc:
                            failures.append({"ids": [row["id"] for row in job[0]], "accent": job[1], "error": re.sub(r"https?://\S+", "[URL]", str(exc))[:300]})
                        atomic_json(args.output / "failures.json", failures)
                    status(db, args.output, rows, accents, "waiting" if blocked else "generating", generator, blocked)
                    fill()
            complete = saved_entries(db, args.output)
            if any((row["id"], accent) not in complete for row in rows for accent in accents):
                status(db, args.output, rows, accents, "incomplete", generator, blocked or "Some batches failed; rerun to resume only missing recordings")
                return 1
            status(db, args.output, rows, accents, "verifying", generator)
            finalize(db, args.output, rows, accents, args.bundle)
            atomic_json(args.output / "failures.json", [])
            status(db, args.output, rows, accents, "complete", generator)
            return 0
        finally:
            db.close()


if __name__ == "__main__":
    raise SystemExit(main())
