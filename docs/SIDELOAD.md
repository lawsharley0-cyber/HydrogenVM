# Sideloading HydrogenVM onto an iPhone

HydrogenVM is not on the App Store. It is installed by signing it with your
own Apple ID, the same way other developer builds are installed.

## What you need

- An iPhone that runs **iOS 26** or later (the deployment target is iOS 26.0),
  with a 64-bit (arm64) chip. iPhone 11 or newer is a safe bet.
- A computer to sign the app: a Mac with Xcode, or Windows/Linux/Mac with
  **Sideloadly** or **AltStore**.
- The unsigned build, `HydrogenVM-unsigned.ipa`. It is produced by the macOS CI
  job (manual run from the Actions tab, or a `[mac]` commit). Download it from
  the run's artifacts, or build it yourself on a Mac:
  ```sh
  brew install xcodegen
  cd app && xcodegen
  xcodebuild -project HydrogenVM.xcodeproj -target HydrogenVM -configuration Release \
    -sdk iphoneos SYMROOT="$PWD/xbuild" CODE_SIGNING_ALLOWED=NO build
  ```

## Route A: Sideloadly or AltStore (no Xcode needed)

1. Install Sideloadly (or AltStore) on your computer.
2. Connect the iPhone by cable, trust the computer, and enable Developer Mode
   (Settings → Privacy & Security → Developer Mode).
3. Drag `HydrogenVM-unsigned.ipa` into Sideloadly, enter your Apple ID, and
   start. It signs the app with a free provisioning profile and installs it.
4. On the iPhone: Settings → General → VPN & Device Management → trust your
   Apple ID.

## Route B: Xcode (Mac)

1. Open `app/HydrogenVM.xcodeproj`, select the HydrogenVM target, and set
   **Signing & Capabilities → Team** to your Apple ID.
2. Run on the connected iPhone (Release scheme for speed).

## Limits of a free Apple ID

- Installed apps expire after **7 days** and must be re-signed.
- The app's entitlements (`extended-virtual-addressing`,
  `increased-memory-limit`) are used to reserve the 4 GB guest address space
  and more memory headroom. Free Apple IDs may not be allowed these
  entitlements. If signing fails with an entitlement error, remove the two
  keys from `app/HydrogenVM/HydrogenVM.entitlements` for a free-account build.
  The app checks whether it can reserve the arena (`canReserveGuestArena`)
  and reports it in the Diagnostics tab. Guest apps may run out of memory
  without the larger arena.
- A paid Apple Developer account removes the 7-day limit and supports the
  entitlements.

## First run checklist

1. Open HydrogenVM. Check the **Diagnostics** tab: the self-test should pass
   and the arena should be reported as reservable.
2. Library tab → import an `.ipa` (Files app, or copy into the app's Documents
   through Finder file sharing).
3. Run it. Report problems through the run log and the Diagnostics tab.

## Known limits

- iPhone only, arm64 only, iOS 26+.
- No app icon yet (the app uses the default icon).
- Speed is below the original iPhone 5 target. See `docs/PERFORMANCE.md`.
- The interpreter does not JIT, so it's slower than native and uses more
  battery.
