from __future__ import annotations

import asyncio
import hashlib
import hmac
import io
import json
import logging
import os
import secrets
import shutil
import sqlite3
import subprocess
import time
import struct
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any, cast
from uuid import UUID, uuid4

from esptool.bin_image import ESP32C3FirmwareImage

from binbot_server.application.registry import DeviceSessionRegistry
from binbot_server.config import ComponentConfig
from binbot_server.application.module_images import image_name, prepare_module_image

if TYPE_CHECKING:
    from binbot_server.application.tools import DeviceCommandService

_LOG = logging.getLogger(__name__)

PROFILES = {"binbot_round_screen": "round_screen",
            "binbot_holographic_screen": "holographic_screen",
            "binbot_showcase_turntable": "showcase_turntable",
            "binbot_ambient_light": "ambient_light",
            "binbot_slave": "slave_mcu"}
COMPONENT_ASSETS = {
    "round_screen": {"display.gif": (240, 240)},
    "holographic_screen": {"display.gif": (320, 240)},
    "showcase_turntable": {},
    "ambient_light": {},
}
TERMINAL = {"succeeded", "failed", "cancelled"}
PHASES = {"preparing", "downloading", "transferring", "validating", "activating",
          "verifying", "reconciling", "succeeded", "failed", "cancelled"}


class OTAError(Exception):
    def __init__(self, code: str, message: str, status: int = 409):
        super().__init__(message)
        self.code, self.status = code, status


@dataclass(frozen=True)
class Principal:
    name: str
    scopes: frozenset[str]
    device_ids: frozenset[str] = frozenset()

    def require(self, scope: str, device_id: str = "") -> None:
        if scope not in self.scopes or (device_id and self.device_ids and device_id not in self.device_ids):
            raise OTAError("FORBIDDEN", "credential does not permit this operation", 403)


def parse_firmware(data: bytes) -> dict[str, Any]:
    if data.startswith(b"BBSOTA1\0"):
        return parse_slave_firmware(data)
    try:
        image = ESP32C3FirmwareImage(io.BytesIO(data))
        if image.chip_id != 5 or image.checksum != image.calculate_checksum():
            raise ValueError("wrong chip or image checksum")
        if not image.append_digest or image.stored_digest != image.calc_digest:
            raise ValueError("missing or invalid image digest")
        if image.data_length + 32 != len(data):
            raise ValueError("trailing data or unsupported signed image")
        descriptor = data[32:288]
        if len(descriptor) != 256 or int.from_bytes(descriptor[:4], "little") != 0xABCD5432:
            raise ValueError("missing application descriptor")
        version = descriptor[16:48].split(b"\0", 1)[0].decode("ascii")
        project = descriptor[48:80].split(b"\0", 1)[0].decode("ascii")
        if project not in PROFILES or not version.isascii() or not version.isdecimal():
            raise ValueError("unknown project or non-integer version")
        return {"profile": PROFILES[project], "project": project, "version": version,
                "elf_sha256": descriptor[144:176].hex(), "size_bytes": len(data),
                "sha256": hashlib.sha256(data).hexdigest()}
    except Exception as exc:
        raise OTAError("INVALID_FIRMWARE", f"invalid ESP32-C3 application image: {exc}", 400) from exc


