#!/usr/bin/env python3
"""Local OTA server with a persistent TCP control channel."""

import argparse
import hashlib
import json
import os
import socketserver
import struct
import threading
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, quote, unquote, urlparse


MAX_FILE_SIZE = 512 * 1024
DEVICE_OPERATION_TIMEOUT = 90


def valid_file_name(name):
    return (isinstance(name, str) and 1 <= len(name) <= 63 and
            name.isascii() and name[0] != "." and name not in (".", "..") and
            all(character not in name for character in "/\\:\t\r\n "))


def read_esp_app_version(image):
    """Read esp_app_desc_t.version from an ESP-IDF application image."""
    magic = 0xABCD5432
    descriptor_size = 256
    for offset in range(0, len(image) - descriptor_size + 1, 4):
        if struct.unpack_from("<I", image, offset)[0] != magic:
            continue
        raw_version = image[offset + 16:offset + 48]
        raw_project = image[offset + 48:offset + 80]
        version = raw_version.split(b"\0", 1)[0]
        project = raw_project.split(b"\0", 1)[0]
        try:
            decoded_version = version.decode("ascii")
            project.decode("ascii")
        except UnicodeDecodeError:
            continue
        if decoded_version:
            return decoded_version
    return ""


class DeviceOperation:
    def __init__(self, request_id, kind):
        self.request_id = request_id
        self.kind = kind
        self.event = threading.Event()
        self.result = None
        self.entries = []

    def finish(self, result):
        if not self.event.is_set():
            self.result = result
            self.event.set()


class DeviceDownload:
    def __init__(self, path):
        self.path = path
        self.size = 0
        self.sha256 = ""
        self.complete = False
        self.error = ""
        self.lock = threading.Lock()


class ConnectedDevice:
    def __init__(self, device_id, version, profile, address, connection):
        self.device_id = device_id
        self.version = version
        self.profile = profile
        self.address = address
        self.connection = connection
        self.lock = threading.Lock()
        self.operation_lock = threading.Lock()
        self.pending_lock = threading.Lock()
        self.pending = {}

    def send_line(self, line):
        payload = (line.rstrip("\r\n") + "\n").encode("ascii")
        with self.lock:
            self.connection.sendall(payload)

    def _get_pending(self, request_id):
        with self.pending_lock:
            return self.pending.get(request_id)

    def handle_line(self, line):
        fields = line.split()
        if not fields:
            return True
        command = fields[0]
        if command == "FS_LIST_BEGIN" and len(fields) == 2:
            operation = self._get_pending(fields[1])
            if operation is not None and operation.kind == "list":
                operation.entries = []
                return True
        elif command == "FS_FILE" and len(fields) == 4:
            operation = self._get_pending(fields[1])
            if operation is not None and operation.kind == "list":
                try:
                    size = int(fields[3], 10)
                except ValueError:
                    operation.finish({"ok": False, "error": "invalid-size"})
                    return True
                if not valid_file_name(fields[2]) or size < 0:
                    operation.finish({"ok": False, "error": "invalid-file"})
                    return True
                operation.entries.append({"name": fields[2], "size": size})
                return True
        elif command == "FS_LIST_END" and len(fields) == 2:
            operation = self._get_pending(fields[1])
            if operation is not None and operation.kind == "list":
                operation.finish({"ok": True, "files": operation.entries})
                return True
        elif command in ("FS_UPDATED", "FS_DELETED", "FS_READ"):
            expected_fields = 4
            if len(fields) == expected_fields:
                operation = self._get_pending(fields[1])
                if operation is not None:
                    try:
                        size = int(fields[3], 10)
                    except ValueError:
                        operation.finish({"ok": False, "error": "invalid-size"})
                        return True
                    operation.finish({
                        "ok": True,
                        "name": fields[2],
                        "size": size,
                    })
                    return True
        elif command == "FS_ERROR" and len(fields) >= 3:
            operation = self._get_pending(fields[1])
            if operation is not None:
                operation.finish({"ok": False, "error": fields[2]})
                return True
        elif command in ("FILE_UPDATED", "UPDATE_ACCEPTED", "NO_UPDATE"):
            return True
        return False

    def fail_pending(self, error="device-disconnected"):
        with self.pending_lock:
            operations = list(self.pending.values())
        for operation in operations:
            operation.finish({"ok": False, "error": error})

    def request(self, request_id, command, kind,
                timeout=DEVICE_OPERATION_TIMEOUT):
        operation = DeviceOperation(request_id, kind)
        with self.operation_lock:
            with self.pending_lock:
                self.pending[request_id] = operation
            try:
                self.send_line(command)
            except OSError:
                with self.pending_lock:
                    self.pending.pop(request_id, None)
                return {"ok": False, "error": "send-failed"}

            if not operation.event.wait(timeout):
                result = {"ok": False, "error": "device-timeout"}
            else:
                result = operation.result or {
                    "ok": False, "error": "empty-device-response"
                }
            with self.pending_lock:
                self.pending.pop(request_id, None)
            return result


