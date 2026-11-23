#!/usr/bin/env python3
"""Install an immutable pronunciation dataset and switch its link atomically."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import tarfile
import tempfile

OEM = Path("/oem")


def signature(data):
    return hashlib.sha256(data).hexdigest()


def validate_manifest(manifest):
    files = manifest["files"]
    if manifest.get("version") != 1 or not 1 <= manifest["words"] <= 20000:
        raise ValueError("unsupported vocabulary dataset")
    if len(files) != manifest["recordings"] or len(files) != manifest["words"] * 2:
        raise ValueError("incomplete bilingual dataset")
    words = {}
    for name, entry in files.items():
        if not re.fullmatch(r"(?:us|uk)/[A-Za-z][A-Za-z'-]{0,63}\.mp3", name):
            raise ValueError("unsafe audio path")
        if name != entry["accent"] + "/" + entry["word"] + ".mp3" or not 4 <= entry["bytes"] <= 512 * 1024:
            raise ValueError("invalid recording metadata")
        if not re.fullmatch(r"[a-f0-9]{64}", entry["sha256"]):
            raise ValueError("invalid recording checksum")
        words.setdefault(entry["word"], set()).add(entry["accent"])
    if len(words) != manifest["words"] or any(accents != {"us", "uk"} for accents in words.values()):
        raise ValueError("missing accent or case variant")
    identity = signature(json.dumps(files, sort_keys=True, ensure_ascii=False).encode())
    if manifest["datasetId"] != identity:
        raise ValueError("dataset checksum mismatch")
    return files


def verify(directory):
    manifest = json.loads((directory / "manifest.json").read_text())
    files = validate_manifest(manifest)
    for name, entry in files.items():
        path = directory / name
        if not path.is_file() or path.is_symlink() or path.stat().st_size != entry["bytes"] or signature(path.read_bytes()) != entry["sha256"]:
            raise ValueError("installed audio checksum mismatch: " + name)
    return manifest


def write_record(path, value):
    temporary = path.with_suffix(".incoming")
    fd = os.open(temporary, os.O_CREAT | os.O_TRUNC | os.O_WRONLY, 0o600)
    with os.fdopen(fd, "w") as output:
        os.fchmod(output.fileno(), 0o600)
        json.dump(value, output, indent=2)
    temporary.replace(path)


def switch(link, target):
    temporary = link.with_name(link.name + ".incoming")
    temporary.unlink(missing_ok=True)
    temporary.symlink_to(target)
    temporary.replace(link)


def install(package, record):
    if record.exists():
        raise ValueError("this install record already exists; retain it for rollback")
    link = OEM / "assets/vocabulary-audio"
    if link.exists() and not link.is_symlink():
        raise ValueError("existing vocabulary directory must be preserved before switching to versioned assets")
    previous = os.readlink(link) if link.is_symlink() else None
    data = OEM / "data"
    data.mkdir(parents=True, exist_ok=True)
    with tarfile.open(package, "r:gz") as archive:
        members = archive.getmembers()
        by_name = {member.name: member for member in members}
        if len(by_name) != len(members) or any(not member.isfile() for member in members):
            raise ValueError("dataset archive contains duplicate or non-file entries")
        if "manifest.json" not in by_name or by_name["manifest.json"].size > 8 * 1024 * 1024:
            raise ValueError("missing or oversized vocabulary manifest")
        manifest = json.load(archive.extractfile(by_name["manifest.json"]))
        files = validate_manifest(manifest)
        if set(by_name) != set(files) | {"manifest.json", "catalog.json"}:
            raise ValueError("unexpected dataset archive contents")
        if by_name["catalog.json"].size > 8 * 1024 * 1024:
            raise ValueError("oversized vocabulary catalog")
        target = data / ("vocabulary-audio-" + manifest["datasetId"][:20])
        if target.exists():
            if verify(target)["datasetId"] != manifest["datasetId"]:
                raise ValueError("existing dataset differs")
        else:
            needed = sum(entry["bytes"] + 4096 for entry in files.values()) + 32 * 1024 * 1024
            space = os.statvfs(data)
            if space.f_bavail * space.f_frsize < needed:
                raise ValueError("not enough free space for the vocabulary dataset")
            temporary = Path(tempfile.mkdtemp(prefix=".vocabulary-audio-", dir=data))
            try:
                for accent in ("us", "uk"):
                    (temporary / accent).mkdir()
                for name, entry in files.items():
                    if by_name[name].size != entry["bytes"]:
                        raise ValueError("audio size mismatch: " + name)
                    content = archive.extractfile(by_name[name]).read()
                    if signature(content) != entry["sha256"]:
                        raise ValueError("audio checksum mismatch: " + name)
                    (temporary / name).write_bytes(content)
                for name in ("manifest.json", "catalog.json"):
                    (temporary / name).write_bytes(archive.extractfile(by_name[name]).read())
                temporary.chmod(0o755)
                temporary.replace(target)
            except BaseException:
                shutil.rmtree(temporary, ignore_errors=True)
                raise
    link.parent.mkdir(parents=True, exist_ok=True)
    write_record(record, {"link": str(link), "target": str(target), "previous": previous, "datasetId": manifest["datasetId"]})
    switch(link, str(target))
    os.sync()
    return {"status": "installed", "words": manifest["words"], "recordings": manifest["recordings"],
            "datasetId": manifest["datasetId"], "directory": str(link), "rollbackRecord": str(record)}


def rollback(record):
    value = json.loads(record.read_text())
    link = Path(value["link"])
    if not link.is_symlink() or os.readlink(link) != value["target"]:
        raise ValueError("dataset link changed since this installation")
    if value["previous"] is None:
        link.unlink()
    else:
        switch(link, value["previous"])
    os.sync()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", nargs="?", type=Path)
    parser.add_argument("--record", type=Path, default=Path(__file__).resolve().parent / "audio-installation.json")
    parser.add_argument("--rollback", action="store_true")
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    if args.rollback:
        rollback(args.record)
        print(json.dumps({"status": "rolled-back"}))
    elif args.verify:
        manifest = verify(OEM / "assets/vocabulary-audio")
        print(json.dumps({"status": "verified", "datasetId": manifest["datasetId"], "words": manifest["words"], "recordings": manifest["recordings"]}))
    elif args.package:
        print(json.dumps(install(args.package, args.record)))
    else:
        parser.error("provide a dataset package, --verify or --rollback")


if __name__ == "__main__":
    main()
