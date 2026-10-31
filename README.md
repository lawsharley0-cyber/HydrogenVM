# HydrogenVM

HydrogenVM runs legacy 32-bit ARMv7 iOS apps through a JIT-less emulator on modern Apple devices. The iPhone app imports IPA files and provides a game screen, touch controls, audio, and diagnostic logs.

This is work in progress. Compatibility varies by app and version; reaching a menu or passing Linux tests does not prove complete gameplay on an iPhone. Encrypted FairPlay apps are unsupported.

## iPhone app

The current app requires iOS 26 or later and an arm64 device. Device builds use the extended virtual addressing entitlement for the guest's 4 GB address space.

Unsigned device packages are produced by the [CI workflow](https://github.com/lawsharley0-cyber/HydrogenVM/actions/workflows/ci.yml). When a macOS build completes packaging, download its **HydrogenVM-device-ipa** artifact. It contains the unsigned IPA and a verification manifest recording the source commit. An unsigned IPA still needs signing through your sideloading tool before installation.

Import a compatible app archive from Files, then launch it from the library. Use only app files you are authorized to use; do not add commercial IPAs or game assets to this repository.

## Compatibility and reports

- [Known device problems and reproduction scenarios](docs/dev-system/USER_APP_REPORTS.md)
- [Compatibility harness and evidence](docs/dev-system/COMPAT_HARNESS.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Clash Royale workstream](docs/clash-royale/README.md)

When reporting a problem, include the HydrogenVM commit, app version, device/iOS version, what you did, and the exported logs or crash report.

## Development

The engine uses C++20, CMake, and Ninja with Clang or GCC. The iPhone shell uses Swift and XcodeGen. See [AGENTS.md](AGENTS.md) for contribution rules, ownership, and validation requirements, and [the handoff](docs/GROK_HANDOFF.md) for build and debugging details.

For the iPhone project, install XcodeGen, run `xcodegen` inside `app/`, and open the generated `HydrogenVM.xcodeproj` in Xcode.
