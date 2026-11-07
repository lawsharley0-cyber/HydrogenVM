# Custom text input continuation — November 7, 2026

CI follow-up: revision `5aba0b4` passes the full Linux CI job, unsigned iOS
simulator shell compilation and the actual-bridge crash-journal regression.
The separate SwiftUI harness then failed to compile because its synthetic
request/session bridge still had the old text-input interface. The fixture now
includes keyInput/hasText and incremental callbacks. A new simulator UI case
uses the actual RunView/GameKeyInputKeyboard with a synthetic guest session to
check keyboard appearance, typing, deletion, Return and Done dismissal. This
new case still needs a successful Apple CI run; it is not a real-game or phone
test. Previously expected failures from the old crash-recovery body are not
candidate failures.

Owner report: Hydrogen's keyboard **does not appear** in Clash Royale 2.1306.1.
This is shared emulator compatibility work on `codex/uikeyinput-keyboard`,
separate from the offline guest/editor in
[PR #366](https://github.com/lawsharley0-cyber/HydrogenVM/pull/366), commit
`7d706cd96561d9df923a11100d463aa5580b41df`. The guest still needs a compatible
Hydrogen runtime. Neither branch has been merged by this agent.

## Cause and behavior

Main's #303 keyboard bridge handled UITextField/UITextView host data only.
Generic UIResponder focus changes did not publish keyboard requests, and
incoming edits were discarded unless the responder had TextFieldData.
The archived game's `KeyInput` is a custom UIView implementing UIKeyInput and
UITextInput methods, with its own text/caret/deletion logic. Its superclass
binding at guest 0x004613f4 names UIView, and its native `show` method invokes
superclass `becomeFirstResponder`. A minimal custom UIView reproduced the
missing keyboard request independently of game code.

The shared bridge now recognizes the required UIKeyInput methods, publishes
focus and traits, and forwards incremental insert/delete/return messages on
the guest main run loop. Focus IDs reject stale edits. Refused resignation
preserves the old focus. Resign/removal/deallocation close the custom keyboard.
Existing delegate-aware full-text editing for UITextField/UITextView is retained.
The iPhone shell adds a native UIKeyInput responder with keyboard traits and a
Done accessory; it activates when attached to its window, without a timed focus
guess. It does not invent a replacement text buffer for a custom guest view.

This follows Apple's [custom UIKeyInput text-entry contract](https://developer.apple.com/library/archive/documentation/StringsTextFonts/Conceptual/TextAndWebiPhoneOS/LowerLevelText-HandlingTechnologies/LowerLevelText-HandlingTechnologies.html#//apple_ref/doc/uid/TP40009542-CH6-SW2).
Physical keyboard presentation, guest accessory layout and multistage input
still need device/simulator qualification; a Linux callback is not a keyboard
screenshot on an iPhone.

## Native deletion exposed a second shared gap

After keyboard requests and insertion worked, actual Clash backspace stopped
at missing `NSString::rangeOfComposedCharacterSequencesForRange:` in
`KeyInput::deleteBackward` (0x001cae8a). This failure was retained, not bypassed.
The missing API is now implemented alongside the existing single-index API.
Both use host Unicode segmentation: CoreFoundation on Apple, ICU character
break iterators elsewhere. Linux build jobs install `libicu-dev`; CMake requires
ICU uc/i18n, while the Apple/Xcode targets link CoreFoundation.

Returned coordinates remain UTF-16. Both ends expand to whole clusters, with
Foundation's special behavior for empty ranges inside a string. Invalid and
overflowing ranges are rejected. Surrogate pairs, combining marks, joined emoji,
skin tones, flags, Hangul and CRLF have independently specified vectors. The
original surrogate-only approximation could split several of those clusters.
Unicode-version behavior is supplied by the host libraries.

References: [Apple's character-cluster guide](https://developer.apple.com/library/archive/documentation/Cocoa/Conceptual/Strings/Articles/stringsClusters.html),
[Foundation's range behavior](https://github.com/swiftlang/swift-corelibs-foundation/blob/main/Sources/Foundation/NSString.swift).
No Apple implementation was vendored. An initial ICU offset candidate failed
the low-surrogate vectors: seeking `preceding(index)` can normalize back past
the desired cluster. The corrected implementation seeks the next boundary,
then the previous boundary. Failed-candidate logs remain private.

## Evidence before latest-main integration

All paths below are private on this computer. The source/build is under
`/var/tmp/hvm-keyboard-ff24df3`; the original baseline was
`ff24df3601e282d283168c85b7aa52d7f202062a`.

- Unchanged baseline: fulltest **637/637**, Test Lab **25/25**.
- Minimal custom-responder regression: **637 passed, 1 failed**, no host
  keyboard request despite successful focus.
- New TL-Z before the keyboard fix: focus/text/resign all succeed, show/hide
  counts both zero. After the bridge fix, all **26/26** pass.
- Extending TL-Z to use actual composed deletion stops on the missing NSString
  selector before that fix. The first Unicode candidate then fails specified
  surrogate cases; those expectations were preserved.
- Corrected combined candidate: fulltest **647/647**, Test Lab **26/26**,
  including original field/delegate/Unicode/focus tests and new custom-input
  tests. Test Lab runner unit tests: **41/41**.
- Full compatibility harness: 16 unit-suite checks and three Python suites
  pass; Angry Birds, Art of Glow, Tiny Tower and the pinned unmodified Clash
  startup fixtures all resolve/run, with zero crashes/skips and zero baseline
  regressions. These short probes do not resolve the owner's longer/device app
  reports. `build/compat-final/report.json` and `diff.json` retain exact results.

Actual guest evidence is `/var/tmp/hvm-main-keyboard-qa`. It uses copied saves
and the Mod Lab guest archive SHA-256
`7768935eb28848e1ed019f4b975ef8ef232417313282d9d9cd51c745da0e3903`.
`clan-input-v3` opens Chat, requests a protocol keyboard, inserts `Keyboard QA`,
deletes the last character, inserts `B`, and submits Return. Native chat saves
`Keyboard QB`; requests include active then inactive. The earlier v2 attempt
retains the composed-range crash and never counts as a successful deletion.
The initial name attempt was correctly blocked by the game's King-level-4
rename gate, not a keyboard failure.

The private CLI input driver only installs the normal text-input observer and
submits the public protocol events used by the app bridge. It changes no guest
logic, assertions or result timing and is **absent from Git and production**.
`HvmRun.production.cpp` and `build/hvm.production` preserve the uninstrumented
CLI. These native interaction runs are not Swift keyboard presentation tests.

On that older main, graceful stop still failed on missing
`UILocalNotification::setFireDate:` with signature `e2cb9b51c631`. The same
failure occurs in `baseline-shutdown` with the unchanged baseline binary.
All seven chat steps and the saved message precede the failure, but the run is
recorded as a failure, not a clean lifecycle pass.

## Latest upstream integration and next checks

A fresh fetch found main `76db694cce9afb7e1ab67fa010854f199088b810`, including
the other updater's `7fa8884` local-notification property/copy implementation.
This branch was rebased onto that main without dropping its loader, audio,
libc++, scan or storyboard changes. Do not duplicate or replace those fixes.
Clean builds qualify that pristine main with **647/647 fulltest** and **25/25
Test Lab**, and the integrated candidate with **657/657 fulltest** and **26/26
Test Lab**. Their independent roots are `/var/tmp/hvm-keyboard-latest-baseline`
and `/var/tmp/hvm-keyboard-latest-integrated`. An earlier attempt to reuse the
old archive-extraction build cache produced a mixed-layout host failure; that
attempt is rejected. No runtime check was changed to pass it. Use clean roots
when replacing snapshots that change runtime headers.

The integrated full harness also passes all 16 unit-suite checks, three Python
suites and four resolved app fixtures, with no crashes/skips and no regressions
against `linux-headless.json`. Its exact report/diff are
`/var/tmp/hvm-keyboard-latest-integrated/build/compat-report/{report,diff}.json`.
This does not replace the longer user-specific app/performance reports.

`/var/tmp/hvm-main-keyboard-qa/clan-latest` now exits **0**: insert `Latest QA`,
backspace, insert `B`, Return, native chat saves `Latest QB`, then clean graceful
shutdown. `clan-latest-relaunch` independently exits **0** and retains that
message. Gold/resources, Gems, XP, King level, wins, losses and trophies remain
unchanged; the original source save's hash is unchanged. Screenshots were
reviewed. The new upstream notification implementation resolves the previously
recorded shutdown blocker in these exact runs. The updated private CLI input
driver remains outside Git; `build/hvm.production` is the uninstrumented binary.

Reproduce the runtime gates from a Linux checkout:

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
ninja -C build hvm hvm_tests hvm_decodecheck hvm_heap_tests
build/hvm_tests fulltest
python3 tools/hvm_testlab.py run --hvm build/hvm --out build/testlab-report
bash tools/build_tests.sh build/tests
python3 tools/hvm_compat.py run --build-dir build --out build/compat-report \
  --fixture-root /private/authorized-fixtures \
  --baseline docs/dev-system/baselines/linux-headless.json --require-resolved
```

Use fresh outputs and copied homes; keep the original earned and phone-failing
saves intact. There is no JIT, title-specific runtime switch, assertion
suppression or blanket successful no-op in this change. A 4 KB WSL result is
not a 16 KB iPhone memory-model, audio-quality or performance pass. macOS/Xcode
compilation and actual phone keyboard/input/accessory checks remain required.
Mod Lab custom cards and independent weapon/projectile visual size remain
separate unfinished guest work; this host fix does not implement them.