class DeviceRegistry:
    def __init__(self):
        self._devices = {}
        self._lock = threading.Lock()

    def register(self, device):
        with self._lock:
            previous = self._devices.get(device.device_id)
            self._devices[device.device_id] = device
        if previous is not None and previous.connection is not device.connection:
            try:
                previous.connection.shutdown(2)
                previous.connection.close()
            except OSError:
                pass
        return previous

    def unregister(self, device):
        with self._lock:
            if self._devices.get(device.device_id) is device:
                del self._devices[device.device_id]

    def snapshot(self):
        with self._lock:
            return [
                {
                    "device": device.device_id,
                    "version": device.version,
                    "profile": device.profile,
                    "address": device.address[0],
                    "port": device.address[1],
                }
                for device in self._devices.values()
            ]

    def find_one(self, device_id=None, profile=None):
        with self._lock:
            devices = list(self._devices.values())
        if device_id:
            devices = [device for device in devices
                       if device.device_id == device_id]
        elif profile:
            devices = [device for device in devices
                       if device.profile == profile]
        if not devices:
            raise LookupError("no matching device is connected")
        if len(devices) != 1:
            raise LookupError("multiple devices match; specify device")
        return devices[0]

    @staticmethod
    def new_request_id():
        return uuid.uuid4().hex[:12]

    def trigger(self, version, firmware_url, device_id=None, profile=None):
        command = "UPDATE {} {}".format(version, firmware_url)
        with self._lock:
            targets = list(self._devices.values())
        if device_id:
            targets = [device for device in targets
                       if device.device_id == device_id]
        if profile:
            targets = [device for device in targets
                       if device.profile == profile]

        sent = []
        failed = []
        for device in targets:
            try:
                device.send_line(command)
                sent.append(device.device_id)
            except OSError:
                failed.append(device.device_id)
                self.unregister(device)
        return sent, failed

    def trigger_file(self, name, file_url, size, sha256, device_id=None,
                     profile=None):
        command = "FILE_UPDATE {} {} {} {}".format(
            name, size, sha256, file_url)
        with self._lock:
            targets = list(self._devices.values())
        if device_id:
            targets = [device for device in targets
                       if device.device_id == device_id]
        if profile:
            targets = [device for device in targets
                       if device.profile == profile]

        sent = []
        failed = []
        for device in targets:
            try:
                device.send_line(command)
                sent.append(device.device_id)
            except OSError:
                failed.append(device.device_id)
                self.unregister(device)
        return sent, failed


