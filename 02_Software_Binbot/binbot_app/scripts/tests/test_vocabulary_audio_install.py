import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest import mock


def module(name, file):
    spec = importlib.util.spec_from_file_location(name, file)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


SCRIPTS = Path(__file__).resolve().parents[1]
builder = module("vocabulary_package", SCRIPTS / "package-vocabulary-audio.py")
installer = module("vocabulary_installer", SCRIPTS / "device/install-vocabulary-audio.py")


class VocabularyAudioInstallTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.oem = self.root / "oem"
        (self.oem / "assets").mkdir(parents=True)
        (self.oem / "data/original").mkdir(parents=True)
        (self.oem / "data/original/preserved").write_text("keep")
        self.link = self.oem / "assets/vocabulary-audio"
        self.link.symlink_to(self.oem / "data/original")
        source = self.root / "source"
        source.mkdir()
        rows = []
        for i, word in enumerate(("China", "china"), 1):
            row = {"id": i, "word": word, "meaning": "test", "phonetic": "", "audio": {}}
            for accent in ("us", "uk"):
                filename = word + "-" + accent + ".mp3"
                content = b"\xff\xf3\x84\xc4" + word.encode() + accent.encode()
                (source / filename).write_bytes(content)
                row["audio"][accent] = {"file": filename, "sha256": builder.hashlib.sha256(content).hexdigest(),
                                          "voice": accent, "provider": "fixture"}
            rows.append(row)
        (source / "index.json").write_text(json.dumps(rows))
        self.package, self.record = self.root / "audio.tar.gz", self.root / "record.json"
        builder.build([source], self.package)
        patcher = mock.patch.object(installer, "OEM", self.oem)
        patcher.start(); self.addCleanup(patcher.stop)
        patcher = mock.patch.object(installer.os, "sync")
        patcher.start(); self.addCleanup(patcher.stop)

    def test_install_keeps_both_case_variants_and_rollback_restores_the_original_link(self):
        before = self.link.readlink()
        result = installer.install(self.package, self.record)
        self.assertEqual((result["words"], result["recordings"]), (2, 4))
        verified = installer.verify(self.link)
        self.assertEqual(len(verified["files"]), 4)
        self.assertNotEqual((self.link / "us/China.mp3").read_bytes(), (self.link / "us/china.mp3").read_bytes())
        installed = self.link.resolve()
        installer.rollback(self.record)
        self.assertEqual(self.link.readlink(), before)
        self.assertTrue(installed.is_dir())
        self.assertEqual((self.link / "preserved").read_text(), "keep")

    def altered(self, change):
        destination = self.root / "altered.tar.gz"
        with tarfile.open(self.package) as source, tarfile.open(destination, "w:gz") as target:
            for item in source:
                data = source.extractfile(item).read()
                if item.name == "us/China.mp3" and change == "corrupt":
                    data = b"x" * len(data)
                info = tarfile.TarInfo(item.name); info.size = len(data)
                if item.name == "us/China.mp3" and change == "symlink":
                    info.type = tarfile.SYMTYPE; info.linkname = "../outside"; info.size = 0
                    target.addfile(info)
                else:
                    target.addfile(info, io.BytesIO(data))
            if change == "traversal":
                info = tarfile.TarInfo("../outside"); info.size = 4
                target.addfile(info, io.BytesIO(b"bad!"))
        return destination

    def test_corrupt_payload_is_rejected_before_switching_the_live_dataset(self):
        before = self.link.readlink()
        with self.assertRaisesRegex(ValueError, "checksum"):
            installer.install(self.altered("corrupt"), self.record)
        self.assertEqual(self.link.readlink(), before)
        self.assertFalse(self.record.exists())

    def test_archive_cannot_add_paths_or_symlinks(self):
        before = self.link.readlink()
        for kind in ("traversal", "symlink"):
            with self.assertRaises(ValueError):
                installer.install(self.altered(kind), self.record)
            self.assertEqual(self.link.readlink(), before)
        self.assertFalse((self.root / "outside").exists())

    def test_incomplete_or_tampered_metadata_is_rejected(self):
        with tarfile.open(self.package) as archive:
            data = json.load(archive.extractfile("manifest.json"))
        del data["files"]["uk/china.mp3"]
        with self.assertRaisesRegex(ValueError, "incomplete"):
            installer.validate_manifest(data)


if __name__ == "__main__":
    unittest.main()
