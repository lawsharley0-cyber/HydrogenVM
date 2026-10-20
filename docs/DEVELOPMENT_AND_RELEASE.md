# Local development and the iPhone release path

The Windows/WSL tester builds the **same shared C++ runtime** that the iPhone
application includes. It is useful for reproducing bugs, fixing APIs/CPU/graphics,
adding shared features, testing input and save behavior, and catching regressions
without waiting for GitHub Actions. The pygame window is a host testing frontend;
it is not the SwiftUI iPhone application.

## App coverage

The desktop viewer accepts any IPA and the scanner accepts folders of IPAs or
extracted `.app` bundles. Current guest support is **unencrypted 32-bit ARM iOS
apps (ARMv6/ARMv7)**. Modern arm64-only apps, encrypted packages and Android APKs
are outside that support. Being accepted by the launcher does not guarantee
rendering, gameplay, audio, network services or complete API compatibility.

Workstation launchers:

- **Open Any IPA.cmd** selects an IPA and runs it interactively.
- **Test an App Folder.cmd** scans a collection in strict, exploratory and
  relaunch modes. Its report distinguishes load problems, reached API gaps and
  suspicious output; a `runs` result does not establish complete app functionality.
- **Run Runtime Checks.cmd** builds and tests the shared runtime.
- The Clash-specific launcher continues to enforce that client's pinned hash.

Generic desktop sessions get separate persistent sandboxes for each IPA hash.
This allows different versions of the same app to coexist without overwriting
saves. These profiles are separate from the pinned Clash launcher's profile and
from an iPhone's app data. No automatic progression migration is implied.

Each new session records the IPA hash, actual engine-binary hash, source commit,
dirty-tree state and sandbox path in `session.json`, alongside logs and images.
Always identify the tested code/client when reporting a bug.

## Iteration

1. Capture the failing app's screen, log, exact input and save/relaunch behavior.
2. Edit the shared runtime in this checkout; fix the underlying behavior.
3. Launch again. The launchers incrementally rebuild current local files; a
   commit or push is not needed to test a change.
4. Run runtime checks and the affected app probe. For shared changes, run the
   older-game regression set and any additional affected apps. Add focused
   checks for material correctness, rather than tests that mirror a patch.
5. Review and commit the tested change on `gemini-merge`.
6. Use an Apple build for changes to SwiftUI, iOS integration, signing and device
   behavior. Windows tests cannot compile/verify those parts or establish iPhone
   performance, memory limits, Metal/UIKit behavior or device audio.

Current baseline: 57 local runtime checks, four app regression probes, a generic
Angry Birds desktop smoke test, and six package-validator unit tests. The latter
use synthetic archive metadata, not installable application fixtures.

## Building and packaging HydrogenVM.ipa

The product to package is the **HydrogenVM iPhone app**, containing the emulator
and its iOS frontend. Guest game IPAs are imported separately; the desktop viewer
and guest game assets are not bundled into the emulator's IPA.

The existing `.github/workflows/ci.yml` Apple job uses macOS/Xcode, XcodeGen and
the iPhoneOS SDK. It is requested by manual workflow dispatch, the schedule, or
a pushed commit containing `[mac]`. It depends on the Linux job. A local Mac can
run the same build/package commands. A Windows WSL build is not an iOS build.
The current project targets arm64 iPhones with iOS 26.0 or later.

Required pre-device gates in that workflow:

1. Shared Linux tests/report and Apple-toolchain tests/report.
2. A Release device build with `CODE_SIGNING_ALLOWED=NO`.
3. Package the device `.app` under `Payload/HydrogenVM.app`.
4. `tools/verify_ipa.py` checks archive CRC/layout, resolved metadata, executable
   permissions, arm64 executable format and `LC_BUILD_VERSION` targeting iOS
   devices. An arm64 simulator build is explicitly rejected.
5. Build the simulator app and pass the existing UIKit guest test inside it.
6. Upload **HydrogenVM-device-ipa** only after the preceding steps succeed. It
   includes `HydrogenVM-unsigned.ipa`, a JSON manifest and SHA-256 file. The
   manifest records source commit, package hash, version/build, minimum OS and
   platform. **macos-results** remains the diagnostic artifact, including failures.

The metadata checker can also be run on Windows/WSL against an already-built
device IPA. It cannot turn a Linux binary into an iOS executable:

```sh
python3 tools/verify_ipa.py HydrogenVM-unsigned.ipa \
  --out HydrogenVM-unsigned.manifest.json --source-commit COMMIT_SHA
python3 -m unittest discover -s tools/tests -p test_verify_ipa.py
```

## When an IPA is ready for use

The workflow creates an **unsigned** device package. It still needs a valid
signing/provisioning route and the entitlements in
`app/HydrogenVM/HydrogenVM.entitlements`, including its memory/address-space
requirements. Do not treat a ZIP, green metadata check or simulator launch as
proof that a signed device installation works.

Before calling a build reliable: sign/install it on a supported iPhone; verify
first launch, app import, touch input, rendering, audio, background/foreground,
save/stop and reopening for the supported test apps; then associate those results
with that exact IPA hash. App-specific goals such as Clash offline NPC battles
need their own end-to-end evidence.

The added package checks pass locally, but the updated macOS packaging path
has not been run here. There is no new verified iPhone IPA from this Windows
session. GitHub Actions currently has an account billing/spending-limit block;
use a Mac with Xcode or resolve that external block for the Apple build stage.
