# Guest Mod Lab continuation — November 6, 2026

The owner now requests modding during restoration: an in-game editor for cards,
unit behavior, sizes/speeds and currencies. This explicitly supersedes earlier
"modding after playability" scheduling. Original seven-trainer content remains
protected. Advanced opponent AI refinement is still deferred. Work remains on
`codex/clash-offline-continuation`, draft PR #366; this is not merged into main.

## Implemented prototype

Original Settings now has **Mod Lab / Open editor**, replacing the unavailable
Facebook connection control. Before earned graduation it explains the unlock;
afterward it opens a guest-owned UIKit window. Close discards draft unit edits.
The separate Apply units and Save balances buttons make changes explicit.

The pinned catalog resolves 31 distinct summoned troop/building definitions
through native spell IDs and `LogicSpellData::getSummonCharacter`, not guessed
data-table numbers. Shared summoned definitions and projectiles share modifiers.
The current controls cover movement, attack and deployment speed, troop scale,
projectile speed, health, damage and attack range. Default is 100%; ranges are
10–800%, except troop scale 25–400%. Stationary movement and missing-projectile
controls are disabled. Attack/deploy speed changes invert the native interval.
Baseline fields are restored before applying a profile, preventing compounding.
Health/damage hooks retain original level scaling before applying percentages.
Training and friendly sectors restore original data; paid arena battles use the
active profile. Settings apply to both sides when they share a definition.

Gold/Gems edits use original native wallet operations and verify resulting
balances before capture. They require the actual home avatar, no battle, no
pending reward/name/friendly transaction, and enforce the native Gold cap and
editor bounds. Before the first modification, the current native checkpoint is
copied atomically to `Documents/ClashGuestOffline-v1.bin.before-mods`; later
edits retain that first backup. Do not delete or replace the earned/phone-failing
source saves when testing.

`Documents/ClashModLab-v1.bin` is separate from the native game checkpoint:
seven little-endian words (MOD1 magic, version 1, record count, enabled,
selected index, body FNV-1a, header FNV-1a), followed by count × eight uint32
percentages. The loader checks counts, both hashes, EOF and every bound. An
invalid optional profile is retained, mods are disabled, and the editor explains
the rejection; it does not abort the game. Explicit Reset draft + Apply archives
it as `.rejected` before a new profile is committed. An existing rejected archive
is never overwritten. Read/write/flush/fsync/rename failures do not claim success.

No original asset is modified. Native entry hooks require exact pinned prologue
bytes, and preserve original instructions through trampolines. No game assertion
or bounds check is removed. No shared runtime `src/` change is included.

## Verification and exact limits

Run focused tests from the repository:

```sh
python3 -m unittest discover -s tools/tests -p '*clash_guest*.py' -v
python3 tools/build_clash_guest.py /var/tmp/clash-client.ipa \
  /var/tmp/clash-modlab.ipa --build-dir /var/tmp/clash-modlab-build
```

**45 focused checks pass**, including freestanding ARMv7 compilation, original
spell IDs for buildings/shared units, integer scaling/input bounds, repeated
apply/restore behavior, training/friendly policy, level-scaled health and both
character/projectile damage, truncated/corrupt/out-of-range profiles, and
explicit recovery without overwriting retained files. Native-call component
tests are not visual gameplay qualification.

Private native UI evidence is under `/var/tmp/hvm-clash-modlab-qa` on this
computer. The host is the immutable `/var/tmp/hvm-clan-core-work/build/hvm`
(912a87e; shared source tree 9b29155c175d76eda08111072700403272a19d7e).
Use `tools/clash_guest_ui_probe.py --app <extracted .app> --home <copied home>
--gpu --capture-audio --startup-seconds 20 --seconds 240 --plan <plan.json>
--out <new directory> --hvm <host>`. Extract the IPA privately; do not re-import
over a source save. Follow UI_AUDIT.md for logical 320×568 coordinates.

Observed in the running original client, with reports recording no crash:

- `open-v4`: home → Settings → correctly labeled Mod Lab entry and editor.
- `balances` (v5): Gold/FreeGold 75→175, Gems 87→97 using the editor buttons;
  wins/losses, XP, King level and trophies unchanged.
- `stats-v5`: enable, Knight move 100→125%, scroll, Apply, Close. The profile
  contains the actual enabled 125% setting, other fields 100%.
- `reduce-v5`: process relaunch retains balances and tuning; editor buttons
  reduce Gold/FreeGold 175→75 and Gems 97→87. First backup still contains
  original Gold 75/Gems 87; its hash remains
  `c2927bd73f1780ccec70ca558c9f6968dd807e4ebd30a97c4dfb9be5eb09db38`.
- `reset-v6`: another relaunch retains the setting; Reset draft + Apply stores
  disabled, all-100% defaults.
- `corrupt-v6`: an isolated copy with a malformed optional modifier file boots
  home and opens the editor with mods OFF and a rejection message. The malformed
  bytes remain unchanged.
