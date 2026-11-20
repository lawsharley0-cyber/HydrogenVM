#!/usr/bin/env python3
"""HydrogenVM Phase H2: Signed downloadable improvement packs (Ed25519).

Packages data-only runtime improvements (placeholder tables, compatibility
profiles, Java stand-ins, server-pack rules, and control layouts) into a
versioned JSON envelope signed with Ed25519 via OpenSSL. Unsigned, malformed,
or tampered packs are strictly refused.

Usage:
  python3 tools/pack_sign.py keygen --private priv.pem --public pub.pem
  python3 tools/pack_sign.py sign --in payload.json --private priv.pem --out pack.hvmp.json
  python3 tools/pack_sign.py verify --pack pack.hvmp.json --public pub.pem
"""

from __future__ import annotations

import argparse
import base64
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

PACK_SCHEMA_VERSION = 1
ALLOWED_SECTIONS = frozenset(
    {
        "placeholders",
        "compat_profiles",
        "java_standins",
        "server_packs",
        "control_layouts",
    }
)


def canonical_payload_bytes(payload: dict[str, Any]) -> bytes:
    """Deterministic canonical UTF-8 JSON representation for signing."""
    return json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("utf-8")


def validate_payload(payload: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if not isinstance(payload, dict):
        return ["payload must be a JSON object"]
    if payload.get("schema_version") != PACK_SCHEMA_VERSION:
        errors.append(f"schema_version must be {PACK_SCHEMA_VERSION}")
    if not isinstance(payload.get("pack_version"), str) or not payload["pack_version"].strip():
        errors.append("pack_version must be a non-empty string")
    sections = payload.get("sections")
    if not isinstance(sections, dict):
        errors.append("sections must be a JSON object")
    else:
        for key in sections:
            if key not in ALLOWED_SECTIONS:
                errors.append(f"unsupported section: {key}")
    return errors


def generate_ed25519_keypair(private_pem: Path, public_pem: Path) -> None:
    private_pem.parent.mkdir(parents=True, exist_ok=True)
    public_pem.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["openssl", "genpkey", "-algorithm", "ED25519", "-out", str(private_pem)],
        check=True,
        capture_output=True,
    )
    subprocess.run(
        ["openssl", "pkey", "-in", str(private_pem), "-pubout", "-out", str(public_pem)],
        check=True,
        capture_output=True,
    )


def sign_pack(payload: dict[str, Any], private_pem: Path) -> dict[str, Any]:
    errs = validate_payload(payload)
    if errs:
        raise ValueError("invalid pack payload: " + "; ".join(errs))
    raw = canonical_payload_bytes(payload)
    with tempfile.TemporaryDirectory() as td:
        msg_path = Path(td) / "payload.bin"
        sig_path = Path(td) / "payload.sig"
        msg_path.write_bytes(raw)
        subprocess.run(
            [
                "openssl",
                "pkeyutl",
                "-sign",
                "-inkey",
                str(private_pem),
                "-rawin",
                "-in",
                str(msg_path),
                "-out",
                str(sig_path),
            ],
            check=True,
            capture_output=True,
        )
        sig_b64 = base64.b64encode(sig_path.read_bytes()).decode("ascii")
    return {
        "format": "hydrogenvm-improvement-pack",
        "algorithm": "ed25519",
        "payload": payload,
        "signature": sig_b64,
    }


def verify_pack(envelope: dict[str, Any], public_pem: Path) -> tuple[bool, str]:
    if not isinstance(envelope, dict):
        return False, "envelope must be a JSON object"
    if envelope.get("format") != "hydrogenvm-improvement-pack":
        return False, "unexpected pack format"
    if envelope.get("algorithm") != "ed25519":
        return False, "unsupported signature algorithm"
    sig_b64 = envelope.get("signature")
    if not isinstance(sig_b64, str) or not sig_b64.strip():
        return False, "missing signature"
    payload = envelope.get("payload")
    errs = validate_payload(payload)
    if errs:
        return False, "invalid payload: " + "; ".join(errs)

    try:
        sig_bytes = base64.b64decode(sig_b64, validate=True)
    except Exception:
        return False, "malformed base64 signature"

    raw = canonical_payload_bytes(payload)
    with tempfile.TemporaryDirectory() as td:
        msg_path = Path(td) / "payload.bin"
        sig_path = Path(td) / "payload.sig"
        msg_path.write_bytes(raw)
        sig_path.write_bytes(sig_bytes)
        proc = subprocess.run(
            [
                "openssl",
                "pkeyutl",
                "-verify",
                "-pubin",
                "-inkey",
                str(public_pem),
                "-rawin",
                "-in",
                str(msg_path),
                "-sigfile",
                str(sig_path),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        if proc.returncode != 0:
            return False, "Ed25519 signature verification failed"
    return True, "ok"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="HydrogenVM Phase H2 signed improvement pack tool")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_kg = sub.add_parser("keygen", help="Generate an Ed25519 signing keypair")
    p_kg.add_argument("--private", type=Path, required=True)
    p_kg.add_argument("--public", type=Path, required=True)

    p_sign = sub.add_parser("sign", help="Sign an improvement pack payload JSON")
    p_sign.add_argument("--in", dest="inp", type=Path, required=True)
    p_sign.add_argument("--private", type=Path, required=True)
    p_sign.add_argument("--out", type=Path, required=True)

    p_ver = sub.add_parser("verify", help="Verify a signed improvement pack")
    p_ver.add_argument("--pack", type=Path, required=True)
    p_ver.add_argument("--public", type=Path, required=True)

    args = parser.parse_args(argv)
    if args.cmd == "keygen":
        generate_ed25519_keypair(args.private, args.public)
        print(f"Generated Ed25519 keypair: {args.private}, {args.public}")
        return 0
    if args.cmd == "sign":
        payload = json.loads(args.inp.read_text(encoding="utf-8"))
        env = sign_pack(payload, args.private)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(env, indent=2) + "\n", encoding="utf-8")
        print(f"Signed pack written to {args.out}")
        return 0
    if args.cmd == "verify":
        env = json.loads(args.pack.read_text(encoding="utf-8"))
        ok, msg = verify_pack(env, args.public)
        print(f"{'OK' if ok else 'REFUSED'}: {msg}")
        return 0 if ok else 1
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
