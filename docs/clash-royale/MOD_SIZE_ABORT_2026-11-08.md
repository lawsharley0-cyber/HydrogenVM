# Mod Lab troop-size save abort — November 8, 2026

The owner reported an abort when saving troop changes. The native reproduction
is Settings → Mod Lab → Mods ON → Knight troop size 100→125% → Apply units.
Before this fix it aborts with `Mod Lab: invalid native scale`. The valid
modifier file was committed before Apply, so reopening that instance repeats
the abort. Do not delete the save or the modifier profile to work around it.

## Cause and correction

The pinned ARMv7 2.1306.1 getter `LogicCharacterData::getScale` at Thumb
`0xd4f3d` reads a **binary32 float** at `0x1cc`. Knight's native 1.0 has bits
`0x3f800000`; Archer's 0.87 has bits `0x3f5eb852`. The original CSV's integer
Scale column does not imply an integer native cache. The modifier code passed
these float bits into the checked integer scaler, correctly triggering its
100,000,000 bound. The defect was choosing the wrong scaler, not the guard.

`mods_math.inc` now has a separate checked binary32 size operation. Only size
uses it; movement/timing/projectile/range and level-scaled HP/damage retain the
existing checked integer arithmetic and floor behavior. Size rejects negative,
NaN and infinite native values, out-of-range percentages, and infinite results.
Restoration still writes the exact original bits; applying repeatedly starts
from the original and does not compound. Profile format and identity are
unchanged, so the valid percentage profile left by this crash needs no migration.
Training/friendly eligibility, pre-mod backups, custom definition registry and
save-prefix checks are unchanged.

The old native-data C mock initialized Scale to integer 100 and checked integer
150. That invalid representation hid this defect. It now uses real 1.0/0.87
bits, checks 1.5 exactly and verifies original-bit restoration. A separate test
covers 1.25, fractional Archer scaling, the 25–400% limits, zero, invalid native
representations and overflow. The integer bound was not raised or removed.

## Verification

Focused guest suite: **60 tests pass**, including ARMv7 freestanding compilation:

```sh
python3 -m unittest discover -s tools/tests -p '*clash_guest*.py'
python3 tools/build_clash_guest.py /var/tmp/clash-client.ipa \
  /var/tmp/hvm-clash-mod-size-fix-v1.ipa \
  --build-dir /var/tmp/hvm-clash-mod-size-fix-v1-build
```

Native runs use the clean current-main runtime built from `588eebb`,
`/var/tmp/hvm-forge-main-baseline/build/hvm`, with ordinary GPU, touch, audio
capture and graceful stop through `tools/clash_guest_ui_probe.py`. Use
`--ready-log 'GuestOffline: queued response=24101' --ready-timeout 180`
before startup settling, and a fresh output directory for every probe.
The source earned sandbox is read-only; each case uses its own full copy.

The original failing case remains at
`/var/tmp/hvm-mod-save-crash-qa/size-ui-home`; its old run log records the abort
and native registers `r4=0x3f800000`, `r5=125`. A copy with the corrected guest
boots home and opens Mod Lab with Mods ON and Knight size 125%. The exact
modifier file and pre-mod backup hashes are unchanged. Wins 9, losses 8,
trophies 59, Gold/free Gold 75, Gems 87, XP 8 and King level 2 remain intact.
Recovery output: `/var/tmp/hvm-mod-size-fix-qa/recovery/`.

A fresh ordinary touch Apply of Knight size 125% succeeds and a separate
process relaunch retains the modifier and progression. Evidence:
`/var/tmp/hvm-mod-size-fix-v2-qa/apply/` and `apply-relaunch/`.
A second native UI run selects Archer, enables Mods and raises every one of
the eight fields to 125%. Apply succeeds, the checksummed profile contains
exactly eight 125 values for Archer, and a separate process relaunch retains
progression. The captured editor visibly reports "Unit settings saved for the
next battle." Evidence: `archer/` and `archer-relaunch/` under the same v2 QA
root. These are Apply/reload checks, not an eight-field combat/visual audit.

Committed ordinary input plans are `tools/clash_guest/qa/mod-size-apply.json`
and `mod-archer-all-fields.json`. Both require a copied post-training home with
no existing modifier file (default Knight selected, Mods OFF). Use a fresh
copy for each plan, then run a second process against the same copy to verify
relaunch. Compare decoded progression and backup hashes, validate the modifier
checksums/values, and review screenshots; captured steps alone are not proof.

The corresponding private driver is `outputs/qa_mod_size_fix.py` in the task
root. Its first Apply attempt stopped because the private drag step omitted
its required `expected` metadata; the guest exited normally. That incomplete
attempt is retained separately and is not counted as an Apply pass.

Package under test: `/var/tmp/hvm-clash-mod-size-fix-v1.ipa`.
Archive SHA-256:
`6ab8baf1a742bf4bb5c20857a6943c99e2f9bf965a5984e9ba7cae4108df7b1f`.
Executable SHA-256:
`fedd9bc2dcc0cee9fb59d7aa966bb4f268ca67b852cf21707f3ce782af20e4f9`.
This supersedes the earlier `CardForge-cf8546d` download for troop-size edits.
Only guest code/tests/docs change; no runtime source or original assets change.
The earlier main runtime 682 fulltest/26 Test Lab baseline is reused, not
reported as a new run. Native reports still disclose disabled StoreKit.

## Limits and next work

These checks do not establish every modifier/card/phone interaction. Confirm
the corrected IPA on the owner's iPhone while preserving the instance's full
Documents folder, including FGR1 definitions and all linked save files. Do not
reset or delete the failing instance. Phone import/update behavior remains
separate from Linux profile recovery; this fix needs a guest update, not an
emulator rebuild.

Raw cache diagnostics also expose two pre-existing breadth gaps: the current
"Deploy speed" control modifies native LoadTime at `0x1bc` (getter `0xd51d1`),
while DeployTime is `0x1c0` (getter `0xd520d`). This patch preserves that behavior
rather than silently changing already-saved timing profiles. Correct the label
or introduce explicit compatible deploy/windup controls in a follow-up.
The Scale getter follows `0xe8` to an attached character before reading Scale;
Bomb Tower has such a reference. Native modifier breadth/attached visuals need
separate qualification. Do not call this a full visual-sizing fix. Independent
weapon/projectile sizing, the remaining Card Forge/UI audit, phone keyboard,
audio quality and stutter work remain open; preserve all seven original trainers.
