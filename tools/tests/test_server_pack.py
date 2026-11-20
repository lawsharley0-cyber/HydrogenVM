"""Unit tests for tools/server_pack.py (Phase F1 server-replacement toolkit)."""

from __future__ import annotations

import json
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import server_pack  # noqa: E402


class ServerPackTests(unittest.TestCase):
    def test_rayman_pack_validates(self) -> None:
        pack = server_pack.load_pack(ROOT / "docs/server-packs/rayman-adventures.json")
        self.assertEqual(pack["game"], "Rayman Adventures")
        self.assertGreaterEqual(len(pack["rules"]), 5)

    def test_invalid_pack_rejected(self) -> None:
        errs = server_pack.validate_pack({"schema_version": 99, "game": "", "rules": []})
        self.assertGreaterEqual(len(errs), 3)

    def test_loopback_server_and_persistent_state(self) -> None:
        pack = server_pack.load_pack(ROOT / "docs/server-packs/rayman-adventures.json")
        with tempfile.TemporaryDirectory() as td:
            state_file = Path(td) / "rayman-state.json"
            engine = server_pack.ServerPackEngine(pack, state_path=state_file)
            srv = server_pack.create_loopback_server(engine, port=0)
            port = srv.server_address[1]
            thread = threading.Thread(target=srv.serve_forever, daemon=True)
            thread.start()
            try:
                # 1. Login handshake returns offline session ticket
                req = urllib.request.Request(
                    f"http://127.0.0.1:{port}/v3/profiles/sessions",
                    data=b"{}",
                    method="POST",
                    headers={"Content-Type": "application/json"},
                )
                with urllib.request.urlopen(req) as resp:
                    self.assertEqual(resp.status, 200)
                    body = json.loads(resp.read().decode("utf-8"))
                    self.assertTrue(body["ticket"].startswith("hvm-offline-ticket-"))
                    self.assertEqual(body["profileId"], "00000000-0000-4000-8000-000000000001")

                # 2. Cloud save PUT persists to disk and survives restart
                save_req = urllib.request.Request(
                    f"http://127.0.0.1:{port}/v3/profiles/me/storage",
                    data=json.dumps({"world": 2, "eggs": 7}).encode("utf-8"),
                    method="PUT",
                    headers={"Content-Type": "application/json"},
                )
                with urllib.request.urlopen(save_req) as resp:
                    self.assertEqual(resp.status, 200)
                self.assertTrue(state_file.exists())

                # 3. Store remains unavailable offline (no IAP circumvention)
                with self.assertRaises(urllib.error.HTTPError) as ctx:
                    urllib.request.urlopen(f"http://127.0.0.1:{port}/v1/store/items")
                self.assertEqual(ctx.exception.code, 503)
            finally:
                srv.shutdown()
                srv.server_close()
                thread.join(timeout=2)

            # Reload into fresh engine and verify persisted save
            engine2 = server_pack.ServerPackEngine(pack, state_path=state_file)
            self.assertEqual(engine2.state["cloud_save"], {"world": 2, "eggs": 7})

    def test_export_capture_deduplicates_rules(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            cap = Path(td) / "capture.jsonl"
            cap.write_text(
                "\n".join(
                    [
                        json.dumps({"method": "GET", "host": "api.example.com", "path": "/v1/ping?a=1", "status": 200, "body": {"ok": True}}),
                        json.dumps({"method": "GET", "host": "api.example.com", "path": "/v1/ping?a=2", "status": 200, "body": {"ok": True}}),
                        json.dumps({"method": "POST", "host": "api.example.com", "path": "/v1/login", "status": 200, "body": {"token": "abc"}}),
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            pack = server_pack.export_pack_from_capture(cap, game="Example Game")
            self.assertEqual(server_pack.validate_pack(pack), [])
            self.assertEqual(len(pack["rules"]), 2)
            self.assertEqual(pack["hosts"], ["api.example.com"])


if __name__ == "__main__":
    unittest.main()