class DeviceControlHandler(socketserver.StreamRequestHandler):
    registry = None

    def handle(self):
        device = None
        try:
            raw_hello = self.rfile.readline(256)
            if not raw_hello:
                return
            hello = raw_hello.decode("ascii", "replace").strip()
            fields = hello.split()
            if len(fields) not in (3, 4) or fields[0] != "HELLO":
                self.wfile.write(b"ERROR invalid-hello\n")
                return

            profile = fields[3] if len(fields) == 4 else ""
            device = ConnectedDevice(fields[1], fields[2], profile,
                                     self.client_address, self.request)
            self.registry.register(device)
            device.send_line("OK")
            print("device connected: {} version={} profile={} address={}:{}".format(
                device.device_id, device.version, device.profile or "unknown",
                self.client_address[0], self.client_address[1]), flush=True)

            while True:
                raw_line = self.rfile.readline(256)
                if not raw_line:
                    break
                line = raw_line.decode("ascii", "replace").strip()
                if line == "PING":
                    device.send_line("PONG")
                elif line:
                    if not device.handle_line(line):
                        print("device {}: {}".format(device.device_id, line),
                              flush=True)
        except (ConnectionError, OSError):
            pass
        finally:
            if device is not None:
                device.fail_pending()
                self.registry.unregister(device)
                print("device disconnected: {}".format(device.device_id),
                      flush=True)


class ThreadingControlServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


