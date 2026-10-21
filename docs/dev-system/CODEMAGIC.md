# Codemagic

GitHub Actions cannot run the macOS job while the repository owner is blocked
by Actions billing. The unsigned Apple build lives in `codemagic.yaml` at the
repository root and runs on a free Codemagic personal account. The Linux job in
`.github/workflows/ci.yml` stays on GitHub Actions.

`AGENTS.md` is not in this repository. This file is the setup and limit note
for the workflow `ios-unsigned`.

## Connect the repository

Use the personal Codemagic account. Leave billing disabled, and do not put the
app on a Codemagic team. The free allowance is **500 Mac mini M2 minutes per
month**, refilled on the 1st. Those free minutes are for a personal account.
A team does not receive them. Turning billing on is what allows Codemagic to
charge minutes past 500; with billing left off, the account cannot run up a
bill. `mac_mini_m4` and the other paid instance types need billing, so this
workflow pins `instance_type: mac_mini_m2`.

1. Sign in at [codemagic.io](https://codemagic.io) with that personal account.
2. Choose **Add application**. When Codemagic asks where the app belongs, keep
   it on the personal account. The add-application step skips the team picker
   when you are not an admin of a billed team.
3. Choose **GitHub**. Authorize **Codemagic CI/CD**, then install the GitHub
   App on the GitHub account that owns this repository. Grant **only this
   repository** (Select repositories), then **Install & Authorize**.
4. Select this repository and finish adding the application. Codemagic uses
   `codemagic.yaml` from the repository root when that file is on the branch
   being built. Leave the Flutter workflow editor unused.
5. Open the application settings and use **Create webhook** under **Webhooks**
   in the right sidebar. If the button already says **Update webhook**, a hook
   is in place. Pushes start builds only after a webhook exists. **Start new
   build** works without one.
6. Do not add a certificate, a provisioning profile, an App Store Connect key,
   or any other secret. The scripts pass `CODE_SIGNING_ALLOWED=NO`. There is
   no `publishing:` block and no environment group.

The workflow name in the UI is **HydrogenVM iOS (unsigned IPA + simulator)**.
Its id is `ios-unsigned`.

## When a build starts

Automatic builds run on **push to `main` only**. Pull requests, tags, and
pushes to other branches do not start one. A commit message that contains
`[skip ci]` or `[ci skip]` skips an automatic build.

**Start new build** can run any branch, including a pull-request branch, and
still spends M2 minutes. Use that to try this workflow before it is on `main`.

`cancel_previous_builds: true` cancels queued and in-progress webhook builds
for the same branch when a newer push arrives. Minutes already used still
count. `max_build_duration: 60` stops a single build after 60 minutes, so one
stuck run cannot take more than that. The personal plan runs one build at a
time. Eight builds that each hit the 60-minute cap would use the whole month
(480 of 500). Shorter runs leave more of the allowance.

The Xcode 26.6 image Codemagic documents for this version is a Mac mini M2
with an 8-core CPU and 12 GB of RAM, macOS 26.5.1, and iOS 26.5 simulators
(including iPhone 17). `xcode: "26.6"` matches that image. The same image's
published tool list includes Python 3.12.7, `zip`, and Homebrew, and does not
list `xcodegen`, `cmake`, or `ninja` on `PATH`, so the first script installs
those three and skips Homebrew's own update.

## What the workflow checks

Commands are the ones already used by the macOS job in
`.github/workflows/ci.yml`, `tools/run_tests.sh`, `tools/verify_ipa.py`, and
the simulator picker in `tools/sim_test.sh`.

1. **Host tests.** `cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release`, then `ninja -C build hvm_tests`. CMake target `hvm_tests` is the test runner in `CMakeLists.txt`. It then runs `build/hvm_tests selftest` and `build/hvm_tests fulltest`. Those two modes are built into the runner. They do not compile guest ELF files and do not need ld64.
2. **XcodeGen.** `cd app && xcodegen` produces `HydrogenVM.xcodeproj` from `app/project.yml`. The spec name and the only application target are `HydrogenVM`. The bundle id is `com.lfull.HydrogenVM` (`PRODUCT_BUNDLE_IDENTIFIER`; `Info.plist` expands `$(PRODUCT_BUNDLE_IDENTIFIER)`). Deployment target is iOS 26.0, `ARCHS` is `arm64`, and `SUPPORTED_PLATFORMS` is `iphoneos` because the 4 GB guest arena is a device constraint.
3. **Unsigned device build.** `xcodebuild -project HydrogenVM.xcodeproj -target HydrogenVM -configuration Release -sdk iphoneos SYMROOT="$PWD/xbuild" CODE_SIGNING_ALLOWED=NO build`. The log is `app/device-build.log`. The step exits with `xcodebuild`'s status.
4. **Unsigned IPA.** The Release `HydrogenVM.app` is zipped at `Payload/HydrogenVM.app`, then checked with:

   ```sh
   python3 tools/verify_ipa.py HydrogenVM-unsigned.ipa \
     --out HydrogenVM-unsigned.manifest.json \
     --source-commit "$(git rev-parse HEAD)"
   ```

   `--out` also writes `HydrogenVM-unsigned.manifest.sha256`. The checker requires exactly `Payload/HydrogenVM.app`, bundle id `com.lfull.HydrogenVM`, package type `APPL`, expanded version metadata, an executable bit, and a thin arm64 device Mach-O whose `LC_BUILD_VERSION` platform is iOS. A simulator binary is rejected.
5. **Simulator smoke.** The same target is built with `-sdk iphonesimulator`, `SUPPORTED_PLATFORMS="iphoneos iphonesimulator"`, and `ARCHS=arm64`, which is the override the macOS job uses so the device-only platform setting does not block the simulator SDK. The script picks the newest available iPhone runtime the same way `tools/sim_test.sh` does, boots it, installs `HydrogenVM.app`, launches `com.lfull.HydrogenVM`, waits 10 seconds, and writes `app/sim.png`.

Artifacts, from the clone root: `app/HydrogenVM-unsigned.ipa`, `app/HydrogenVM-unsigned.manifest.*`, `app/*.log`, and `app/sim.png`.

## What it does not check

`tools/sim_test.sh` is the guest check used by the macOS GitHub Actions job.
It installs a guest `.app` next to the host app (CI passes
`build/ios/UIKitTest.app`), drives taps, and requires a console line such as
`ALL CHECKS PASSED`. Those guest bundles are 32-bit iOS apps produced on Linux
(`tools/ios_build.py`, `tools/build_cctools.sh`, and the `ios-test-apps`
artifact). This Mac workflow does not build them and does not download that
artifact. Launching the host app and saving a screenshot does not show that a
guest ran, and it does not show that the host process was still alive after
the 10 second wait.

Also out of scope:

- `tools/run_tests.sh` beyond `selftest` and `fulltest` (the asm and C suites need `tools/build_tests.sh`).
- `tools/hvm_report.py`, CoreMark, and the Apple-clang speed report.
- `tools/ios_tests.sh` and any path that needs `HVM_LD64`.
- A signature, a provisioning profile, or an install on an iPhone. `verify_ipa.py` records `signatureVerified: false` and `deviceTested: false`. Sideloading the unsigned IPA is still the process in `docs/SIDELOAD.md`, including the free Apple ID entitlement limits for the 4 GB arena.
- The Linux test report and the `ci-reports` branch.

## This file was not run on a Mac

`codemagic.yaml` was checked with yamllint (default rules) and against
Codemagic's published JSON schema (`https://codemagic.io/codemagic-schema.json`).
Nothing in this change was executed with Xcode, `xcodebuild`, `simctl`, or a
Codemagic builder. The free M2 builder has 12 GB of RAM, so a compile that
exhausts memory would appear only there. The first real signal is a manual
build of the branch that introduces the file.