def parse_slave_firmware(data: bytes) -> dict[str, Any]:
    try:
        if len(data) < 168:
            raise ValueError("truncated package")
        fmt, chip, version, capacity, a, b = struct.unpack_from("<6I", data, 8)
        if fmt != 1 or chip != 0x582 or capacity != 0x35000 or any(data[136:156]):
            raise ValueError("unsupported format, chip or layout")
        if (any(size < 4 or size > capacity or size % 4 for size in (a, b))
                or len(data) != 160 + a + b):
            raise ValueError("invalid image sizes")
        if zlib.crc32(data[:156]) != int.from_bytes(data[156:160], "little"):
            raise ValueError("header CRC mismatch")
        for slot, image in enumerate((data[160:160 + a], data[160 + a:])):
            if hashlib.sha256(image).digest() != data[64 + slot * 32:96 + slot * 32]:
                raise ValueError("slot SHA-256 mismatch")
            expected_crc = int.from_bytes(data[128 + slot * 4:132 + slot * 4], "little")
            if zlib.crc32(image) != expected_crc:
                raise ValueError("slot CRC mismatch")
        identity = hashlib.sha256(struct.pack("<I", version) + data[64:128]).digest()
        if identity != data[32:64]:
            raise ValueError("release identity mismatch")
        return {"profile": "slave_mcu", "project": "binbot_slave", "version": str(version),
                "elf_sha256": identity.hex(), "image_size_bytes": max(a, b),
                "size_bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    except (ValueError, struct.error) as exc:
        raise OTAError("INVALID_FIRMWARE", f"invalid slave A/B package: {exc}", 400) from exc


class ComponentOTAService:
    """Persistent catalog, job state and retryable outbox owned by the main Server."""

    def __init__(self, config: ComponentConfig, registry: DeviceSessionRegistry,
                 asset_directory: str = "", ffmpeg_path: str = "/usr/bin/ffmpeg",
                 *, command_service: DeviceCommandService | None = None):
        self.config, self.registry = config, registry
        self.directory = Path(config.directory)
        self.directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.directory.chmod(0o700)
        self.db = sqlite3.connect(self.directory / "component.sqlite3")
        self.db.row_factory = sqlite3.Row
        self.db.executescript("""
            PRAGMA journal_mode=WAL;
            PRAGMA synchronous=FULL;
            CREATE TABLE IF NOT EXISTS component_uploads (
                id TEXT PRIMARY KEY, owner TEXT, size INTEGER, sha TEXT, token_hash TEXT,
                expires REAL, state TEXT, firmware_id TEXT);
            CREATE TABLE IF NOT EXISTS component_firmware (
                id TEXT PRIMARY KEY, sha TEXT UNIQUE, metadata TEXT, created REAL);
            CREATE TABLE IF NOT EXISTS component_assets (
                id TEXT PRIMARY KEY, owner TEXT, file_name TEXT, size INTEGER,
                sha TEXT, path TEXT, created REAL);
            CREATE TABLE IF NOT EXISTS component_snapshots (
                device TEXT, component TEXT, info TEXT, observed REAL,
                PRIMARY KEY(device, component));
            CREATE TABLE IF NOT EXISTS component_jobs (
                id TEXT PRIMARY KEY, owner TEXT, device TEXT, component TEXT, firmware TEXT,
                idem TEXT, fingerprint TEXT, status TEXT, snapshot TEXT,
                UNIQUE(owner, device, component, idem));
            CREATE UNIQUE INDEX IF NOT EXISTS component_one_active
                ON component_jobs(device, component)
                WHERE status NOT IN ('succeeded', 'failed', 'cancelled');
            CREATE TABLE IF NOT EXISTS component_outbox (
                job TEXT PRIMARY KEY, sent REAL DEFAULT 0);
        """)
        self.db.execute("UPDATE component_uploads SET state='prepared' WHERE state='writing'")
        self.db.commit()
        self._probes: dict[str, tuple[str, asyncio.Future[dict[str, Any]], str]] = {}
        self._task: asyncio.Task[None] | None = None
        self._waiting_probes: dict[str, asyncio.Task[None]] = {}
        self.asset_directory = Path(asset_directory or str(self.directory / "assets"))
        self.ffmpeg_path = ffmpeg_path
        self.command_service = command_service

    def principal(self, token: str) -> Principal:
        if self.config.allow_unauthenticated:
            return Principal("unauthenticated", frozenset(self.config.scopes), frozenset(self.config.device_ids))
        expected = os.environ.get(self.config.token_env, "")
        if not expected or not hmac.compare_digest(expected, token):
            raise OTAError("UNAUTHORIZED", "explicit service credential required", 401)
        return Principal("mcp-service", frozenset(self.config.scopes), frozenset(self.config.device_ids))

    def _one(self, table: str, key: str) -> sqlite3.Row:
        # table names are internal constants, never request parameters.
        row = self.db.execute(f"SELECT * FROM {table} WHERE id=?", (key,)).fetchone()
        if row is None:
            raise OTAError("NOT_FOUND", "record does not exist", 404)
        return cast(sqlite3.Row, row)

    async def capable(self, device: str) -> None:
        row = next((d for d in await self.registry.snapshot() if d["id"] == device), None)
        if row is None:
            raise OTAError("DEVICE_OFFLINE", "device is not connected")
        if not isinstance(row["capabilities"], list) or "component_ota_v1" not in row["capabilities"]:
            raise OTAError("CAPABILITY_REQUIRED", "device requires component_ota_v1")

    async def send(self, device: str, name: str, payload: dict[str, Any], command: str) -> None:
        await self.capable(device)
        await self.registry.send_command(device, {"type": "device.command", "command_id": command,
                                                 "name": name, "payload": payload})

    async def component(self, principal: Principal, device: str, component: str,
                        fresh: bool = True) -> dict[str, Any]:
        principal.require("device:read", device)
        if component not in {"component-1", "slave-1"}:
            raise OTAError("UNKNOWN_COMPONENT", "unknown component", 404)
        if fresh:
            request_id = str(uuid4())
            future = asyncio.get_running_loop().create_future()
            self._probes[request_id] = (device, future, component)
            try:
                async with asyncio.timeout(5):
                    await self.send(device, "component.probe", {"component_id": component}, request_id)
                    await future
            except TimeoutError as exc:
                raise OTAError("PROBE_TIMEOUT", "component did not respond within five seconds", 504) from exc
            finally:
                self._probes.pop(request_id, None)
        row = self.db.execute("SELECT * FROM component_snapshots WHERE device=? AND component=?",
                              (device, component)).fetchone()
        if row is None:
            raise OTAError("NO_SNAPSHOT", "component has not been observed", 404)
        online = any(d["id"] == device for d in await self.registry.snapshot())
        return {**json.loads(row["info"]), "device_id": device, "component_id": component,
                "observed_at": row["observed"], "fresh": fresh, "online": online}

    def prepare(self, principal: Principal, size: int, sha: str) -> dict[str, Any]:
        principal.require("firmware:write")
        if not self.config.public_origin:
            raise OTAError("PUBLIC_ORIGIN_REQUIRED", "configure component.public_origin for upload URLs", 503)
        if not 168 <= size <= self.config.max_firmware_bytes or len(sha) != 64:
            raise OTAError("INVALID_UPLOAD", "invalid size or SHA-256", 400)
        try:
            bytes.fromhex(sha)
        except ValueError as exc:
            raise OTAError("INVALID_UPLOAD", "invalid SHA-256", 400) from exc
        upload, token, expires = str(uuid4()), secrets.token_urlsafe(32), time.time() + 900
        self.db.execute("INSERT INTO component_uploads VALUES(?,?,?,?,?,?,?,NULL)",
                        (upload, principal.name, size, sha.lower(), hashlib.sha256(token.encode()).hexdigest(),
                         expires, "prepared"))
        self.db.commit()
        return {"upload_id": upload, "upload_url": self.config.public_origin.rstrip("/") +
                f"/api/v1/admin/firmware/uploads/{upload}?token={token}", "expires_at": expires}

    def begin_upload(self, upload: str, token: str) -> sqlite3.Row:
        row = self._one("component_uploads", upload)
        if time.time() > row["expires"] or not hmac.compare_digest(
            row["token_hash"], hashlib.sha256(token.encode()).hexdigest()
        ):
            raise OTAError("UPLOAD_EXPIRED", "upload credential expired or rejected", 401)
        if row["state"] != "prepared":
            raise OTAError("UPLOAD_USED", "upload slot is already in use")
        self.db.execute("UPDATE component_uploads SET state='writing' WHERE id=?", (upload,))
        self.db.commit()
        return row

    def finish_upload(self, upload: str, valid: bool) -> None:
        self.db.execute("UPDATE component_uploads SET state=? WHERE id=?",
                        ("uploaded" if valid else "prepared", upload))
        self.db.commit()

    async def register(self, principal: Principal, upload: str) -> dict[str, Any]:
        principal.require("firmware:write")
        row = self._one("component_uploads", upload)
        if row["owner"] != principal.name:
            raise OTAError("FORBIDDEN", "upload belongs to another principal", 403)
        if row["firmware_id"]:
            return self.firmware(principal, row["firmware_id"], write=True)
        if row["state"] != "uploaded":
            raise OTAError("UPLOAD_INCOMPLETE", "upload must complete first")
        data = await asyncio.to_thread((self.directory / f"{upload}.upload").read_bytes)
        metadata = await asyncio.to_thread(parse_firmware, data)
        if metadata["sha256"] != row["sha"] or len(data) != row["size"]:
            raise OTAError("HASH_MISMATCH", "uploaded file does not match declaration", 400)
        with self.db:
            existing = self.db.execute("SELECT id FROM component_firmware WHERE sha=?",
                                       (metadata["sha256"],)).fetchone()
            firmware_id = existing["id"] if existing else str(uuid4())
            if not existing:
                path = self.directory / f'{metadata["sha256"]}.bin'
                os.replace(self.directory / f"{upload}.upload", path)
                fd = os.open(self.directory, os.O_DIRECTORY)
                try:
                    os.fsync(fd)
                finally:
                    os.close(fd)
                self.db.execute("INSERT INTO component_firmware VALUES(?,?,?,?)",
                                (firmware_id, metadata["sha256"], json.dumps(metadata), time.time()))
            self.db.execute("UPDATE component_uploads SET firmware_id=? WHERE id=?", (firmware_id, upload))
        return self.firmware(principal, firmware_id, write=True)

    def firmware(self, principal: Principal, firmware: str, write: bool = False) -> dict[str, Any]:
        principal.require("firmware:write" if write else "firmware:read")
        row = self._one("component_firmware", firmware)
        return {**json.loads(row["metadata"]), "firmware_id": row["id"], "created_at": row["created"]}

    def list_firmware(self, principal: Principal, profile: str = "", cursor: str = "",
                      limit: int = 50) -> dict[str, Any]:
        principal.require("firmware:read")
        if not 1 <= limit <= 100 or (profile and profile not in PROFILES.values()):
            raise OTAError("INVALID_ARGUMENT", "invalid profile or limit", 400)
        rows = self.db.execute("SELECT * FROM component_firmware WHERE id>? ORDER BY id", (cursor,))
        items = [{**json.loads(r["metadata"]), "firmware_id": r["id"], "created_at": r["created"]}
                 for r in rows if not profile or json.loads(r["metadata"])["profile"] == profile]
        return {"items": items[:limit], "next_cursor": items[limit - 1]["firmware_id"] if len(items) > limit else None}

    def job(self, principal: Principal, job: str, write: bool = False) -> dict[str, Any]:
        row = self._one("component_jobs", job)
        principal.require("component:ota:write" if write else "device:read", row["device"])
        return cast(dict[str, Any], json.loads(row["snapshot"]))

    def list_jobs(self, principal: Principal, device: str, limit: int = 30) -> dict[str, Any]:
        principal.require("device:read", device)
        if not device or not 1 <= limit <= 100:
            raise OTAError("INVALID_ARGUMENT", "device_id and a limit in 1..100 are required", 400)
        rows = self.db.execute(
            "SELECT snapshot FROM component_jobs WHERE device=? "
            "ORDER BY status IN ('succeeded','failed','cancelled'), json_extract(snapshot, '$.created_at') DESC LIMIT ?",
            (device, limit))
        return {"items": [self.public_job(json.loads(row["snapshot"])) for row in rows]}

    async def start_job(self, principal: Principal, device: str, component: str, firmware: str,
                        idem: str, allow_downgrade: bool = False,
                        wait_for_component: bool = False) -> dict[str, Any]:
        principal.require("component:ota:write", device)
        if component not in {"component-1", "slave-1"}:
            raise OTAError("UNKNOWN_COMPONENT", "unknown component", 404)
        if not idem or len(idem) > 128:
            raise OTAError("INVALID_ARGUMENT", "idempotency_key must contain 1..128 characters", 400)
        fingerprint = json.dumps([firmware, allow_downgrade])
        if wait_for_component:
            fingerprint = json.dumps([firmware, allow_downgrade, True])
        def existing_job() -> dict[str, Any] | None:
            existing = self.db.execute("SELECT * FROM component_jobs WHERE owner=? AND device=? AND component=? AND idem=?",
                                       (principal.name, device, component, idem)).fetchone()
            if existing:
                if existing["fingerprint"] != fingerprint:
                    raise OTAError("IDEMPOTENCY_CONFLICT", "idempotency key is bound to another request")
                return cast(dict[str, Any], json.loads(existing["snapshot"]))
            return None
        existing = existing_job()
        if existing:
            return self.public_job(existing)
        target = self.firmware(principal, firmware)
        if (component == "slave-1") != (target["profile"] == "slave_mcu"):
            raise OTAError("WRONG_PROFILE", "firmware does not match component target")
        before = None if wait_for_component else await self.component(principal, device, component)
        if before is not None:
            if target["profile"] != before["profile"] or target["project"] != before["project"]:
                raise OTAError("WRONG_PROFILE", "firmware does not match component")
            image_size = target.get("image_size_bytes", target["size_bytes"])
            if not before["healthy"] or image_size > before["capacity"]:
                raise OTAError("NOT_READY", "component unhealthy or firmware exceeds OTA slot")
            if not before["version"].isascii() or not before["version"].isdecimal():
                raise OTAError("UNSUPPORTED_VERSION", "component version must be a nonnegative integer")
            if int(target["version"]) < int(before["version"]) and not allow_downgrade:
                raise OTAError("DOWNGRADE_REJECTED", "allow_downgrade is required for an older version")
        job_id = str(uuid4())
        current = before is not None and target["elf_sha256"] == before["elf_sha256"] and target["version"] == before["version"]
        job = {"job_id": job_id, "device_id": device, "component_id": component,
               "firmware_id": firmware, "target": target, "before": before,
               "status": "waiting_for_component" if wait_for_component else "succeeded" if current else "preparing",
               "last_phase": "waiting_for_component" if wait_for_component else "preparing",
               "event_seq": 0, "bytes_written": 0, "total_bytes": target["size_bytes"],
               "phase_percent": 0, "last_event_at": time.time(), "created_at": time.time(),
               "cancel_requested": False, "result": "already_current" if current else None,
               "download_token": secrets.token_urlsafe(32)}
        if wait_for_component:
            job.update(wait_for_component=True, allow_downgrade=allow_downgrade,
                       waiting_reason="component_not_observed", last_probe_at=0)
        try:
            with self.db:
                # A concurrent probe may have yielded while the same key was created.
                existing = existing_job()
                if existing:
                    return self.public_job(existing)
                self.db.execute("INSERT INTO component_jobs VALUES(?,?,?,?,?,?,?,?,?)",
                                (job_id, principal.name, device, component, firmware, idem, fingerprint,
                                 job["status"], json.dumps(job)))
                if not current and not wait_for_component:
                    self.db.execute("INSERT INTO component_outbox(job) VALUES(?)", (job_id,))
        except sqlite3.IntegrityError as exc:
            raise OTAError("COMPONENT_BUSY", "component has an active or reconciling job") from exc
        return self.public_job(job)

    def _start_waiting_jobs(self, device: str, component: str, info: dict[str, Any]) -> None:
        rows = self.db.execute(
            "SELECT snapshot FROM component_jobs WHERE device=? AND component=? "
            "AND status='waiting_for_component'", (device, component)).fetchall()
        for row in rows:
            job = json.loads(row["snapshot"])
            target = job["target"]
            if any(target[key] != info[key] for key in ("profile", "project")):
                job["waiting_reason"] = "waiting_for_matching_profile"
                self._save_job(job)
                continue
            if not info["healthy"]:
                job["waiting_reason"] = "component_initializing"
                self._save_job(job)
                continue
            version = info["version"]
            if not version.isascii() or not version.isdecimal():
                job.update(status="failed", result="unsupported_component_version")
            elif target.get("image_size_bytes", target["size_bytes"]) > info["capacity"]:
                job.update(status="failed", result="firmware_exceeds_ota_slot")
            elif int(target["version"]) < int(version) and not job["allow_downgrade"]:
                job.update(status="succeeded", result="newer_version_present")
            elif target["version"] == version and target["elf_sha256"] == info["elf_sha256"]:
                job.update(status="succeeded", result="already_current")
            else:
                job.update(status="preparing", result=None)
            job.update(before={**info, "device_id": device, "component_id": component,
                               "observed_at": time.time(), "fresh": True, "online": True},
                       waiting_reason=None, last_phase=job["status"], last_event_at=time.time())
            # Commit the preflight and delivery intent together, including after restart.
            with self.db:
                self.db.execute("UPDATE component_jobs SET status=?,snapshot=? WHERE id=?",
                                (job["status"], json.dumps(job), job["job_id"]))
                if job["status"] == "preparing":
                    self.db.execute("INSERT OR IGNORE INTO component_outbox(job) VALUES(?)", (job["job_id"],))

    async def _probe_waiting_job(self, job_id: str) -> None:
        try:
            row = self._one("component_jobs", job_id)
            job = json.loads(row["snapshot"])
            if job["status"] != "waiting_for_component":
                return
            p = Principal("pending-ota", frozenset({"device:read"}), frozenset({job["device_id"]}))
            await self.component(p, job["device_id"], job["component_id"])
        except OTAError as exc:
            job = json.loads(self._one("component_jobs", job_id)["snapshot"])
            if job["status"] == "waiting_for_component":
                job["waiting_reason"] = exc.code.lower()
                self._save_job(job)
        finally:
            self._waiting_probes.pop(job_id, None)

    @staticmethod
    def public_job(job: dict[str, Any]) -> dict[str, Any]:
        return {k: v for k, v in job.items() if k != "download_token"}

    def _save_job(self, job: dict[str, Any]) -> None:
        with self.db:
            self.db.execute("UPDATE component_jobs SET status=?,snapshot=? WHERE id=?",
                            (job["status"], json.dumps(job), job["job_id"]))
            if job["status"] in TERMINAL:
                self.db.execute("DELETE FROM component_outbox WHERE job=?", (job["job_id"],))

    def cancel(self, principal: Principal, job_id: str) -> dict[str, Any]:
        job = self.job(principal, job_id, write=True)
        if job["status"] in TERMINAL:
            return self.public_job(job)
        if job["status"] == "waiting_for_component":
            job.update(cancel_requested=True, status="cancelled", result="cancelled_before_component_connected")
            self._save_job(job)
            return self.public_job(job)
        if job["last_phase"] in {"activating", "verifying"}:
            raise OTAError("CANCEL_TOO_LATE", "activation has begun; boot verification must finish")
        job.update(cancel_requested=True, status="cancel_requested")
        self._save_job(job)
        self.db.execute("UPDATE component_outbox SET sent=0 WHERE job=?", (job_id,))
        self.db.commit()
        return self.public_job(job)

    def download(self, firmware: str, job_id: str, token: str) -> Path:
        row = self._one("component_jobs", job_id)
        job = json.loads(row["snapshot"])
        if row["firmware"] != firmware or row["status"] in TERMINAL or not hmac.compare_digest(job["download_token"], token):
            raise OTAError("FORBIDDEN", "download credential is not valid for this job", 403)
        return self.directory / f'{job["target"]["sha256"]}.bin'

    @staticmethod
    def _supported_image(data: bytes) -> bool:
        return data.startswith((b"GIF87a", b"GIF89a", b"\x89PNG\r\n\x1a\n",
                                b"\xff\xd8\xff")) or (
            data.startswith(b"RIFF") and data[8:12] == b"WEBP"
        )

    def _normalize_component_gif(self, data: bytes, width: int, height: int) -> tuple[str, Path, int, str]:
        if len(data) > 16 * 1024 * 1024 or not self._supported_image(data):
            raise OTAError("INVALID_GIF", "uploaded file is not a supported image", 415)
        if not Path(self.ffmpeg_path).is_file():
            raise OTAError("FFMPEG_UNAVAILABLE", "FFmpeg is not installed", 503)
        asset_id = str(uuid4())
        self.asset_directory.mkdir(parents=True, exist_ok=True)
        source = self.asset_directory / f".{asset_id}.input"
        final = self.asset_directory / f"{asset_id}.gif"
        # The ESP32 file updater needs room for the old asset, the temporary
        # download, and the committed replacement at the same time. Try the
        # highest quality first, then reduce frame rate and palette size until
        # the normalized asset fits the 512 KiB transfer limit.
        encodings = (
            (12.5, 256),
            (10, 192),
            (7.5, 128),
            (5, 96),
            (4, 64),
            (3, 96),
            (2.5, 96),
            (2, 64),
        )
        selected: Path | None = None
        selected_size = 0
        try:
            source.write_bytes(data)
            last_detail = "unknown FFmpeg error"
            for attempt, (fps, colors) in enumerate(encodings):
                candidate = self.asset_directory / f".{asset_id}.normalized.{attempt}.gif"
                filter_graph = (
                    f"fps={fps},format=rgba,"
                    f"scale={width}:{height}:force_original_aspect_ratio=decrease:flags=lanczos,"
                    f"pad={width}:{height}:(ow-iw)/2:(oh-ih)/2:color=black@0,"
                    f"split[s0][s1];[s0]palettegen=max_colors={colors}:reserve_transparent=1[p];"
                    "[s1][p]paletteuse=dither=sierra2_4a:alpha_threshold=128"
                )
                try:
                    result = subprocess.run(
                        [self.ffmpeg_path, "-hide_banner", "-loglevel", "error", "-y", "-i",
                         str(source), "-filter_complex", filter_graph,
                         # Full frames retain the transparent color index.
                         # The device resolves transparent pixels against its
                         # own display background when rendering RGB565.
                         "-gifflags", "-offsetting-transdiff", "-f", "gif", str(candidate)],
                        capture_output=True, text=True, timeout=60, check=False)
                except subprocess.TimeoutExpired as exc:
                    candidate.unlink(missing_ok=True)
                    raise OTAError("INVALID_GIF", "GIF conversion timed out", 400) from exc
                if result.returncode or not candidate.is_file() or candidate.stat().st_size < 13:
                    last_detail = result.stderr.strip()[-240:] if result.stderr else last_detail
                    candidate.unlink(missing_ok=True)
                    continue
                output = candidate.read_bytes()
                valid_dimensions = (
                    output[:6] in (b"GIF87a", b"GIF89a") and
                    int.from_bytes(output[6:8], "little") == width and
                    int.from_bytes(output[8:10], "little") == height
                )
                if valid_dimensions and len(output) <= 512 * 1024:
                    selected = candidate
                    selected_size = len(output)
                    break
                last_detail = f"normalized GIF is {len(output)} bytes"
                candidate.unlink(missing_ok=True)
            if selected is None:
                raise OTAError("INVALID_GIF", f"could not fit normalized GIF in 512 KiB: {last_detail}", 400)
            os.replace(selected, final)
            fd = os.open(self.asset_directory, os.O_DIRECTORY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
            output = final.read_bytes()
            return asset_id, final, selected_size, hashlib.sha256(output).hexdigest()
        finally:
            source.unlink(missing_ok=True)
            if selected is not None:
                selected.unlink(missing_ok=True)
            for candidate in self.asset_directory.glob(f".{asset_id}.normalized.*.gif"):
                candidate.unlink(missing_ok=True)

    async def start_file_job(self, principal: Principal, device: str, component: str,
                             file_name: str, data: bytes, idem: str) -> dict[str, Any]:
        principal.require("component:ota:write", device)
        if component != "component-1":
            raise OTAError("UNKNOWN_COMPONENT", "only component-1 is configured", 404)
        if not idem or len(idem) > 128:
            raise OTAError("INVALID_ARGUMENT", "idempotency_key must contain 1..128 characters", 400)
        input_sha = hashlib.sha256(data).hexdigest()
        fingerprint = json.dumps([input_sha])
        existing = self.db.execute(
            "SELECT * FROM component_jobs WHERE owner=? AND device=? AND component=? AND idem=?",
            (principal.name, device, component, idem)).fetchone()
        if existing:
            if existing["fingerprint"] != fingerprint:
                raise OTAError("IDEMPOTENCY_CONFLICT", "idempotency key is bound to another request")
            return self.public_job(json.loads(existing["snapshot"]))
        before = await self.component(principal, device, component)
        profile = before["profile"]
        assets = COMPONENT_ASSETS.get(profile, {})
        # "auto" is the browser-friendly default; every supported profile has
        # one canonical display slot.
        # The uploaded filename is user metadata only. The device has one
        # stable runtime slot, regardless of the original name or extension.
        resolved_name = "display.gif"
        dimensions = assets.get(resolved_name)
        if not dimensions:
            raise OTAError("UNSUPPORTED_ASSET", f"{profile} does not support display assets", 409)
        asset_id, path, size, sha = await asyncio.to_thread(
            self._normalize_component_gif, data, *dimensions)
        gallery = self.asset_directory / "module-images" / asset_id
        try:
            await asyncio.to_thread(prepare_module_image, self, gallery.parent, data,
                                    image_name(file_name or "Web GIF.gif"), asset_id,
                                    (*dimensions, path))
            job_id = str(uuid4())
            now = time.time()
            job = {"job_id": job_id, "device_id": device, "component_id": component,
                   "kind": "file", "file_name": resolved_name, "asset_id": asset_id,
                   "source_name": image_name(file_name or "Web GIF.gif"),
                   "gallery_asset_id": asset_id, "gallery_status": "pending",
                   "target": {"size_bytes": size, "sha256": sha,
                              "width": dimensions[0], "height": dimensions[1]},
                   "before": before,
                   "status": "preparing", "last_phase": "preparing", "event_seq": 0,
                   "bytes_written": 0, "total_bytes": size, "phase_percent": 0,
                   "last_event_at": now, "created_at": now, "cancel_requested": False,
                   "result": None, "download_token": secrets.token_urlsafe(32)}
            with self.db:
                existing = self.db.execute(
                    "SELECT * FROM component_jobs WHERE owner=? AND device=? AND component=? AND idem=?",
                    (principal.name, device, component, idem)).fetchone()
                if existing:
                    if existing["fingerprint"] != fingerprint:
                        raise OTAError("IDEMPOTENCY_CONFLICT", "idempotency key is bound to another request")
                    Path(path).unlink(missing_ok=True)
                    shutil.rmtree(gallery, ignore_errors=True)
                    return self.public_job(json.loads(existing["snapshot"]))
                self.db.execute("INSERT INTO component_assets VALUES(?,?,?,?,?,?,?)",
                                (asset_id, principal.name, resolved_name, size, sha, str(path), now))
                self.db.execute("INSERT INTO component_jobs VALUES(?,?,?,?,?,?,?,?,?)",
                                (job_id, principal.name, device, component, asset_id, idem,
                                 fingerprint, job["status"], json.dumps(job)))
                self.db.execute("INSERT INTO component_outbox(job) VALUES(?)", (job_id,))
            return self.public_job(job)
        except sqlite3.IntegrityError as exc:
            Path(path).unlink(missing_ok=True)
            shutil.rmtree(gallery, ignore_errors=True)
            raise OTAError("COMPONENT_BUSY", "component has an active or reconciling job") from exc
        except Exception:
            Path(path).unlink(missing_ok=True)
            shutil.rmtree(gallery, ignore_errors=True)
            raise

    async def restore_file_gallery(self, principal: Principal, job_id: str) -> dict[str, Any]:
        job = self.job(principal, job_id, write=True)
        if job.get("kind") != "file" or job["status"] != "succeeded":
            raise OTAError("NOT_READY", "only successful display uploads can be added to the gallery")
        if job.get("gallery_asset_id"):
            return self.public_job(job)
        asset = self._one("component_assets", job["asset_id"])
        path = Path(asset["path"])
        data = await asyncio.to_thread(path.read_bytes)
        target = job["target"]
        if len(data) != target["size_bytes"] or hashlib.sha256(data).hexdigest() != target["sha256"]:
            raise OTAError("HASH_MISMATCH", "saved display asset does not match the successful upload", 400)
        await asyncio.to_thread(prepare_module_image, self, self.asset_directory / "module-images",
                                data, job.get("source_name", f'Web-GIF-{job_id[:8]}.gif'),
                                job["asset_id"], (target["width"], target["height"], path))
        # An event may have arrived while the bundle was being prepared.
        job = self.job(principal, job_id, write=True)
        job.update(gallery_asset_id=job["asset_id"], gallery_status="pending")
        self._save_job(job)
        return self.public_job(job)

    async def _sync_gallery(self, job: dict[str, Any]) -> None:
        if not self.command_service:
            return
        command_id = job.get("gallery_command_id")
        command = await self.command_service.command(UUID(command_id)) if command_id else None
        if command and command["status"] == "completed":
            job.update(gallery_status="completed", gallery_error=None)
            self._save_job(job)
            return
        if time.time() - job.get("gallery_sent_at", 0) < 5:
            return
        job["gallery_sent_at"] = time.time()
        if command and command["status"] in {"pending", "delivered"}:
            self._save_job(job)
            await self.send(job["device_id"], "modules.image",
                            cast(dict[str, Any], command["payload"]), str(command_id))
            return
        attempt = job.get("gallery_attempt", 0) + 1
        job.update(gallery_attempt=attempt, gallery_status="pending")
        self._save_job(job)
        asset_id = job["gallery_asset_id"]
        # Only the newest direct update for this profile may change the current
        # selection. Older uploads still belong in the reusable gallery.
        newer = self.db.execute(
            "SELECT 1 FROM component_jobs WHERE device=? AND status='succeeded' "
            "AND json_extract(snapshot,'$.kind')='file' "
            "AND json_extract(snapshot,'$.before.profile')=? "
            "AND json_extract(snapshot,'$.created_at')>? LIMIT 1",
            (job["device_id"], job["before"]["profile"], job["created_at"])).fetchone()
        display_profile = 0 if newer else {"round_screen": 1, "holographic_screen": 2}.get(job["before"]["profile"], 0)
        command = await self.command_service.send(
            job["device_id"], "modules.image",
            {"asset_id": asset_id, "asset_path": f"/v1/device/module-images/{asset_id}",
             "display_profile": display_profile},
            f'component-gallery-{job["job_id"]}-{attempt}',
        )
        job.update(gallery_command_id=command["command_id"], gallery_status="delivered", gallery_error=None)
        self._save_job(job)

    def download_asset(self, asset_id: str, job_id: str, token: str) -> Path:
        row = self._one("component_jobs", job_id)
        job = json.loads(row["snapshot"])
        if job.get("kind") != "file" or job.get("asset_id") != asset_id or row["status"] in TERMINAL or \
           not hmac.compare_digest(job["download_token"], token):
            raise OTAError("FORBIDDEN", "download credential is not valid for this job", 403)
        asset = self._one("component_assets", asset_id)
        path = Path(asset["path"])
        if not path.is_file():
            raise OTAError("NOT_FOUND", "component asset is not available", 404)
        return path

    async def event(self, device: str, event: dict[str, Any]) -> None:
        request_id, status = event["request_id"], event["status"]
        if status == "probe":
            pending = self._probes.get(request_id)
            component = event.get("component_id", "component-1")
            if component not in {"component-1", "slave-1"}:
                raise OTAError("UNKNOWN_COMPONENT", "unknown component", 404)
            if pending and (pending[0] != device or component != pending[2]):
                return
            if event.get("info"):
                self._snapshot(device, event["info"], component)
                self._start_waiting_jobs(device, component, event["info"])
            else:
                self.db.execute("DELETE FROM component_snapshots WHERE device=? AND component=?", (device, component))
                self.db.commit()
            if pending and not pending[1].done():
                if event.get("info"):
                    pending[1].set_result(event["info"])
                else:
                    pending[1].set_exception(OTAError("PROBE_FAILED", event.get("message", "probe failed"), 502))
            return
        row = self.db.execute("SELECT * FROM component_jobs WHERE id=?", (request_id,)).fetchone()
        if row is None:
            _LOG.debug("ignoring result for unknown component job device=%s request=%s", device, request_id)
            return
        job = json.loads(row["snapshot"])
        if row["device"] != device or event["component_id"] != row["component"]:
            raise OTAError("FORBIDDEN", "event belongs to another device", 403)
        if job["status"] in TERMINAL or event["event_seq"] <= job["event_seq"]:
            return
        if status not in PHASES or event["total_bytes"] != job["total_bytes"] or not 0 <= event["bytes_written"] <= job["total_bytes"]:
            raise OTAError("INVALID_EVENT", "invalid OTA phase or byte count", 400)
        if job.get("kind") != "file" and job["last_phase"] in {"activating", "verifying"} and status in {
            "preparing", "downloading", "transferring", "validating", "cancelled"
        }:
            raise OTAError("INVALID_EVENT", "activation barrier cannot be reversed", 400)
        info = event.get("info")
        if info:
            self._snapshot(device, info, row["component"])
        if status == "succeeded" and job.get("kind") != "file":
            if not info or not info["healthy"] or info["boot_id"] == job["before"]["boot_id"] or any(
                info[key] != job["target"][key] for key in ("profile", "project", "version", "elf_sha256")
            ):
                status = "reconciling"
                event["message"] = "healthy target boot has not been proven"
        job.update(status=status, event_seq=event["event_seq"], bytes_written=event["bytes_written"],
                   phase_percent=event["bytes_written"] * 100 // job["total_bytes"],
                   last_event_at=time.time(), result=event.get("message") or None)
        if status != "reconciling":
            job["last_phase"] = status
        self._save_job(job)

    def _snapshot(self, device: str, info: dict[str, Any], component: str = "component-1") -> None:
        if (component == "slave-1") != (info["profile"] == "slave_mcu"):
            raise OTAError("WRONG_PROFILE", "snapshot profile does not match component")
        self.db.execute("INSERT OR REPLACE INTO component_snapshots VALUES(?,?,?,?)",
                        (device, component, json.dumps(info), time.time()))
        self.db.commit()

    def start(self) -> None:
        self._task = asyncio.create_task(self._dispatch())

    async def _dispatch(self) -> None:
        while True:
            waiting = self.db.execute("SELECT snapshot FROM component_jobs WHERE status='waiting_for_component'").fetchall()
            for row in waiting:
                job = json.loads(row["snapshot"])
                if job["job_id"] in self._waiting_probes or time.time() - job.get("last_probe_at", 0) < 5:
                    continue
                job["last_probe_at"] = time.time()
                self._save_job(job)
                self._waiting_probes[job["job_id"]] = asyncio.create_task(self._probe_waiting_job(job["job_id"]))
            rows = list(self.db.execute("SELECT j.snapshot,o.sent FROM component_outbox o JOIN component_jobs j ON j.id=o.job"))
            for row in rows:
                job = json.loads(row["snapshot"])
                if time.time() - job["last_event_at"] > 30 and job["status"] != "reconciling":
                    job["status"] = "reconciling"
                    self._save_job(job)
                if time.time() - row["sent"] < 5:
                    continue
                target = job["target"]
                if job.get("kind") == "file":
                    payload = {"job_id": job["job_id"], "component_id": job["component_id"],
                               "file_name": job["file_name"], "size_bytes": target["size_bytes"],
                               "sha256": target["sha256"], "cancel_requested": job["cancel_requested"],
                               "asset_path": f'/v1/device/component-assets/{job["asset_id"]}?job_id={job["job_id"]}&token={job["download_token"]}'}
                    command_name = "component.file"
                else:
                    payload = {"job_id": job["job_id"], "component_id": job["component_id"],
                               "firmware_id": job["firmware_id"], **target,
                               "before_boot_id": job["before"]["boot_id"],
                               "before_version": job["before"]["version"],
                               "before_elf_sha256": job["before"]["elf_sha256"],
                               "allow_downgrade": int(target["version"]) < int(job["before"]["version"]),
                               "cancel_requested": job["cancel_requested"],
                               "reconcile_only": job["last_phase"] in {"activating", "verifying"},
                               "asset_path": f'/v1/device/firmware/{job["firmware_id"]}?job_id={job["job_id"]}&token={job["download_token"]}'}
                    command_name = "component.ota"
                try:
                    await self.send(job["device_id"], command_name, payload, job["job_id"])
                    self.db.execute("UPDATE component_outbox SET sent=? WHERE job=?", (time.time(), job["job_id"]))
                    self.db.commit()
                except Exception:
                    pass  # Offline jobs remain in the durable outbox for reconnect.
            galleries = self.db.execute(
                "SELECT snapshot FROM component_jobs WHERE status='succeeded' "
                "AND json_extract(snapshot,'$.gallery_status') IN ('pending','delivered') "
                "ORDER BY json_extract(snapshot,'$.created_at')"
            ).fetchall()
            for row in galleries:
                job = json.loads(row["snapshot"])
                try:
                    await self._sync_gallery(job)
                except Exception as exc:
                    job["gallery_error"] = str(exc)
                    self._save_job(job)
                    _LOG.warning("gallery sync pending for job %s: %s", job["job_id"], exc)
            await asyncio.sleep(1)

    async def close(self) -> None:
        if self._task:
            self._task.cancel()
            await asyncio.gather(self._task, return_exceptions=True)
        probes = list(self._waiting_probes.values())
        for task in probes:
            task.cancel()
        await asyncio.gather(*probes, return_exceptions=True)
        self.db.close()


def register_component_queries(tools: Any, service: ComponentOTAService) -> None:
    from binbot_server.application.tools import ToolContext, ToolSpec, ToolError

    def device_principal(context: ToolContext) -> Principal:
        if not context.device_id:
            raise ToolError("select a device first")
        return Principal(context.source, frozenset({"device:read"}), frozenset({context.device_id}))

    async def get(context: ToolContext, args: dict[str, Any]) -> dict[str, Any]:
        if set(args) - {"fresh"} or ("fresh" in args and not isinstance(args["fresh"], bool)):
            raise ToolError("only a boolean fresh argument is accepted")
        p = device_principal(context)
        return await service.component(p, context.device_id or "", "component-1", args.get("fresh", True))

    async def job(context: ToolContext, args: dict[str, Any]) -> dict[str, Any]:
        if set(args) != {"job_id"} or not isinstance(args["job_id"], str):
            raise ToolError("job_id is required")
        return service.public_job(service.job(device_principal(context), args["job_id"]))

    tools.register(ToolSpec("component.get", "Read the selected device's UART component.",
                            {"type": "object", "properties": {"fresh": {"type": "boolean"}},
                             "additionalProperties": False}, "device.read", get))
    tools.register(ToolSpec("component.ota.get", "Read a component OTA job for the selected device.",
                            {"type": "object", "properties": {"job_id": {"type": "string"}},
                             "required": ["job_id"], "additionalProperties": False}, "device.read", job))
