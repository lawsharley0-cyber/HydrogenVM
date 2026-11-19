# Importable offline edition

This page documents the **earlier host-dependent metadata package**. The owner
now explicitly requests guest-owned offline logic; see
[GUEST_MOD_PROGRESS.md](GUEST_MOD_PROGRESS.md) for the executable mod prototype,
ordinary-main tests and remaining work. Do not call this metadata-only package
a self-contained offline game.

The owner requests a separate Clash IPA imported into HydrogenVM. This is an
ARMv7 **guest package**, not a native modern-iOS port. HydrogenVM still supplies
the embedded NPC service and emulation. An older HydrogenVM build cannot gain
that service from metadata alone. The owner's iPhone still reportedly stops
at 50% / couldn't log in using a PR #138 build; its precise commit and device
logs remain unavailable. Do not describe this packaging change as fixing that
failure or claim that TCP is its established cause.

Build from the documented, privately supplied pinned source:

```sh
python3 tools/package_clash_offline.py /path/to/pinned.ipa /path/to/Clash-Royale-Offline.ipa
```

The tool pins both source archive and executable hashes. It changes only
`Payload/Clash Royale.app/Info.plist`: display name `Clash Royale Offline`,
`HydrogenVMOfflineBackend=clash-2.1306.1`, package version 1. It verifies every
other archive member remains byte-identical and writes an adjacent manifest.
No source executable/assets, credentials or saved progress are added to Git.
Metadata changes invalidate any source signature; import this package into
HydrogenVM rather than installing it directly on iOS.

The bundle ID and client version remain `com.supercell.scroll` / `2.1306.1`,
preserving the existing sandbox/account identity. Importing can update the
existing Clash library entry rather than creating an independent account.
Back up existing progress using HydrogenVM before changing a valued install.

Shared Process::load reads the backend marker, enables the embedded service,
then checks the unchanged executable hash before running it. Unknown backends
fail explicitly. The marker works with both the desktop CLI and Apple shell;
no `--offline-clash` flag is needed. These two cases have C++ regressions.

## Local qualification, October 25, 2026

Package SHA256:
`d12c60ac4b0270b830109448e2d86f9850243e06fb9620355a30827b0c613f17`.
Source/executable hashes are also in its generated manifest. A fresh import
with the current local build, without loopback/offline flags, ran 35 seconds
and reached the original Trainer Earl welcome scene with no load error/crash.
The log records backend selection, listener, connection, login and keepalives.
Private local evidence: `evidence/package-import/startup.{json,log,png}` under
the WSL restoration workspace. This is Linux/WSL evidence, **not iPhone proof**.

```sh
hvm run /path/to/Clash-Royale-Offline.ipa --home /tmp/clash-package-test \
  --run-for 35 --graceful --gpu --report /tmp/clash-package-startup.json
```

Runtime 252/252, Test Lab 15/15 and the offline socket/state tests pass locally.
TL-O independently exposes the previously wrong __sincosf_stret float-aggregate
ABI (float fails before, double control passes; both pass after). Original
Character::updateTurretClip calls that import with a hidden output pointer;
the correction addresses unwritten recoil coordinates, but full cannon motion,
princess angle and phone visual fidelity still require qualification.

CI now requires synthetic offline service/state tests on Linux and Apple hosts,
and TL-O with the Linux-built guest on Apple. Those are stronger host checks,
not a substitute for importing this IPA and reaching gameplay on the phone.
