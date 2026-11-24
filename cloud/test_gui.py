"""Windows GUI integration checks. Run from the repository root with uv."""
import hashlib
import json
import os
from pathlib import Path
import secrets
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import ota_gui as gui


class GuiIntegration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.bin = Path(os.environ.get("OTA_TEST_BIN", ROOT / "app_f411/MDK-ARM/app_f411/app_f411.bin"))
        cls.data = cls.bin.read_bytes()
        cls.version = gui.firmware_version_from_bin(cls.data)
        folder = tempfile.TemporaryDirectory()
        cls.addClassCleanup(folder.cleanup)
        token = secrets.token_urlsafe(32)
        secret = Path(folder.name) / "token"
        secret.write_text(token, encoding="ascii")
        proc = subprocess.Popen(["node", str(ROOT / "cloud/service.mjs")], stdout=subprocess.PIPE,
                                text=True, env={**os.environ, "OTA_BIND": "127.0.0.1", "OTA_PORT": "0",
                                "OTA_TOKEN_FILE": str(secret), "OTA_DB_PATH": str(secret.with_suffix(".sqlite"))})
        def shutdown():
            proc.terminate()
            proc.wait(timeout=10)
            proc.stdout.close()
        cls.addClassCleanup(shutdown)
        port = int(proc.stdout.readline().split()[-1])
        base = f"http://127.0.0.1:{port}" + gui.API_PATH
        cls.url = base + "/latest"
        with gui.requests.Session() as session:
            session.trust_env = False
            session.headers["Authorization"] = "Bearer " + token
            admin = base + "/admin/releases"
            session.post(admin, params={"version": cls.version}, data=cls.data,
                         headers={"Content-Type": "application/octet-stream"}, timeout=5).raise_for_status()
            session.post(f"{admin}/{cls.version}/publish", timeout=5).raise_for_status()
            cls.info = session.get(cls.url, timeout=5).json()

    def setUp(self):
        root = gui.tk.Tk()
        root.withdraw()
        def close_window():
            for timer in root.tk.splitlist(root.tk.call("after", "info")):
                root.after_cancel(timer)
            root.destroy()
        self.addCleanup(close_window)
        guard = patch.object(gui.serial, "Serial", side_effect=AssertionError("hardware disabled"))
        self.addCleanup(guard.stop)
        guard.start()
        with patch.object(gui.list_ports, "comports", return_value=[]):
            self.app = gui.OTAGuiApp(root)
        self.assertEqual(self.app.api_url_var.get(), "https://47.107.50.1" + gui.API_PATH + "/latest")
        self.app.api_url_var.set(self.url)
        errors = patch.object(gui.messagebox, "showerror")
        self.error = errors.start()
        self.addCleanup(errors.stop)

    def check(self):
        self.app.btn_check.invoke()
        deadline = time.monotonic() + 8
        while self.app.checking_cloud and time.monotonic() < deadline:
            self.app.root.update()
            time.sleep(0.01)
        self.assertFalse(self.app.checking_cloud)

    def assert_dispatch(self):
        # Inspect only the handoff; never run the serial worker or open a port.
        with patch.object(self.app, "_require_serial", return_value=True), patch.object(gui.threading, "Thread") as thread:
            self.app._start_ota()
            self.assertEqual(thread.call_args.kwargs["target"], self.app._ota_worker)
            self.assertEqual(thread.call_args.kwargs["args"], (self.data, self.version))
            thread.return_value.start.assert_called_once()

    def test_real_service(self):
        self.check()
        self.assertEqual(self.app.firmware_data, self.data)
        self.app.device_version = self.version
        self.check()
        self.assertIsNone(self.app.firmware_data)
        self.app.device_version = None
        self.check()
        self.assert_dispatch()

    def test_numeric_comparison(self):
        self.assertTrue(gui.is_newer_version("1.0.10", "1.0.9"))
        self.app.device_version = "255.255.255"
        self.check()
        self.assertEqual(self.app.cloud_info["latest_version"], self.version)
        self.assertIsNone(self.app.firmware_data)

    @unittest.skipUnless(os.getenv("OTA_PUBLIC_CHECK") == "1", "opt-in public HTTPS check")
    def test_public_https(self):
        self.app.api_url_var.set(gui.DEFAULT_API_URL)
        self.app.device_version = "1.0.6"
        self.check()
        self.assertEqual(self.app.cloud_info["latest_version"], self.version)
        self.assertEqual(self.app.firmware_data, self.data)
        self.assertIsNone(self.app.ser)
        self.assert_dispatch()

    def test_changed_source_and_local_fallback(self):
        self.check()
        self.app.api_url_var.set("http://192.0.2.1" + gui.API_PATH + "/latest")
        self.assertIsNone(self.app.firmware_data)
        self.check()
        self.error.assert_called_once()
        self.app.local_path_var.set(str(self.bin))
        self.app.src_var.set("local")
        self.assert_dispatch()

    def test_device_version_changed_since_download(self):
        self.check()
        self.app.device_version = self.version
        with patch.object(self.app, "_require_serial") as serial_gate:
            self.app._start_ota()
            serial_gate.assert_not_called()
        self.assertIsNone(self.app.firmware_data)
        self.error.assert_called_once()

    def test_failed_refresh(self):
        owner = self
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                manifest = self.path.endswith("/latest")
                body = json.dumps(owner.manifest).encode() if manifest else owner.payload
                if manifest and owner.failure == "invalid_json":
                    body = b"{"
                if not manifest and owner.failure == "timeout":
                    self.send_response(200)
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    time.sleep(0.6)
                    return
                if owner.failure == ("manifest_redirect" if manifest else "bin_redirect"):
                    self.send_response(302)
                    self.send_header("Location", owner.url)
                elif owner.failure == "missing":
                    self.send_response(404)
                else:
                    self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body[:128] if not manifest and owner.failure == "interrupt" else body)

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        self.app.api_url_var.set(f"http://127.0.0.1:{server.server_port}" + gui.API_PATH + "/latest")
        for failure in ("sha256", "crc32", "size", "stack", "entry", "version", "target", "app_address",
                        "download_url", "manifest_redirect", "bin_redirect", "interrupt", "missing",
                        "timeout", "invalid_json", "damaged_bin", "version_mismatch"):
            with self.subTest(failure=failure):
                self.failure, self.manifest, self.payload = "", self.info.copy(), self.data
                self.check()
                self.assertEqual(self.app.firmware_data, self.data)
                self.failure = failure
                if failure in ("sha256", "crc32"):
                    self.manifest[failure] = "0" * len(self.info[failure])
                elif failure == "size":
                    self.manifest["size"] -= 4
                elif failure in ("stack", "entry", "version"):
                    raw = bytearray(self.data)
                    offset = {"stack": 0, "entry": 4, "version": raw.index(b"APP_VERSION=")}[failure]
                    raw[offset:offset+4] = bytes(4)
                    self.payload = bytes(raw)
                    self.manifest.update(sha256=hashlib.sha256(raw).hexdigest(), crc32=f"{gui.zlib.crc32(raw):08X}")
                elif failure in ("target", "app_address", "download_url"):
                    self.manifest[failure] = "//other.invalid/bin"
                elif failure == "damaged_bin":
                    self.payload = self.data[:-1] + bytes([self.data[-1] ^ 1])
                elif failure == "version_mismatch":
                    v = "0.0.0" if self.version != "0.0.0" else "0.0.1"
                    self.manifest.update(latest_version=v, download_url=f"{gui.API_PATH}/releases/{v}/firmware.bin")
                with patch.object(gui, "DOWNLOAD_TIMEOUT_S", 0.2 if failure == "timeout" else 5):
                    self.check()
                self.assertIsNone(self.app.firmware_data)
                self.assertIsNone(self.app.cloud_info)
                with patch.object(self.app, "_require_serial") as serial_gate:
                    self.app._start_ota()
                    serial_gate.assert_not_called()
        self.app.local_path_var.set(str(self.bin))
        self.app.src_var.set("local")
        self.assert_dispatch()


if __name__ == "__main__":
    unittest.main(verbosity=2)
