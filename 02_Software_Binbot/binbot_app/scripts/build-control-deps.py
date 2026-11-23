#!/usr/bin/env python3
"""Fetch a pinned libmosquitto and build a private host/RV1106 dependency prefix."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
VERSION = "2.0.22"
SHA256 = "2f752589ef7db40260b633fbdb536e9a04b446a315138d64a7ff3c14e2de6b68"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", choices=["host", "rv1106"])
    args = parser.parse_args()
    base = ROOT / "build/platform-deps"
    base.mkdir(parents=True, exist_ok=True)
    archive = base / f"mosquitto-{VERSION}.tar.gz"
    if not archive.exists():
        urllib.request.urlretrieve(f"https://mosquitto.org/files/source/mosquitto-{VERSION}.tar.gz", archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise SystemExit("libmosquitto archive checksum mismatch")
    source = base / f"mosquitto-{VERSION}"
    if not source.exists():
        with tarfile.open(archive) as package:
            package.extractall(base, filter="data")
    build = base / f"mosquitto-{args.target}"
    prefix = base / f"{args.target}"
    flags = [f"-DCMAKE_INSTALL_PREFIX={prefix}", "-DCMAKE_BUILD_TYPE=Release",
             "-DWITH_BROKER=OFF", "-DWITH_CLIENTS=OFF", "-DWITH_APPS=OFF", "-DWITH_PLUGINS=OFF",
             "-DDOCUMENTATION=OFF", "-DWITH_LIB_CPP=OFF", "-DWITH_CJSON=OFF", "-DWITH_STATIC_LIBRARIES=ON"]
    if args.target == "rv1106":
        flags.append(f"-DCMAKE_TOOLCHAIN_FILE={ROOT / 'cmake/toolchains/rv1106.cmake'}")
    subprocess.run(["cmake", "-S", str(source), "-B", str(build), *flags], check=True)
    subprocess.run(["cmake", "--build", str(build), "-j2"], check=True)
    subprocess.run(["cmake", "--install", str(build)], check=True)
    print(f"Configure BinBot with -DCMAKE_PREFIX_PATH={prefix}")


if __name__ == "__main__":
    main()
