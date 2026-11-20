#!/usr/bin/env python3
"""HydrogenVM Phase F1: Generic data-driven local stand-in server toolkit.

Serves retired online game endpoints strictly on loopback (127.0.0.1) from a
declarative JSON server pack (endpoints, matchers, templated replies, and
simple persistent JSON state). Never circumvents DRM or purchases.

Usage:
  python3 tools/server_pack.py validate docs/server-packs/rayman-adventures.json
  python3 tools/server_pack.py serve docs/server-packs/rayman-adventures.json --port 8080 --state state.json
  python3 tools/server_pack.py export-capture capture.jsonl --game "My Game" --out pack.json
"""

from __future__ import annotations

import argparse
import fnmatch
import http.server
import json
import os
import re
import socketserver
import sys
import threading
import time
import urllib.parse
import uuid
from pathlib import Path
from typing import Any

SCHEMA_VERSION = 1
_TEMPLATE_RE = re.compile(r"\{\{\s*([a-zA-Z0-9_.-]+)\s*\}\}")


def validate_pack(pack: dict[str, Any]) -> list[str]:
    """Return a list of schema validation errors (empty if valid)."""
    errors: list[str] = []
    if not isinstance(pack, dict):
        return ["pack root must be a JSON object"]
    if pack.get("schema_version") != SCHEMA_VERSION:
        errors.append(f"schema_version must be {SCHEMA_VERSION}")
    if not isinstance(pack.get("game"), str) or not pack["game"].strip():
        errors.append("game must be a non-empty string")
    hosts = pack.get("hosts", [])
    if not isinstance(hosts, list) or not all(isinstance(h, str) and h for h in hosts):
        errors.append("hosts must be a list of non-empty strings")
    rules = pack.get("rules")
    if not isinstance(rules, list) or not rules:
        errors.append("rules must be a non-empty list")
        return errors
    for idx, rule in enumerate(rules):
        if not isinstance(rule, dict):
            errors.append(f"rule[{idx}] must be an object")
            continue
        has_matcher = any(k in rule for k in ("path", "path_prefix", "path_regex"))
        if not has_matcher:
            errors.append(f"rule[{idx}] needs one of path, path_prefix, or path_regex")
        if "path_regex" in rule:
            try:
                re.compile(rule["path_regex"])
            except re.error as exc:
                errors.append(f"rule[{idx}].path_regex invalid: {exc}")
        status = rule.get("status", 200)
        if not isinstance(status, int) or not (100 <= status <= 599):
            errors.append(f"rule[{idx}].status must be an HTTP status int (100..599)")
    return errors


def load_pack(path: str | Path) -> dict[str, Any]:
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    errs = validate_pack(data)
    if errs:
        raise ValueError(f"invalid server pack {path}: " + "; ".join(errs))
    return data


def _host_matches(pattern: str, host: str) -> bool:
    host_only = host.split(":", 1)[0].strip().lower()
    pat = pattern.strip().lower()
    return fnmatch.fnmatch(host_only, pat)


def match_rule(
    pack: dict[str, Any],
    method: str,
    path: str,
    host: str = "",
) -> dict[str, Any] | None:
    """Return the first matching rule for (method, path, host), or None."""
    method_u = method.upper()
    pack_hosts = pack.get("hosts") or []
    if host and pack_hosts:
        if not any(_host_matches(pat, host) for pat in pack_hosts):
            return None
    for rule in pack.get("rules", []):
        rule_method = str(rule.get("method", "*")).upper()
        if rule_method != "*" and rule_method != method_u:
            continue
        rule_host = rule.get("host")
        if rule_host and host and not _host_matches(str(rule_host), host):
            continue
        if "path" in rule and path != rule["path"]:
            continue
        if "path_prefix" in rule and not path.startswith(rule["path_prefix"]):
            continue
        if "path_regex" in rule and not re.search(rule["path_regex"], path):
            continue
        return rule
    return None


def _render_string(text: str, state: dict[str, Any], path: str, query: dict[str, list[str]]) -> str:
    def repl(match: re.Match[str]) -> str:
        key = match.group(1)
        if key == "now_unix":
            return str(int(time.time()))
        if key == "now_iso":
            return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        if key == "uuid":
            return str(uuid.uuid4())
        if key == "req.path":
            return path
        if key.startswith("req.query."):
            qkey = key[len("req.query.") :]
            vals = query.get(qkey)
            return vals[0] if vals else ""
        if key.startswith("state."):
            skey = key[len("state.") :]
            val = state.get(skey, "")
            return json.dumps(val) if isinstance(val, (dict, list)) else str(val)
        return match.group(0)

    return _TEMPLATE_RE.sub(repl, text)


def _render_value(val: Any, state: dict[str, Any], path: str, query: dict[str, list[str]]) -> Any:
    if isinstance(val, str):
        return _render_string(val, state, path, query)
    if isinstance(val, list):
        return [_render_value(v, state, path, query) for v in val]
    if isinstance(val, dict):
        return {k: _render_value(v, state, path, query) for k, v in val.items()}
    return val


