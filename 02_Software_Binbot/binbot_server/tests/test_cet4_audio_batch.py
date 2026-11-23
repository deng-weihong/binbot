"""Check database coverage, word/audio alignment and resumable batch generation."""

import array
import importlib.util
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys
import wave

import numpy as np
import pytest

SCRIPTS = Path(__file__).parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))
spec = importlib.util.spec_from_file_location("cet4_audio_batch_script", SCRIPTS / "generate-cet4-audio.py")
assert spec and spec.loader
batch = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = batch
spec.loader.exec_module(batch)


def sample_signal():
    rate = 16000
    def quiet(seconds):
        return [0] * round(seconds * rate)
    def tone(frequency, seconds):
        return [round(14000 * math.sin(2 * math.pi * frequency * i / rate)) for i in range(round(seconds * rate))]
    return array.array("h", quiet(.5) + tone(220, .4) + quiet(.5) + tone(440, .5) + quiet(.5)), rate


def test_complete_sql_is_parsed_without_execution_and_matches_existing_export():
    source = batch.ROOT / "binbot_demo/cet4.sql"
    rows = batch.load_catalog(source)
    assert rows == json.loads((batch.ROOT / "binbot_console/public/cet4.json").read_text())
    assert len(rows) == 4413
    assert len({row["id"] for row in rows}) == 4413
    # There are four case variants in the database; a case-folded map would lose them.
    assert len({row["word"].casefold() for row in rows}) == 4409


def test_malformed_sql_row_is_rejected_instead_of_silently_losing_words(tmp_path):
    source = tmp_path / "cet4.sql"
    source.write_text("DROP TABLE IF EXISTS `cet4zx`;\nINSERT INTO `cet4zx` VALUES (1, 'word', NULL, 'meaning');\n")
    with pytest.raises(ValueError, match="unparsed"):
        batch.load_catalog(source)


def test_explicit_pauses_separate_words_without_cutting_speech_and_mismatches_are_rejected():
    samples, rate = sample_signal()
    start, end, boundaries, peaks = batch.word_boundaries(samples, rate, 2)
    assert .40 <= start <= .50 and 1.89 <= end <= 1.99
    assert len(boundaries) == 1 and len(peaks) == 2
    absolute_cut = start + boundaries[0]
    assert .9 < absolute_cut < 1.4
    assert all(value == 0 for value in samples[round((absolute_cut - .1) * rate):round((absolute_cut + .1) * rate)])
    with pytest.raises(batch.SplitMismatch, match="expected 3"):
        batch.word_boundaries(samples, rate, 3)
    with pytest.raises(batch.SplitMismatch, match="empty"):
        batch.word_boundaries(array.array("h", [0] * 32000), rate, 1)


@pytest.mark.skipif(not shutil.which("ffmpeg"), reason="FFmpeg is required for audio generation")
def test_cached_batch_encodes_independent_correctly_assigned_word_files_without_network(tmp_path, monkeypatch):
    rows = [{"id": 1, "word": "China"}, {"id": 2, "word": "china"}]
    source = tmp_path / "batches"
    source.mkdir()
    key = batch.digest(batch.build_ssml(rows, "us").encode())[:24]
    recording = source / ("us-" + key + ".wav")
    samples, rate = sample_signal()
    with wave.open(str(recording), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(rate)
        output.writeframes(samples.tobytes())
    def no_network(*args, **kwargs):
        raise AssertionError("cached batch must not call the remote TTS provider")
    monkeypatch.setattr(batch, "TextToSpeechClient", no_network)
    results = batch.Generator(tmp_path, 10).run(rows, "us")
    assert [entry["word"] for entry in results] == ["China", "china"]
    assert len({entry["file"] for entry in results}) == 2
    frequencies = []
    for entry in results:
        path = tmp_path / entry["file"]
        content = path.read_bytes()
        assert batch.digest(content) == entry["sha256"]
        assert batch.mp3_info(content)["durationMs"] == entry["durationMs"]
        with pytest.raises(ValueError, match="truncated"):
            batch.mp3_info(content[:-1])
        pcm = subprocess.check_output(["ffmpeg", "-v", "error", "-i", str(path), "-f", "s16le", "-"])
        decoded = array.array("h", pcm)
        # The dominant frequency distinguishes word clips despite low-level MP3
        # ringing, which adds zero crossings around silence and decoder startup.
        spectrum = abs(np.fft.rfft(np.asarray(decoded, dtype=float)))
        hz = np.fft.rfftfreq(len(decoded), 1 / 24000)
        frequencies.append(float(hz[np.argmax(spectrum)]))
    assert 195 < frequencies[0] < 245 and 415 < frequencies[1] < 465


def test_resume_only_reuses_matching_files_and_removes_corrupt_completion_records(tmp_path):
    source = tmp_path / "words.json"
    rows = [{"id": 1, "word": "China", "phonetic": "", "meaning": ""},
            {"id": 2, "word": "china", "phonetic": "", "meaning": ""}]
    source.write_text(json.dumps(rows))
    output = tmp_path / "collection"
    output.mkdir()
    db = batch.connect(output, rows, source)
    try:
        path = output / "one.mp3"
        path.write_bytes(b"complete recording")
        entry = {"id": 1, "accent": "us", "file": path.name, "sha256": batch.digest(path.read_bytes()), "bytes": path.stat().st_size, "provider": "text-to-speech.cn"}
        db.execute("INSERT INTO audio VALUES (?,?,?)", (1, "us", json.dumps(entry)))
        db.commit()
        assert batch.saved_entries(db, output) == {(1, "us"): entry}
        path.write_bytes(b"corrupted")
        assert not batch.saved_entries(db, output)
        result = batch.status(db, output, rows, ["us", "uk"], "generating")
        assert result["completed"] == 0 and result["remaining"] == 4
    finally:
        db.close()


def test_word_timestamps_preserve_order_and_reject_a_wrong_label_or_overlapping_words():
    rows = [{"id": 1, "word": "China"}, {"id": 2, "word": "china"}]
    samples, rate = sample_signal()
    events = [{"text": "China", "offset": 5_000_000, "duration": 4_000_000},
              {"text": "china", "offset": 14_000_000, "duration": 5_000_000}]
    regions = batch.timestamp_regions(rows, events, samples, rate)
    assert len(regions) == 2 and regions[0][1] < regions[1][0]
    assert regions[0][0] <= int(.5 * rate) and regions[0][1] >= int(.9 * rate)
    assert regions[1][0] <= int(1.4 * rate) and regions[1][1] >= int(1.9 * rate)
    with pytest.raises(batch.SplitMismatch, match="not aligned"):
        batch.timestamp_regions(rows, [events[0], {**events[1], "text": "Japan"}], samples, rate)
    with pytest.raises(batch.SplitMismatch, match="too close"):
        batch.timestamp_regions(rows, [events[0], {**events[1], "offset": 9_100_000}], samples, rate)