- `fresh-v6`: a new home reaches original Trainer Earl and the welcome tutorial;
  saved trainer wins remain zero. Screenshots were inspected, not only counted.
- `final-v7`: final UI text/layout cleanup; retained home opens Settings/editor,
  scrolls through all eight controls and closes without a recorded crash.

Final private candidate `/var/tmp/hvm-clash-modlab-v7.ipa` has archive SHA-256
`7768935eb28848e1ed019f4b975ef8ef232417313282d9d9cd51c745da0e3903`, executable
SHA-256 `00617bfac089346a83a39f603073e1dcfb4a092ae0089578de487e2a9e7b0daf`.

The initial failed v2 build guessed Cannon's data lookup incorrectly and hit a
native damage-types assertion. It was not published. The correction resolves
the original card's actual summoned-character reference. This is not an
assertion workaround. Early styling/label issues were also corrected; final
prototype artifacts are recorded with the next source commit.

**Still pending:** actual paid-battle visual/timing/stat comparisons for every
control, typed numeric input on current Hydrogen/iPhone, zero/cap wallet UI
scenarios, device lifecycle/audio/performance qualification. Linux captured
audio is not a listening test or a stutter verdict. The old host uses fallback
UIKit fonts, so this panel's appearance is not a final phone design verdict.

## Keyboard: owner symptom and source finding

November 7 follow-up: the shared fix is now in draft
[PR #381](https://github.com/lawsharley0-cyber/HydrogenVM/pull/381), commit
`53e6d8b60712acbcba448416f16ce0d735b794f8`, on `codex/uikeyinput-keyboard`.
It supports generic UIKeyInput responders and the NSString composed-range API
that actual native backspace exposed. Both corrected composed-range APIs use
host Unicode engines. Clean pristine main `76db694` passes 647 runtime checks
and 25 Test Lab cases; the integrated candidate passes 657 and 26 respectively.
Its full compatibility harness has no regressions. Native Clan chat inserts,
deletes, sends and saves `Latest QB`, then cleanly quits and retains it on a
separate relaunch with unchanged wallet/progression/source save. The latest
updater's notification-property implementation resolves the older main's
independently reproduced graceful-shutdown failure in those runs. The CLI
input driver is private testing instrumentation, absent from the production
source. Read `docs/KEYBOARD_COMPATIBILITY_2026-11-07.md` on that host branch.
GitHub/macOS compilation and actual iPhone keyboard/accessory presentation
remain separate qualification; this guest IPA alone cannot change the host
keyboard bridge. The earlier investigation details follow for provenance.

The owner confirmed the **keyboard does not appear**, rather than appearing
but dropping text. Latest fetched main is
`ff24df3601e282d283168c85b7aa52d7f202062a`. Its #303 bridge publishes requests
from UITextField/UITextView and `handleTextInput` rejects responders without
TextFieldData. Generic UIResponder::becomeFirstResponder does not publish a
request. The pinned Clash binary's `KeyInput` is a custom **UIView** subclass,
with its own `hasText`, `insertText:`, `deleteBackward`, keyboard traits and
UITextInput methods. Its Mach-O superclass binding at 0x004613f4 is UIView;
native `KeyInput::show` invokes superclass `becomeFirstResponder`.

This was a concrete compatibility gap and is addressed by the host candidate
above; actual on-device presentation is still pending. The older qualified host/feature branch has
a different generic UIKeyInput bridge. Do not blindly merge main or apply
SwiftUI focus-delay guesses. A read-only main snapshot/build is being prepared
at `/var/tmp/hvm-keyboard-ff24df3`. Next: add a minimal custom UIView/UIKeyInput
case that fails before the fix, preserve the existing delegate/full-text tests,
then support generic responders through the shared host bridge. Run Test Lab
and `hvm_tests fulltest` before/after any `src/` change and verify actual Clash
name/chat input. A phone still must verify actual keyboard presentation.

## Remaining requested editor work

Custom cards are **not implemented** by this prototype. Add stable append-only
custom card definitions, cloned native character/projectile references and
idempotent native ownership grants. Preserve custom IDs referenced in saves;
resetting tuning must not delete those definitions. Prefer guest Documents
overlays and original CSV parser/reference creation to guessed object copies.
Reused art must be labeled as such. Prove CSV redirection narrowly before using
it; do not intercept unrelated I/O or ship diagnostic forced results.

Independent weapon/projectile visual size also remains unimplemented. The
original projectile table has no visual Scale column; speed/radius controls
cannot honestly substitute for a render-scale hook. More currencies, elixir,
spawn counts/intervals, spell tuning and per-card variants need their own bounds,
save rules and native/visual verification. Do not add decorative nonfunctional
controls. The remaining vanilla progression/clan/full-button/visual/audio and
performance work in PLAYABILITY_2026-11-06.md is still open.
