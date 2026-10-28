# Memory Telemetry implementation notes

This document outlines the testing, limitations, and operational semantics of the MemoryTelemetry collector added to diagnose iOS app memory-pressure constraints.

## Memory Metrics Provenance & Units
- **`residentBytes`**: Gathered from `task_info(TASK_VM_INFO)` -> `resident_size`. Indicates the total memory currently loaded in RAM.
- **`physicalFootprint`**: Gathered from `task_info(TASK_VM_INFO)` -> `phys_footprint`. Represents the memory charged to the application by the OS.
- **`peakFootprint`**: Gathered from `task_info(TASK_VM_INFO)` -> `lifetime_max_phys_footprint`. Peak memory charged across the process's lifetime.
- **`availableBytes`**: `os_proc_available_memory()` where available (iOS 13+). Indicates the remaining memory before Jetsam kills the app.
- **`observedPeak`**: Tracked manually via periodic 1Hz sampling, noting the highest `physicalFootprint` seen during the session.

> If an API is unavailable (e.g. `os_proc_available_memory` on macOS or Linux), its value will be marked as `~0ull` in the telemetry.

## Persistence and Memory Warnings
A periodic background thread evaluates memory footprints at 1Hz without performing dynamic allocations in the inner loop.
If `notifyMemoryWarning()` is called, the background thread wakes via a condition variable and flushes a compact, bounded JSON snapshot to the sandbox (`<home>/telemetry.json`). The snapshot keeps the last 600 samples (10 minutes).
On unexpected termination (e.g. iOS Jetsam kill), this file allows the next launch to recover the most recent incomplete session. Note: since formatting the JSON requires allocations, writing out is intentionally throttled.

## Limitations
- **1Hz Sampling Delay**: A memory warning or Jetsam kill may happen before the 1s sleep concludes (if `UIApplicationDidReceiveMemoryWarningNotification` is skipped by the OS or the process dies abruptly). Thus, 1Hz sampling is *not* a guaranteed pre-Jetsam flush.
- **iOS validation**: Linux testing validates the thread safety, schema, and API bindings. Accurate tracking of iOS footprint sizes requires real-world deployment on iPhones, as Linux limits memory differently.
