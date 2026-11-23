#!/usr/bin/env python3
"""Install App/audio/MQTT binaries with a private backup and automatic rollback."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time

OEM = Path("/oem")
INIT = Path("/etc/init.d/S99binbot")
PIDS = Path("/var/run")
BINARIES = ("binbot_app", "binbot_audio", "binbot_control")
PRESERVE = ("binbot_dialog", "binbot_wake", "binbot_camera", "binbot_slave", "binbot_component")


def atomic_copy(source, target, mode):
    temporary = target.with_name(target.name + ".control-incoming")
    with source.open("rb") as incoming:
        fd = os.open(str(temporary), os.O_CREAT | os.O_TRUNC | os.O_WRONLY, mode)
        with os.fdopen(fd, "wb") as output:
            os.fchmod(output.fileno(), mode)
            shutil.copyfileobj(incoming, output)
            output.flush()
            os.fsync(output.fileno())
    os.replace(str(temporary), str(target))


def replace_bytes(target, data, mode=0o600):
    temporary = target.with_name(target.name + ".control-incoming")
    fd = os.open(str(temporary), os.O_CREAT | os.O_TRUNC | os.O_WRONLY, mode)
    with os.fdopen(fd, "wb") as output:
        os.fchmod(output.fileno(), mode)
        output.write(data)
        output.flush()
        os.fsync(output.fileno())
    os.replace(str(temporary), str(target))


def process_identity(name):
    pidfile = PIDS / (name + ".pid")
    try:
        pid = int(pidfile.read_text().strip())
        proc = Path("/proc") / str(pid)
        fields = (proc / "stat").read_text().rsplit(")", 1)[1].split()
        if fields[0] in {"Z", "X"}:
            return None
        executable = os.readlink(str(proc / "exe")).removesuffix(" (deleted)")
    except (FileNotFoundError, ProcessLookupError):
        # Exiting processes may retain /proc/<pid> after exe disappears.
        return None
    if Path(executable).name != name:
        raise RuntimeError("PID file does not match " + name)
    return [pid, fields[19]]


def stop(name):
    identity = process_identity(name)
    if identity:
        os.kill(identity[0], signal.SIGTERM)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if process_identity(name) != identity:
                break
            time.sleep(0.1)
        else:
            os.kill(identity[0], signal.SIGKILL)
    (PIDS / (name + ".pid")).unlink(missing_ok=True)


def start_services(stage):
    text = INIT.read_text()
    head, marker, _ = text.partition('\ncase "${1:-start}" in')
    if not marker or "start_daemon()" not in head:
        raise RuntimeError("unsupported device init script")
    functions = stage / "service-functions.sh"
    replace_bytes(functions, (head + "\n").encode())
    script = '. {}; cd {}; start_daemon binbot_audio -8; start_daemon binbot_app 5'.format(
        shlex.quote(str(functions)), shlex.quote(str(OEM)))
    if (OEM / "binbot_control").exists():
        script += '; if [ "${BINBOT_CONTROL_ENABLE:-0}" = 1 ]; then start_daemon binbot_control; fi'
    subprocess.run(["/bin/sh", "-ec", script], check=True, stdout=subprocess.DEVNULL)


def fingerprint(name):
    target = OEM / name
    return {"sha256": hashlib.sha256(target.read_bytes()).hexdigest() if target.exists() else None,
            "process": process_identity(name)}


def validate_stage(stage):
    manifest = json.loads((stage / "manifest.json").read_text())
    expected = set(BINARIES) | {"init-binbot.sh", "install-control.py"}
    if set(manifest["files"]) != expected:
        raise ValueError("unexpected deployment manifest")
    for name, digest in manifest["files"].items():
        data = (stage / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError("deployment checksum mismatch: " + name)
        if name in BINARIES and (data[:6] != b"\x7fELF\x01\x01" or int.from_bytes(data[18:20], "little") != 40):
            raise ValueError("expected an ARM ELF32 binary: " + name)


def update_environment(text, config):
    if not isinstance(config, dict) or set(config) != {"device_id", "token", "mqtt_host", "mqtt_port"}:
        raise ValueError("invalid deployment configuration")
    if not re.fullmatch(r"[A-Za-z0-9._:-]{1,64}", config["device_id"]) or config["device_id"] in {".", ".."}:
        raise ValueError("invalid device ID")
    if not re.fullmatch(r"[A-Za-z0-9._:-]{1,253}", config["mqtt_host"]):
        raise ValueError("invalid MQTT host")
    if not isinstance(config["token"], str) or not 24 <= len(config["token"]) <= 512 or "\n" in config["token"]:
        raise ValueError("invalid device token")
    if type(config["mqtt_port"]) is not int or not 1 <= config["mqtt_port"] <= 65535:
        raise ValueError("invalid MQTT port")
    values = {"BINBOT_CONTROL_ENABLE": "1", "BINBOT_DEVICE_ID": config["device_id"],
              "BINBOT_DEVICE_TOKEN": config["token"], "BINBOT_MQTT_HOST": config["mqtt_host"],
              "BINBOT_MQTT_PORT": str(config["mqtt_port"])}
    lines = []
    for line in text.splitlines():
        match = re.match(r"^\s*(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*=", line)
        if not match or match[1] not in values:
            lines.append(line)
    lines.extend(key + "=" + shlex.quote(value) for key, value in values.items())
    return "\n".join(lines) + "\n"


def capture_environment():
    env = OEM / "binbot.env"
    script = "set -a; "
    if env.exists():
        script += ". {}; ".format(shlex.quote(str(env)))
    script += "env -0"
    raw = subprocess.check_output(["/bin/sh", "-ec", script])
    return dict(item.split("=", 1) for item in raw.decode().split("\0") if "=" in item)


def backup_file(source, backup, label, files):
    if source.exists():
        destination = backup / label
        shutil.copy2(str(source), str(destination))
        destination.chmod(0o600)
        files.append({"target": str(source), "label": label,
                      "mode": source.stat().st_mode & 0o777,
                      "link": os.readlink(str(source)) if source.is_symlink() else None})
    else:
        files.append({"target": str(source), "label": None})


def rollback(stage):
    backup = stage / "backup"
    document = json.loads((backup / "files.json").read_text())
    for name in ("binbot_control", "binbot_app", "binbot_audio"):
        stop(name)
    for row in document:
        target = Path(row["target"])
        if row["label"] is None:
            target.unlink(missing_ok=True)
        elif row.get("link"):
            temporary = target.with_name(target.name + ".control-incoming")
            temporary.unlink(missing_ok=True)
            temporary.symlink_to(row["link"])
            os.replace(str(temporary), str(target))
        else:
            atomic_copy(backup / row["label"], target, row["mode"])
    start_services(stage)
    os.sync()
    print("Control deployment rolled back")


def activate(stage, config):
    validate_stage(stage)
    environment = capture_environment()
    original_id = environment.get("BINBOT_DEVICE_ID", "binbot-001")
    if config["device_id"] != original_id:
        raise ValueError("device ID differs from this device's current identity")
    envfile = OEM / "binbot.env"
    updated = update_environment(envfile.read_text() if envfile.exists() else "", config)
    backup = stage / "backup"
    backup.mkdir(mode=0o700)
    preserved = {name: fingerprint(name) for name in PRESERVE}
    files = []
    for name in BINARIES:
        backup_file(OEM / name, backup, name, files)
    backup_file(INIT, backup, "init-binbot.sh", files)
    backup_file(envfile, backup, "binbot.env", files)
    replace_bytes(backup / "files.json", json.dumps(files).encode())
    try:
        for name in ("binbot_control", "binbot_app", "binbot_audio"):
            stop(name)
        state = Path(environment.get("BINBOT_STATE_PATH", str(OEM / "data/binbot_state.ini")))
        if state.exists():
            shutil.copy2(str(state), str(backup / "state.ini"))
            (backup / "state.ini").chmod(0o600)
        for name in BINARIES:
            atomic_copy(stage / name, OEM / name, 0o755)
        atomic_copy(stage / "init-binbot.sh", INIT, 0o755)
        replace_bytes(envfile, updated.encode())
        (OEM / "data").mkdir(mode=0o700, exist_ok=True)
        start_services(stage)
        time.sleep(3)
        if not all(process_identity(name) for name in BINARIES):
            raise RuntimeError("a deployed process exited; rolling back")
        if any(fingerprint(name) != before for name, before in preserved.items()):
            raise RuntimeError("an unrelated device service changed; rolling back")
        control_socket = Path(environment.get("BINBOT_CONTROL_SOCKET", "/tmp/binbot_control.sock"))
        if not control_socket.is_socket():
            raise RuntimeError("App control socket was not created; rolling back")
        os.sync()
    except BaseException:
        rollback(stage)
        raise
    print(json.dumps({"status": "installed", "device_id": config["device_id"],
                      "release": str(stage), "rollback": "python3 " + str(stage / "install-control.py") + " --rollback"}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rollback", action="store_true")
    args = parser.parse_args()
    stage = Path(__file__).resolve().parent
    if args.rollback:
        rollback(stage)
    else:
        activate(stage, json.load(sys.stdin))


if __name__ == "__main__":
    main()