class OtaHandler(BaseHTTPRequestHandler):
    firmware_path = ""
    firmware_name = "binbot_app_profiles.bin"
    manifest_name = "manifest.json"
    firmware_version = ""
    public_base_url = ""
    firmware_size = 0
    firmware_sha256 = ""
    control_port = 8001
    profile = ""
    serve_once = False
    served = False
    state_lock = threading.Lock()
    registry = None
    file_assets = {}
    asset_lock = threading.Lock()
    storage_stage_dir = ""
    download_jobs = {}
    download_lock = threading.Lock()

    def _select_device(self, query):
        device_id = query.get("device", [None])[0] or None
        profile = query.get("profile", [None])[0] or self.profile or None
        return self.registry.find_one(device_id, None if device_id else profile)

    def _base_url(self):
        base_url = self.public_base_url.rstrip("/")
        if not base_url:
            base_url = "http://{}".format(self.headers.get(
                "Host", "127.0.0.1"))
        return base_url

    @classmethod
    def _register_download(cls, request_id, job):
        with cls.download_lock:
            cls.download_jobs[request_id] = job

    @classmethod
    def _get_download(cls, request_id):
        with cls.download_lock:
            return cls.download_jobs.get(request_id)

    @classmethod
    def _remove_download(cls, request_id):
        with cls.download_lock:
            return cls.download_jobs.pop(request_id, None)

    def _send_file(self, path, name):
        size = os.path.getsize(path)
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(size))
        self.send_header("Content-Disposition",
                         'attachment; filename="{}"'.format(name))
        self.send_header("Connection", "close")
        self.end_headers()
        with open(path, "rb") as source:
            while True:
                chunk = source.read(16 * 1024)
                if not chunk:
                    break
                self.wfile.write(chunk)

    def _receive_body(self, path):
        try:
            length = int(self.headers.get("Content-Length", "-1"))
        except ValueError as error:
            raise ValueError("invalid Content-Length") from error
        if length < 0:
            raise ValueError("Content-Length is required")
        if length > MAX_FILE_SIZE:
            raise ValueError("file is too large")

        digest = hashlib.sha256()
        received = 0
        with open(path, "wb") as target:
            while received < length:
                chunk = self.rfile.read(min(16 * 1024, length - received))
                if not chunk:
                    raise ValueError("request body ended early")
                target.write(chunk)
                digest.update(chunk)
                received += len(chunk)
        return received, digest.hexdigest()

    def _send_operation_error(self, result):
        self._send_json(result, status=502)

    def do_GET(self):  # noqa: N802 - required by BaseHTTPRequestHandler
        parsed = urlparse(self.path)
        path = parsed.path
        query = parse_qs(parsed.query, keep_blank_values=True)
        if path == "/health":
            self._send_json({
                "ok": True,
                "version": self.firmware_version,
                "firmware": self.firmware_name,
                "control_port": self.control_port,
                "profile": self.profile,
                "devices": len(self.registry.snapshot()),
            })
            return

        if path == "/devices":
            self._send_json({"devices": self.registry.snapshot()})
            return

        if path in ("/fs", "/fs/"):
            try:
                device = self._select_device(query)
                request_id = self.registry.new_request_id()
                result = device.request(request_id,
                                        "FS_LIST {}".format(request_id),
                                        "list")
                if not result.get("ok"):
                    self._send_operation_error({
                        "ok": False,
                        "device": device.device_id,
                        "error": result.get("error", "device-error"),
                    })
                    return
                self._send_json({
                    "ok": True,
                    "device": device.device_id,
                    "files": result.get("files", []),
                })
            except (LookupError, ValueError) as error:
                self._send_json({"ok": False, "error": str(error)}, status=400)
            return

        if path.startswith("/fs/read/"):
            name = unquote(path[len("/fs/read/"):])
            if not valid_file_name(name):
                self._send_json({"ok": False, "error": "invalid file name"},
                                status=400)
                return
            request_id = self.registry.new_request_id()
            job_path = os.path.join(self.storage_stage_dir,
                                    request_id + ".download")
            job = DeviceDownload(job_path)
            try:
                device = self._select_device(query)
                self._register_download(request_id, job)
                callback_url = self._base_url() + "/device-upload/" + request_id
                result = device.request(
                    request_id,
                    "FS_GET {} {} {}".format(request_id, name, callback_url),
                    "read")
                if not result.get("ok"):
                    self._send_operation_error({
                        "ok": False,
                        "device": device.device_id,
                        "error": result.get("error", "device-error"),
                    })
                elif not job.complete or not os.path.isfile(job.path):
                    self._send_operation_error({
                        "ok": False,
                        "device": device.device_id,
                        "error": "device-upload-missing",
                    })
                elif result.get("size") != job.size:
                    self._send_operation_error({
                        "ok": False,
                        "device": device.device_id,
                        "error": "device-upload-size-mismatch",
                    })
                else:
                    self._send_file(job.path, name)
            except (LookupError, ValueError) as error:
                self._send_json({"ok": False, "error": str(error)}, status=400)
            except (BrokenPipeError, ConnectionResetError):
                pass
            finally:
                self._remove_download(request_id)
                try:
                    os.remove(job_path)
                except OSError:
                    pass
            return

        if path.startswith("/files/"):
            name = unquote(path[len("/files/"):])
            with self.asset_lock:
                asset = self.file_assets.get(name)
            if asset is None:
                self.send_error(404, "file not found")
                return
            try:
                with open(asset["path"], "rb") as source:
                    self.send_response(200)
                    self.send_header("Content-Type", "application/octet-stream")
                    self.send_header("Content-Length", str(asset["size"]))
                    self.send_header("Connection", "close")
                    self.end_headers()
                    while True:
                        chunk = source.read(16 * 1024)
                        if not chunk:
                            break
                        self.wfile.write(chunk)
                print("served file {} ({} bytes)".format(
                    name, asset["size"]), flush=True)
            except (BrokenPipeError, ConnectionResetError):
                print("client disconnected while downloading file {}".format(
                    name), flush=True)
            return

        if path == "/" + self.manifest_name:
            if not self.firmware_path:
                self.send_error(404, "firmware is not configured")
                return
            base_url = self._base_url()
            self._send_json({
                "version": self.firmware_version,
                "firmware_url": base_url + "/" + self.firmware_name,
                "size": self.firmware_size,
                "sha256": self.firmware_sha256,
            })
            return

        if path != "/" + self.firmware_name or not self.firmware_path:
            self.send_error(404, "not found")
            return

        with self.state_lock:
            if self.serve_once and self.served:
                self.send_error(404, "firmware already served")
                return
            self.served = True

        try:
            size = os.path.getsize(self.firmware_path)
            with open(self.firmware_path, "rb") as firmware:
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(size))
                self.send_header("Connection", "close")
                self.end_headers()
                while True:
                    chunk = firmware.read(16 * 1024)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
            print("served {} ({} bytes)".format(self.firmware_name, size),
                  flush=True)
        except (BrokenPipeError, ConnectionResetError):
            print("client disconnected while downloading firmware", flush=True)

    def do_POST(self):  # noqa: N802 - required by BaseHTTPRequestHandler
        path = urlparse(self.path).path
        if path.startswith("/device-upload/"):
            request_id = unquote(path[len("/device-upload/"):])
            job = self._get_download(request_id)
            if job is None:
                self.send_error(404, "download request not found")
                return
            temporary_path = job.path + ".part"
            try:
                size, digest = self._receive_body(temporary_path)
                os.replace(temporary_path, job.path)
                with job.lock:
                    job.size = size
                    job.sha256 = digest
                    job.complete = True
                self._send_json({"ok": True, "size": size,
                                 "sha256": digest})
            except (OSError, ValueError) as error:
                try:
                    os.remove(temporary_path)
                except OSError:
                    pass
                with job.lock:
                    job.error = str(error)
                    job.complete = False
                self._send_json({"ok": False, "error": str(error)}, status=400)
            return

        if path != "/trigger":
            self.send_error(404, "not found")
            return

        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length > 1024:
                raise ValueError("request too large")
            raw_body = self.rfile.read(length) if length else b"{}"
            request = json.loads(raw_body.decode("utf-8"))
            if not isinstance(request, dict):
                raise ValueError("request body must be a JSON object")
            requested_version = str(request.get("version") or
                                    self.firmware_version)
            device_id = request.get("device")

            file_name = request.get("file") or request.get("name")
            if file_name is not None:
                with self.asset_lock:
                    asset = self.file_assets.get(file_name)
                if not isinstance(file_name, str) or asset is None:
                    raise ValueError("unknown file asset")
                if not device_id and not self.profile:
                    raise ValueError(
                        "server was started without --profile; specify device or "
                        "restart with --profile")
                base_url = self._base_url()
                file_url = base_url + "/files/" + quote(file_name, safe="")
                sent, failed = self.registry.trigger_file(
                    file_name, file_url, asset["size"], asset["sha256"],
                    device_id, self.profile)
                self._send_json({
                    "file": file_name,
                    "size": asset["size"],
                    "sha256": asset["sha256"],
                    "file_url": file_url,
                    "profile": self.profile,
                    "sent": sent,
                    "failed": failed,
                })
                return

            if not device_id and not self.profile:
                raise ValueError(
                    "server was started without --profile; specify device or "
                    "restart with --profile")
            if not self.firmware_path:
                raise ValueError("firmware is not configured")
            base_url = self._base_url()
            firmware_url = base_url + "/" + self.firmware_name
            sent, failed = self.registry.trigger(
                requested_version, firmware_url, device_id, self.profile)
            self._send_json({
                "version": requested_version,
                "profile": self.profile,
                "firmware_url": firmware_url,
                "sent": sent,
                "failed": failed,
            })
        except (ValueError, TypeError, json.JSONDecodeError) as error:
            self.send_error(400, str(error))

    def do_PUT(self):  # noqa: N802 - required by BaseHTTPRequestHandler
        parsed = urlparse(self.path)
        path = parsed.path
        if not path.startswith("/fs/"):
            self.send_error(404, "not found")
            return

        name = unquote(path[len("/fs/"):])
        if not valid_file_name(name):
            self._send_json({"ok": False, "error": "invalid file name"},
                            status=400)
            return

        staged_path = ""
        asset_name = ""
        try:
            query = parse_qs(parsed.query, keep_blank_values=True)
            device = self._select_device(query)
            request_id = self.registry.new_request_id()
            staged_path = os.path.join(self.storage_stage_dir,
                                       request_id + ".upload")
            size, digest = self._receive_body(staged_path)
            asset_name = "__upload_" + request_id
            with self.asset_lock:
                self.file_assets[asset_name] = {
                    "path": staged_path,
                    "size": size,
                    "sha256": digest,
                }
            file_url = self._base_url() + "/files/" + quote(asset_name,
                                                             safe="")
            result = device.request(
                request_id,
                "FS_PUT {} {} {} {} {}".format(
                    request_id, name, size, digest, file_url),
                "write")
            response = {
                "ok": bool(result.get("ok")),
                "device": device.device_id,
                "file": name,
                "size": size,
                "sha256": digest,
            }
            if not result.get("ok"):
                response["error"] = result.get("error", "device-error")
                self._send_json(response, status=502)
            else:
                self._send_json(response)
        except (LookupError, OSError, ValueError) as error:
            self._send_json({"ok": False, "error": str(error)}, status=400)
        finally:
            if asset_name:
                with self.asset_lock:
                    self.file_assets.pop(asset_name, None)
            if staged_path:
                try:
                    os.remove(staged_path)
                except OSError:
                    pass

    def do_DELETE(self):  # noqa: N802 - required by BaseHTTPRequestHandler
        parsed = urlparse(self.path)
        path = parsed.path
        if not path.startswith("/fs/"):
            self.send_error(404, "not found")
            return
        name = unquote(path[len("/fs/"):])
        if not valid_file_name(name):
            self._send_json({"ok": False, "error": "invalid file name"},
                            status=400)
            return
        try:
            device = self._select_device(parse_qs(parsed.query,
                                                   keep_blank_values=True))
            request_id = self.registry.new_request_id()
            result = device.request(
                request_id,
                "FS_DELETE {} {}".format(request_id, name),
                "delete")
            response = {
                "ok": bool(result.get("ok")),
                "device": device.device_id,
                "file": name,
            }
            if not result.get("ok"):
                response["error"] = result.get("error", "device-error")
                self._send_json(response, status=502)
            else:
                self._send_json(response)
        except (LookupError, ValueError) as error:
            self._send_json({"ok": False, "error": str(error)}, status=400)

    def _send_json(self, value, status=200):
        body = json.dumps(value, separators=(",", ":")).encode("ascii") + b"\n"
        self._send_bytes(body, "application/json; charset=utf-8", status)

    def _send_bytes(self, body, content_type, status=200):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format_string, *args):
        print("{} - {}".format(self.address_string(),
                                format_string % args), flush=True)


