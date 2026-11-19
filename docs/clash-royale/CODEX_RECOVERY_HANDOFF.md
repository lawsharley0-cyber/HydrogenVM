# Codex Recovery Handoff — Clash Royale Offline Continuation

**Repository:** `https://github.com/lawsharley0-cyber/HydrogenVM`  
**Branch:** `codex/clash-offline-continuation` (Associated with PR #138)  
**Date of Recovery:** October 25, 2026  
**Recovered From:** OpenAI Codex session `rollout-2026-10-20T14-43-45-01a15ad7-5c8b-7222-ae87-545437c2d162.jsonl`

---

## 1. Executive Summary & Incident Context

OpenAI Codex was actively developing the **Clash Royale Offline** restoration for HydrogenVM when execution was abruptly terminated by an API usage limit error (`usage_limit_exceeded` at timestamp 2026-10-25T00:23:29Z). 

The goal of the task is to enable the pinned early Clash Royale client (v2.1306.1, armv7, bundle `com.supercell.scroll`) to run inside HydrogenVM as a persistent, single-player offline game with functional NPC battles across arenas, working progression, and crash-resilient reward collection.

All local Codex session files have been verified in read-only mode, and the exact state of work has been cross-referenced with the git repository and WSL build environment.

---

## 2. Recovered State & Verified Achievements

### A. Core Progression & Battles (Verified)
- **Training Camp Completion:** All 7 native tutorial missions completed and verified against NPC trainers (NPC1 to NPC7).
- **Name Dialog:** Handled ChangeAvatarNameMessage (10212) using native command 201 and persistent avatar serialization. CoreText caret offset API (`CTLineGetOffsetForStringIndex`) implemented to prevent layout crashes.
- **Arena 1 Transition:** Player successfully promoted to Goblin Stadium (Arena 1) with native score synchronization (34 trophies, rising to 65 and 90 after subsequent wins).
- **Arena Entry Fees:** Bundled CSV `MatchCost` enforced; Gold atomically deducted on sector dispatch (Training Camp 0, Arenas 1..6 cost 1..6).

### B. Rewards, Chests & Persistence (Verified)
- **Crash-Safe Journaling:** Pre-claim checkpoints written prior to granting native rewards (standard chests, free chests via command 518, and crown chests via command 523 / source 3). If interrupted mid-grant, restarts replay the journal without duplicate grants.
- **Elapsed Home Time:** Measured 64-bit `homeSavedAt` timestamp applied through `LogicClientHome::fastForward` to allow chest timers to advance across process launches without clock manipulation.
- **Chest Cycle Reconciled:** Avatar `ChestOrderIndex` synced with earned cycle; victory screen chest labels match awarded cycle slots.

### C. Combat & Visual Fixes (Verified)
- **Tower Scaling & Health Bars:** Fixed zero-filled buff initialization bug in `LogicCharacterBuffComponent` where damage and size defaulted to 0 instead of 100% (which caused HP to clamp to 1% and health bars to freeze). Health bars now properly reflect damage and shrink.
- **Tower Occupants & Facing Vectors:** Restored opposing initial direction vectors (`+256` vs `-256`) and all 4 Princess Tower occupants.
- **Scenery & Trees:** Sector decoder updated to preserve static CSV decorations (`goblin_tree`, rocks, fences).

### D. ARMv7 Math ABI Fix (Verified)
- In `src/runtime/libc/Math.cpp`, corrected `sincosf_stret` ABI: ARMv7 iOS returns the 8-byte aggregate `{float sin, cos}` through the hidden `r0` pointer with the angle in `r1` (previously implemented as a `uint64_t` return).
- Validated with newly added TestLab suite case `TL-O` in `tests/ios/apps/testlab/main.m`.

---

## 3. Unfinished Task at Time of Interruption

### Context:
The user reported:
> *"when I simply download the new hydrogen IPA does clash launch into the game? the last clash build pushed just sent me to the 50% couldn't log in. It would be far more efficient to push this into a clash royale mod .ipa"*

Codex recognized two things:
1. Modifying the original client binary directly cannot make it run natively on 64-bit-only modern iOS (iOS 11+ dropped 32-bit armv7). Emulation inside HydrogenVM is required.
2. However, packaging the pinned Clash client with metadata indicating it requires the embedded offline backend allows HydrogenVM to automatically route and activate the backend on import/launch without requiring CLI flags or brittle name checks.

### Codex's In-Progress Changes:
1. **`src/runtime/Process.cpp`:** Added check in `Process::load()` for `HydrogenVMOfflineBackend` from `Info.plist`:
   ```cpp
   const auto offlineBackend = infoPlist_ ? infoPlist_->getString("HydrogenVMOfflineBackend") : "";
   if (!offlineBackend.empty()) {
       if (offlineBackend != "clash-2.1306.1")
           throw std::runtime_error("Unsupported HydrogenVMOfflineBackend: " + offlineBackend);
       cfg_.offlineClash = true;
       log(LogLevel::Info, "Guest package requests embedded offline backend clash-2.1306.1");
   }
   ```
2. **`tests/runner/ClashOfflineTests.cpp`:** Added unit tests verifying `HydrogenVMOfflineBackend` activation and validation.
3. **`tools/package_clash_offline.py`:** Created packaging tool that copies the pinned client into a modded IPA (`Clash-Royale-Offline-2.1306.1-HydrogenVM.ipa`) with `HydrogenVMOfflineBackend: clash-2.1306.1` set in `Info.plist` without modifying the game executable or assets.
4. **Build & Evidence:** Built and verified unit tests (`hvm_clash_tests` passed, `hvm_tests fulltest` 252 passed, `tools/hvm_testlab.py` 15 passed).
5. **Interrupted Operation:** Codex had just generated the IPA in `outputs/` and inspected `tests/runner/HvmRun.cpp` to verify running the package through `hvm run` without `--offline-clash`, when the usage limit hit.

---

## 4. Antigravity Verification & Continuation Results

1. **Packaged IPA Verification:**
   Tested `hvm run Clash-Royale-Offline-2.1306.1-HydrogenVM.ipa --run-for 10` in WSL.
   - Successfully detected: `[hvm] Guest package requests embedded offline backend clash-2.1306.1`
   - Successfully spawned offline listener: `[hvm] Offline Clash listener running on port 43659`
   - Successfully intercepted client connection: `[hud] Connecting to port 9339 -> port override 9339 -> 43659`
   - Handshake established and received client keepalive packet `10108`.

2. **Test Suite Status:**
   - C++ Offline Tests (`hvm_clash_tests`): **PASSED** (all 20 assertions)
   - Runtime Tests (`hvm_tests fulltest`): **PASSED** (252 passed, 0 failed)
   - TestLab (`tools/hvm_testlab.py`): **PASSED** (15 passed: TL-A through TL-O)

---

## 5. Immediate Next Steps

1. **Commit Uncommitted Changes:**
   Commit the packaging script, `Process.cpp` backend check, `sincosf_stret` ABI fix, and TestLab `TL-O` updates to branch `codex/clash-offline-continuation`.
2. **Push to PR #138:**
   Push changes to remote branch `origin/codex/clash-offline-continuation` to trigger CI build of updated iOS unsigned device IPA.
3. **Investigate iPhone 50% Connection Issue on Device:**
   The owner confirms a PR #138 build stops at 50% / couldn't log in. The failure
   stage and root cause are not established without matching device logs; do not
   assume TCP failure or a missing entitlement from the progress percentage.
   On iOS device:
   - Inspect backend selection, initialization, routing, handshake and home delivery in order.
   - Verify that the iOS app build of HydrogenVM incorporates the `HydrogenVMOfflineBackend` auto-configuration.
