import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location(
    "install_control", Path(__file__).resolve().parents[1] / "device/install-control.py")
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class ProcessIdentityTest(unittest.TestCase):
    def test_exited_unreaped_process_counts_as_stopped(self):
        with tempfile.TemporaryDirectory() as directory:
            proc = subprocess.Popen([sys.executable, "-c", "pass"])
            try:
                os.waitid(os.P_PID, proc.pid, os.WEXITED | os.WNOWAIT)
                (Path(directory) / "binbot_audio.pid").write_text(str(proc.pid))
                with mock.patch.object(installer, "PIDS", Path(directory)):
                    self.assertIsNone(installer.process_identity("binbot_audio"))
            finally:
                proc.wait()

    def test_process_exiting_between_stat_and_exe_counts_as_stopped(self):
        with tempfile.TemporaryDirectory() as directory:
            (Path(directory) / "binbot_audio.pid").write_text(str(os.getpid()))
            with mock.patch.object(installer, "PIDS", Path(directory)), \
                    mock.patch.object(installer.os, "readlink", side_effect=FileNotFoundError):
                self.assertIsNone(installer.process_identity("binbot_audio"))

    def test_live_unrelated_process_is_still_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            (Path(directory) / "binbot_audio.pid").write_text(str(os.getpid()))
            with mock.patch.object(installer, "PIDS", Path(directory)):
                with self.assertRaisesRegex(RuntimeError, "PID file does not match"):
                    installer.process_identity("binbot_audio")


class ControlDeploymentTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.oem, self.stage = self.root / "oem", self.root / "stage"
        self.oem.mkdir()
        self.stage.mkdir()
        (self.oem / "data").mkdir()
        self.init = self.root / "init"
        self.init.write_text("original-init")
        self.old_app = self.root / "original-binbot_app"
        self.old_app.write_bytes(b"original-app")
        (self.oem / "binbot_app").symlink_to(self.old_app)
        (self.oem / "binbot_audio").write_bytes(b"original-audio")
        self.old_env = ("# device configuration\nexport BINBOT_DEVICE_ID='desk-one'\n"
                        "BINBOT_DEVICE_TOKEN='original-device-secret'\nBINBOT_DIALOG_URL='wss://voice.test/ws'\n"
                        "BINBOT_WAKE_ENGINE=hixiaowen\n")
        (self.oem / "binbot.env").write_text(self.old_env)
        binary = bytearray(64)
        binary[:6] = b"\x7fELF\x01\x01"
        binary[18:20] = (40).to_bytes(2, "little")
        files = {}
        for name in installer.BINARIES:
            data = bytes(binary) + name.encode()
            (self.stage / name).write_bytes(data)
            files[name] = hashlib.sha256(data).hexdigest()
        for name in ("init-binbot.sh", "install-control.py"):
            data = ("new-" + name).encode()
            (self.stage / name).write_bytes(data)
            files[name] = hashlib.sha256(data).hexdigest()
        (self.stage / "manifest.json").write_text(json.dumps({"files": files}))
        self.config = {"device_id": "desk-one", "token": "a" * 40, "mqtt_host": "192.168.1.2", "mqtt_port": 1883}
        patcher = mock.patch.multiple(installer, OEM=self.oem, INIT=self.init, PIDS=self.root / "pids")
        patcher.start()
        self.addCleanup(patcher.stop)
        for obj, name, kwargs in (
            (installer, "capture_environment", {"return_value": {"BINBOT_DEVICE_ID": "desk-one"}}),
            (installer, "stop", {}), (installer, "start_services", {}),
            (installer, "fingerprint", {"return_value": {"sha256": "original", "process": [100, 200]}}),
            (installer.os, "sync", {}), (installer.time, "sleep", {}),
            (installer, "process_identity", {"return_value": [100, 200]}),
            (Path, "is_socket", {"return_value": True}),
        ):
            patcher = mock.patch.object(obj, name, **kwargs)
            patcher.start()
            self.addCleanup(patcher.stop)

    def assert_original(self):
        self.assertTrue((self.oem / "binbot_app").is_symlink())
        self.assertEqual((self.oem / "binbot_app").read_bytes(), b"original-app")
        self.assertEqual((self.oem / "binbot_audio").read_bytes(), b"original-audio")
        self.assertFalse((self.oem / "binbot_control").exists())
        self.assertEqual((self.oem / "binbot.env").read_text(), self.old_env)
        self.assertEqual(self.init.read_text(), "original-init")

    def test_valid_install_preserves_configuration_and_rollback_restores_original_links(self):
        installer.activate(self.stage, self.config)
        text = (self.oem / "binbot.env").read_text()
        self.assertIn("BINBOT_WAKE_ENGINE=hixiaowen", text)
        self.assertIn("BINBOT_DIALOG_URL='wss://voice.test/ws'", text)
        self.assertEqual(text.count("BINBOT_DEVICE_TOKEN="), 1)
        self.assertEqual((self.oem / "binbot.env").stat().st_mode & 0o777, 0o600)
        self.assertEqual((self.stage / "backup/binbot.env").stat().st_mode & 0o777, 0o600)
        self.assertFalse((self.oem / "binbot_app").is_symlink())
        installer.rollback(self.stage)
        self.assert_original()

    def test_failed_service_start_rolls_back_binaries_and_private_configuration(self):
        installer.start_services.side_effect = [RuntimeError("simulated start failure"), None]
        with self.assertRaisesRegex(RuntimeError, "start failure"):
            installer.activate(self.stage, self.config)
        self.assert_original()

    def test_corrupt_binary_rejected_before_service_stop_or_backup(self):
        (self.stage / "binbot_audio").write_bytes(b"corrupt")
        with self.assertRaisesRegex(ValueError, "checksum"):
            installer.activate(self.stage, self.config)
        installer.stop.assert_not_called()
        self.assertFalse((self.stage / "backup").exists())
        self.assert_original()

    def test_wrong_device_identity_rejected_before_mutation(self):
        with self.assertRaisesRegex(ValueError, "identity"):
            installer.activate(self.stage, {**self.config, "device_id": "desk-other"})
        installer.stop.assert_not_called()
        self.assert_original()

    def test_changed_unrelated_service_triggers_rollback(self):
        installer.fingerprint.side_effect = [
            {"sha256": "old", "process": [1, 2]} for _ in installer.PRESERVE
        ] + [{"sha256": "changed", "process": [1, 2]}]
        with self.assertRaisesRegex(RuntimeError, "unrelated"):
            installer.activate(self.stage, self.config)
        self.assert_original()


if __name__ == "__main__":
    unittest.main()
