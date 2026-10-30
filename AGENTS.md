# Agent instructions

HydrogenVM is a JIT-less ARMv7 user-mode emulator. Guest code is decoded into `Op` records and executed by ahead-of-time C++ handlers. No guest code is written into executable host memory. One process owns a 4 GB `GuestArena`, an `Engine`, guest threads that are host threads, and a symbol table that binds Mach-O imports to host functions. Frameworks are a guest Objective-C runtime in C++. The UI is composited in software. The phone shell is SwiftUI under `app/`. The desktop CLI is `hvm`.

**Start with [docs/PLAN.md](docs/PLAN.md)**: the only plan, with task cards and the rules for running several agents at once.

This file is the working agreement for a coding agent. It does not change the emulator. The detail is in `docs/dev-system/`:

- [ARCHITECTURE_MAP.md](docs/dev-system/ARCHITECTURE_MAP.md) – roles, files, and groups A-E
- [IMPLEMENTATION_PLAN.md](docs/dev-system/IMPLEMENTATION_PLAN.md) – operational, planned, and needs-owner
- [COMPAT_HARNESS.md](docs/dev-system/COMPAT_HARNESS.md) – how to run and read the gate

## Test Lab is the first gate

Before and after any change under `src/`, run the Test Lab (`TestLab.ipa`, a deterministic 32-bit iOS guest with independently derived expected values) in addition to host unit tests.

## Key Rules

1. Plan-first rule: Agents post a short plan on the issue before coding.
2. Iteration cap: Respect maximum iteration limits per task.
3. Single agent editor for hotspot collision files: Hotspot collision files (shared registration code and Process.cpp) must be edited by a single designated agent editor.
4. Keep T1 off Copilot: Cards needing a game run stay on VM runner.
