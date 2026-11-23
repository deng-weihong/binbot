#!/usr/bin/env python3
"""Validate and bundle prebuilt ESP32-C3 expansion-module firmware."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import zipfile


ROOT = Path(__file__).resolve().parents[2]
PROFILES = {
    "round_screen": ("binbot_round_screen", "4MB", "圆屏/笔筒模块"),
    "holographic_screen": ("binbot_holographic_screen", "4MB", "全息显示模块"),
    "showcase_turntable": ("binbot_showcase_turntable", "2MB", "转盘模块"),
    "ambient_light": ("binbot_ambient_light", "2MB", "氛围灯模块"),
}


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def image_description(data):
    assert data[0] == 0xE9 and 0 < data[1] <= 16, "invalid ESP image header"
    assert struct.unpack_from("<H", data, 12)[0] == 5, "image is not ESP32-C3"
    assert struct.unpack_from("<I", data, 32)[0] == 0xABCD5432, "app descriptor missing"
    assert data[23] == 1, "image must have an appended SHA256"
    cursor, checksum = 24, 0xEF
    for _ in range(data[1]):
        _, length = struct.unpack_from("<II", data, cursor)
        cursor += 8
        assert cursor + length <= len(data), "truncated image segment"
        for value in data[cursor:cursor + length]:
            checksum ^= value
        cursor += length
    checksum_offset = (cursor // 16 + 1) * 16 - 1
    assert data[checksum_offset] == checksum, "ESP image checksum mismatch"
    end = checksum_offset + 1
    assert end + 32 == len(data), "unexpected data after image SHA256"
    assert hashlib.sha256(data[:end]).digest() == data[end:], "image SHA256 mismatch"

    def string(offset, size):
        raw = data[offset:offset + size]
        assert b"\0" in raw, "unterminated app metadata"
        return raw.split(b"\0", 1)[0].decode()

    return {
        "version": string(48, 32), "project": string(80, 32),
        "idf_version": string(144, 32),
        "elf_sha256": data[176:208].hex(),
    }


def partition_table(data):
    result = []
    for cursor in range(0, len(data), 32):
        entry = data[cursor:cursor + 32]
        magic = struct.unpack_from("<H", entry)[0]
        if magic == 0xEBEB:
            assert hashlib.md5(data[:cursor]).digest() == entry[16:], "partition MD5 mismatch"
            return result
        assert magic == 0x50AA, "partition MD5 trailer missing"
        _, kind, subtype, offset, size, label, flags = struct.unpack("<HBBII16sI", entry)
        result.append({"name": label.split(b"\0", 1)[0].decode(), "type": kind,
                       "subtype": subtype, "offset": offset, "size": size, "flags": flags})
    raise ValueError("partition MD5 trailer missing")


def package(profile, date, idf, public):
    project, flash_size, label = PROFILES[profile]
    source = ROOT / "binbot_component"
    build = source / "build" / ("release-" + date) / profile
    destination = public / "binbot_component"
    metadata = json.loads((build / "flasher_args.json").read_text())
    config = dict(re.findall(r"^(CONFIG_\w+)=(.*)$", (build / "sdkconfig").read_text(), re.M))
    flash_files = sorted((int(address, 0), build / name)
                         for address, name in metadata["flash_files"].items())
    application = build / (project + ".bin")
    description = image_description(application.read_bytes())
    version = (source / "version.txt").read_text().strip()
    checks = []

    def check(condition, message):
        if not condition:
            raise RuntimeError(profile + ": " + message)
        checks.append(message)

    check(description["project"] == project and description["version"] == version,
          "embedded project and version match the release")
    check(True, "ESP32-C3 app header, segment checksum and appended SHA256 are valid")
    check(metadata["extra_esptool_args"]["chip"] == "esp32c3", "flash target is ESP32-C3")
    check(metadata["flash_settings"]["flash_size"] == flash_size, "flash capacity matches hardware")
    check(config.get("CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE") == "y", "bootloader rollback is enabled")
    check(config.get("CONFIG_SECURE_FLASH_ENC_ENABLED", "n") != "y", "release image is not encrypted")
    check(config.get("CONFIG_SECURE_BOOT", "n") != "y", "release does not require private secure-boot keys")
    check(config.get("CONFIG_BINBOT_UART_OTA_ENABLE") == "y", "UART OTA is enabled")
    check(config.get("CONFIG_BINBOT_OTA_ENABLE", "n") != "y", "legacy Wi-Fi OTA is disabled")
    for key in ("CONFIG_BINBOT_OTA_WIFI_SSID", "CONFIG_BINBOT_OTA_WIFI_PASSWORD"):
        check(config.get(key, '\"\"') == '\"\"', key + " is empty")
    if profile in ("showcase_turntable", "ambient_light"):
        check(config.get("CONFIG_ESP_CONSOLE_UART_DEFAULT") == "y" and
              config.get("CONFIG_ESP_CONSOLE_SECONDARY_NONE") == "y",
              "UART0 console leaves USB GPIO18/19 available for the module")

    partitions = partition_table((build / "partition_table/partition-table.bin").read_bytes())
    check(True, "partition table MD5 is valid")
    capacity = int(flash_size.removesuffix("MB")) * 1024 * 1024
    by_offset = sorted(partitions, key=lambda item: item["offset"])
    check(all(p["offset"] + p["size"] <= capacity for p in partitions),
          "all partitions fit the declared flash")
    check(all(a["offset"] + a["size"] <= b["offset"] for a, b in zip(by_offset, by_offset[1:])),
          "partitions do not overlap")
    slots = [p for p in partitions if p["type"] == 0 and p["subtype"] in (0x10, 0x11)]
    check(len(slots) == 2 and all(application.stat().st_size <= p["size"] for p in slots),
          "application fits both OTA slots")
    app_offset = next(offset for offset, path in flash_files if path == application)
    check(app_offset == next(p["offset"] for p in slots if p["subtype"] == 0x10),
          "factory application offset matches ota_0")
    has_storage = any(p["name"] == "storage" for p in partitions)
    check(has_storage == (profile in ("round_screen", "holographic_screen")),
          "display storage is present only for display profiles")

    release = f"binbot-esp32c3-{profile}-v{version}-{date}-rc1"
    work = destination / "release-work" / release
    work.mkdir(parents=True, exist_ok=True)
    firmware = destination / "firmware"
    firmware.mkdir(exist_ok=True)
    archive = firmware / (release + ".zip")
    if archive.exists():
        raise FileExistsError(archive)
    command = [sys.executable, "-m", "esptool", "--chip", "esp32c3", "merge-bin",
               "--output", str(work / "factory.bin"), "--pad-to-size", flash_size]
    for offset, path in flash_files:
        command.extend([hex(offset), str(path)])
    subprocess.run(command, check=True)
    factory = (work / "factory.bin").read_bytes()
    check(len(factory) == capacity, "factory image has the complete flash capacity")
    check(all(factory[offset:offset + path.stat().st_size] == path.read_bytes()
              for offset, path in flash_files), "all source segments match their factory offsets")
    nvs = next(p for p in partitions if p["name"] == "nvs")
    check(factory[nvs["offset"]:nvs["offset"] + nvs["size"]] == b"\xff" * nvs["size"],
          "factory NVS is erased and contains no saved credentials")
    shutil.copy2(application, work / "ota.bin")
    check((work / "ota.bin").read_bytes() == application.read_bytes(),
          "OTA binary is the exact application image")

    readme = (destination / "README.md").read_text()
    title, body = readme.split("\n", 1)
    (work / "README.md").write_text(
        title + f"\n\n当前压缩包：**{label}**，版本 **{version}**，Flash **{flash_size}**。\n" + body)

    license_files = {
        "ESP-IDF-APACHE-2.0.txt": idf / "LICENSE",
        "FreeRTOS.txt": idf / "components/freertos/FreeRTOS-Kernel/LICENSE.md",
        "mbedtls.txt": idf / "components/mbedtls/mbedtls/LICENSE",
        "tf-psa-crypto.txt": idf / "components/mbedtls/mbedtls/tf-psa-crypto/LICENSE",
        "argtable3.txt": idf / "components/console/argtable3/LICENSE",
        "linenoise.txt": idf / "components/console/linenoise/LICENSE",
    }
    components = json.loads((build / "project_description.json").read_text()).get("build_components", [])
    for component, relative in {
        "lwip": "lwip/lwip/COPYING", "http_parser": "http_parser/LICENSE.txt",
        "wpa_supplicant": "wpa_supplicant/COPYING", "esp_wifi": "esp_wifi/lib/LICENSE",
        "esp_phy": "esp_phy/lib/LICENSE", "esp_coex": "esp_coex/lib/LICENSE",
    }.items():
        if component in components:
            license_files[component + ".txt"] = idf / "components" / relative
    if profile == "round_screen":
        fonts = ROOT / "binbot_app/assets/fonts"
        for name in ("OFL-NotoSansSC.txt", "OFL-Manrope.txt", "OFL-InterTight.txt"):
            license_files[name] = fonts / name
    (work / "licenses").mkdir(exist_ok=True)
    for name, path in license_files.items():
        shutil.copy2(path, work / "licenses" / name)

    report = {"result": "passed", "checked_at": datetime.now(timezone.utc).isoformat(),
              "checks": checks, "hardware_flash_test": "not performed"}
    (work / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
    manifest = {"release": release, "status": "release candidate", "profile": profile,
                "label": label, "chip": "esp32c3", "version": version,
                "image": description, "flash_settings": metadata["flash_settings"],
                "factory_flash_offset": "0x0", "partitions": partitions,
                "segments": [{"offset": hex(offset), "name": path.name,
                              "size": path.stat().st_size, "sha256": sha256(path)}
                             for offset, path in flash_files],
                "factory_sha256": sha256(work / "factory.bin"),
                "ota_sha256": sha256(work / "ota.bin"),
                "update_transport": "UART1 460800 8N1",
                "compatible_rv1106_app": "53",
                "offline_checks": len(checks)}
    (work / "release.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
    files = sorted(p for p in work.rglob("*") if p.is_file() and p.name != "SHA256SUMS")
    (work / "SHA256SUMS").write_text("".join(
        sha256(path) + "  " + str(path.relative_to(work)) + "\n" for path in files))
    files.append(work / "SHA256SUMS")
    temporary = archive.with_suffix(".zip.tmp")
    with zipfile.ZipFile(temporary, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as bundle:
        for path in files:
            bundle.write(path, release + "/" + str(path.relative_to(work)))
    with zipfile.ZipFile(temporary) as bundle:
        assert bundle.testzip() is None, "archive CRC mismatch"
    temporary.replace(archive)
    (firmware / (archive.name + ".sha256")).write_text(sha256(archive) + "  " + archive.name + "\n")
    result = {"profile": profile, "version": version, "archive": archive.name,
              "bytes": archive.stat().st_size, "sha256": sha256(archive),
              "offline_checks": len(checks)}
    print(json.dumps(result), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--date", default="20261106")
    parser.add_argument("--idf", type=Path, default=Path("/home/dwh/software/toolchains/esp-idf-v6.1"))
    parser.add_argument("--public", type=Path, default=ROOT / "binbot/02_Software_Binbot")
    args = parser.parse_args()
    results = [package(profile, args.date, args.idf, args.public) for profile in PROFILES]
    (args.public / "binbot_component/firmware/releases.json").write_text(
        json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