class ServerPackEngine:
    """Thread-safe stateful evaluator for a loaded server pack."""

    def __init__(self, pack: dict[str, Any], state_path: str | Path | None = None):
        errs = validate_pack(pack)
        if errs:
            raise ValueError("invalid server pack: " + "; ".join(errs))
        self.pack = pack
        self.state_path = Path(state_path) if state_path else None
        self._lock = threading.Lock()
        self.state: dict[str, Any] = dict(pack.get("state") or {})
        if self.state_path and self.state_path.exists():
            saved = json.loads(self.state_path.read_text(encoding="utf-8"))
            if isinstance(saved, dict):
                self.state.update(saved)

    def _save_state_locked(self) -> None:
        if not self.state_path:
            return
        self.state_path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.state_path.with_suffix(self.state_path.suffix + ".tmp")
        tmp.write_text(json.dumps(self.state, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        os.replace(tmp, self.state_path)

    def handle(
        self,
        method: str,
        raw_path: str,
        host: str = "",
        body: bytes = b"",
    ) -> tuple[int, dict[str, str], bytes]:
        parsed = urllib.parse.urlsplit(raw_path)
        path = parsed.path or "/"
        query = urllib.parse.parse_qs(parsed.query)

        with self._lock:
            rule = match_rule(self.pack, method, path, host=host)
            if rule is None:
                status = int(self.pack.get("default_status", 404))
                payload = json.dumps({"error": "no matching stand-in rule", "path": path}).encode("utf-8")
                return status, {"Content-Type": "application/json"}, payload

            dirty = False
            if "persist_body_key" in rule and body:
                key = str(rule["persist_body_key"])
                try:
                    self.state[key] = json.loads(body.decode("utf-8"))
                except Exception:
                    self.state[key] = body.decode("utf-8", errors="replace")
                dirty = True

            for k, v in (rule.get("state_updates") or {}).items():
                self.state[str(k)] = _render_value(v, self.state, path, query)
                dirty = True

            if dirty:
                self._save_state_locked()

            status = int(rule.get("status", 200))
            headers = {str(k): str(v) for k, v in (rule.get("headers") or {}).items()}
            raw_body = rule.get("body", "")
            rendered = _render_value(raw_body, self.state, path, query)
            if isinstance(rendered, (dict, list)):
                headers.setdefault("Content-Type", "application/json")
                out_bytes = json.dumps(rendered).encode("utf-8")
            else:
                headers.setdefault("Content-Type", "text/plain; charset=utf-8")
                out_bytes = str(rendered).encode("utf-8")
            return status, headers, out_bytes


class _ThreadingLoopbackServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def create_loopback_server(
    engine: ServerPackEngine,
    port: int = 0,
) -> _ThreadingLoopbackServer:
    """Create a loopback-only HTTP server bound to 127.0.0.1."""

    class Handler(http.server.BaseHTTPRequestHandler):
        def _dispatch(self) -> None:
            length = int(self.headers.get("Content-Length", "0") or "0")
            body = self.rfile.read(length) if length > 0 else b""
            host = self.headers.get("Host", "")
            status, headers, payload = engine.handle(self.command, self.path, host=host, body=body)
            self.send_response(status)
            for k, v in headers.items():
                self.send_header(k, v)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def do_GET(self) -> None:
            self._dispatch()

        def do_POST(self) -> None:
            self._dispatch()

        def do_PUT(self) -> None:
            self._dispatch()

        def do_DELETE(self) -> None:
            self._dispatch()

        def log_message(self, fmt: str, *args: Any) -> None:
            pass

    return _ThreadingLoopbackServer(("127.0.0.1", port), Handler)


def export_pack_from_capture(
    capture_jsonl: Path,
    game: str,
    hosts: list[str] | None = None,
) -> dict[str, Any]:
    """Convert a JSONL capture of {method, path, status, headers, body} into a server pack."""
    seen: set[tuple[str, str]] = set()
    rules: list[dict[str, Any]] = []
    discovered_hosts: set[str] = set(hosts or [])

    for line in capture_jsonl.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        rec = json.loads(line)
        method = str(rec.get("method", "GET")).upper()
        path = urllib.parse.urlsplit(str(rec.get("path", "/"))).path or "/"
        if rec.get("host"):
            discovered_hosts.add(str(rec["host"]).split(":", 1)[0].lower())
        key = (method, path)
        if key in seen:
            continue
        seen.add(key)
        rules.append(
            {
                "method": method,
                "path": path,
                "status": int(rec.get("status", 200)),
                "headers": rec.get("headers") or {"Content-Type": "application/json"},
                "body": rec.get("body", {}),
            }
        )

    return {
        "schema_version": SCHEMA_VERSION,
        "game": game,
        "hosts": sorted(discovered_hosts),
        "default_status": 404,
        "state": {},
        "rules": rules,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="HydrogenVM local stand-in server toolkit (Phase F1)")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_val = sub.add_parser("validate", help="Validate a server pack JSON file")
    p_val.add_argument("pack", type=Path)

    p_srv = sub.add_parser("serve", help="Run a loopback-only stand-in server for a pack")
    p_srv.add_argument("pack", type=Path)
    p_srv.add_argument("--port", type=int, default=8080)
    p_srv.add_argument("--state", type=Path, default=None)

    p_exp = sub.add_parser("export-capture", help="Generate a server pack from a JSONL capture")
    p_exp.add_argument("capture", type=Path)
    p_exp.add_argument("--game", required=True)
    p_exp.add_argument("--out", type=Path, required=True)

    args = parser.parse_args(argv)
    if args.cmd == "validate":
        pack = load_pack(args.pack)
        print(f"OK: {pack['game']} ({len(pack['rules'])} rules, hosts={pack.get('hosts', [])})")
        return 0
    if args.cmd == "serve":
        pack = load_pack(args.pack)
        engine = ServerPackEngine(pack, state_path=args.state)
        srv = create_loopback_server(engine, port=args.port)
        addr, port = srv.server_address
        print(f"Serving {pack['game']} on http://{addr}:{port} (loopback only)")
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            srv.server_close()
        return 0
    if args.cmd == "export-capture":
        pack = export_pack_from_capture(args.capture, game=args.game)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(pack, indent=2) + "\n", encoding="utf-8")
        print(f"Wrote {args.out} ({len(pack['rules'])} rules)")
        return 0
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