def main():
    parser = argparse.ArgumentParser(
        description="Manage Binbot firmware and device files over HTTP/TCP")
    parser.add_argument("--firmware", default="",
                        help="optional path to the firmware .bin file")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8000,
                        help="HTTP firmware server port")
    parser.add_argument("--control-port", type=int, default=8001,
                        help="persistent TCP OTA control port")
    parser.add_argument("--version", default=None,
                        help="firmware version; defaults to the ESP image")
    parser.add_argument("--firmware-name", default="binbot_app_profiles.bin")
    parser.add_argument("--manifest-name", default="manifest.json")
    parser.add_argument("--public-base-url", default="",
                        help="URL advertised to devices, for example "
                             "http://ota.example.com:8000")
    parser.add_argument("--profile", default="",
                        help="firmware profile for safe broadcast triggers; "
                             "for example round_screen")
    parser.add_argument("--once", action="store_true",
                        help="serve the firmware only once")
    parser.add_argument("--asset", action="append", default=[],
                        metavar="NAME=PATH",
                        help="replaceable file asset; may be repeated")
    args = parser.parse_args()

    firmware_path = os.path.abspath(args.firmware) if args.firmware else ""
    firmware_data = b""
    firmware_size = 0
    firmware_version = args.version or ""
    if firmware_path:
        if not os.path.isfile(firmware_path):
            parser.error("firmware does not exist: {}".format(firmware_path))
        with open(firmware_path, "rb") as firmware:
            firmware_data = firmware.read()
        firmware_size = len(firmware_data)
        firmware_version = args.version or read_esp_app_version(firmware_data)
        if not firmware_version:
            parser.error("could not read ESP app version; pass --version explicitly")

    registry = DeviceRegistry()
    project_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    asset_paths = {}
    for default_name in ("display.gif",):
        default_path = os.path.join(project_root, "assets", default_name)
        if os.path.isfile(default_path):
            asset_paths[default_name] = default_path
    for asset_spec in args.asset:
        if "=" not in asset_spec:
            parser.error("--asset must use NAME=PATH")
        asset_name, asset_path = asset_spec.split("=", 1)
        if (not asset_name or os.path.basename(asset_name) != asset_name or
                "/" in asset_name or "\\" in asset_name):
            parser.error("asset name must be a plain file name")
        asset_paths[asset_name] = os.path.abspath(asset_path)

    file_assets = {}
    for asset_name, asset_path in asset_paths.items():
        if not os.path.isfile(asset_path):
            parser.error("asset does not exist: {}".format(asset_path))
        digest = hashlib.sha256()
        size = 0
        with open(asset_path, "rb") as asset_file:
            while True:
                chunk = asset_file.read(64 * 1024)
                if not chunk:
                    break
                digest.update(chunk)
                size += len(chunk)
        file_assets[asset_name] = {
            "path": asset_path,
            "size": size,
            "sha256": digest.hexdigest(),
        }
    OtaHandler.firmware_path = firmware_path
    OtaHandler.firmware_name = args.firmware_name
    OtaHandler.manifest_name = args.manifest_name
    OtaHandler.firmware_version = firmware_version
    OtaHandler.public_base_url = args.public_base_url
    OtaHandler.firmware_size = firmware_size
    OtaHandler.firmware_sha256 = hashlib.sha256(firmware_data).hexdigest()
    OtaHandler.control_port = args.control_port
    OtaHandler.profile = args.profile
    OtaHandler.serve_once = args.once
    OtaHandler.registry = registry
    OtaHandler.file_assets = file_assets
    OtaHandler.asset_lock = threading.Lock()
    OtaHandler.download_lock = threading.Lock()
    OtaHandler.download_jobs = {}
    OtaHandler.storage_stage_dir = os.path.join(project_root,
                                                ".ota_server_files")
    os.makedirs(OtaHandler.storage_stage_dir, exist_ok=True)
    DeviceControlHandler.registry = registry

    try:
        control_server = ThreadingControlServer(
            (args.host, args.control_port), DeviceControlHandler)
        http_server = ThreadingHTTPServer((args.host, args.port), OtaHandler)
    except OSError as error:
        parser.error("could not bind server port: {}".format(error))

    control_thread = threading.Thread(target=control_server.serve_forever,
                                      name="ota-control", daemon=True)
    control_thread.start()
    print("OTA HTTP server listening on http://{}:{}/".format(
        args.host, args.port), flush=True)
    print("OTA TCP control listening on {}:{}".format(
        args.host, args.control_port), flush=True)
    print("firmware: {}".format(firmware_path or "not configured"), flush=True)
    if firmware_version:
        print("version: {}".format(firmware_version), flush=True)
    print("profile: {}".format(args.profile or "unspecified"), flush=True)
    print("manifest: http://127.0.0.1:{}/{}".format(
        args.port, args.manifest_name), flush=True)
    print("trigger: POST http://127.0.0.1:{}/trigger".format(
        args.port), flush=True)
    try:
        http_server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        http_server.shutdown()
        http_server.server_close()
        control_server.shutdown()
        control_server.server_close()


if __name__ == "__main__":
    main()
