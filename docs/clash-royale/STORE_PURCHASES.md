# Original shop chest and Gold-pack purchases — October 27, 2026

This supersedes the chest/currency backlog in [DAILY_SHOP.md](DAILY_SHOP.md).
Scope: the pinned unencrypted ARMv7 2.1306.1 guest, continuing after `f8505aa`,
on `codex/clash-offline-continuation` / draft PR #138. No shared runtime, original
game assets, prices, reward tables or training behavior changed.

## Behavior and native evidence

The original client executes BuyChest (528) locally, validates arena, balance
and availability, debits Gems and creates its purchased chest. Its echo is base
metadata, **BE int 1**, then table-19 reference (decode `0x5aba0`). The guest
captures it as requested reward source **4**, location zero. The original
randomizer generates the reward; the original ClaimReward command applies it.
The guest never grants replacement cards or charges the price a second time.

Original `chestPurchased` (`0x126c08`) owns Home+`0x7c`; free is +`0x78` and
crown +`0x90`. Original ClaimReward (`0x8203c`) source 4 collects/clears the
purchased chest via `0x126328`. `reward_sources.inc` records this mapping.
Shop Silver/Golden/Magical chests at tested Arena 1 cost **20/60/290 Gems**.
Native arena scaling, card eligibility and original reward generation remain
the authority, rather than newly authored payout tables.

BuyResourcePack (529) is base metadata plus table-55 reference, **without a
selector** (decode `0xa42f8`, execute `0xa4310`). Original execute debits Gems
and credits paid Gold; the guest validates the complete turn and saves that
native result. The original three packs exchange **60/500/4500 Gems** for
**1000/10000/100000 Gold**. FreeGold does not increase on these exchanges.
The real-money Gems section has no offline billing products and appears empty;
this stage neither invents billing products nor qualifies real-money purchases.

## Persistence and validation

GRJ1 keeps its existing layout: five LE words (tag, payload length, source,
location, payload FNV), then immutable original command-213 bytes. Sources 1–3
retain their identities; source 4 extends that format with location zero.
The transaction saves the native Gem debit and purchased chest, with only that
chest's claim flag reverted in a temporary decoded checkpoint, together with
the immutable reward. The normal atomic checkpoint replacement commits them
together. After native reward success and purchased-chest removal, the guest
saves completed native state and clears the journal.

Before login migrations or writes, pending recovery now decodes the original
command without executing it. It requires complete consumption, server executor
(0,0), non-null reward, matching source/location and a corresponding unclaimed
chest in the decoded checkpoint. This catches source-header/command mismatches
despite GRJ1 not having a separate header checksum. It does not suppress native
decoder assertions on malformed data. Failed validation retains the original
file. No synthetic reward was introduced.

## Reproduce and assess the actual-client checks

The qualified host remains `/var/tmp/hvm-clan-core-work/build/hvm`, source
`912a87e`; shared runtime tree `9b29155c175d76eda08111072700403272a19d7e`.
Existing host qualification is fulltest 260/260, TestLab 18/18 and indexed
compatibility report `20261026T150956Z-912a87e64a33.json`. Guest-only changes
here do not constitute a fresh host or 16 KB iPhone qualification.

Build with `tools/build_clash_guest_mod.py` using the pinned fixture described
in the restoration document. Run the extracted app with
`tools/clash_guest_ui_probe.py --hvm <qualified-host> --app <extracted-app>
--home <isolated-home> --gpu --capture-audio --startup-seconds 12
--seconds 200 --plan <plan.json> --out <new-report-directory>`.
Run `python3 -m unittest discover -s tools/tests -p 'test_clash_guest*.py' -v`:
**37 tests pass**, including original wire schemas, malformed/truncated input,
canonical source identities and prior all-seven trainer fallback checks.

Use a private copy of the earned Arena-1 daily-shop save (Gold/FreeGold 75,
Gems 87, XP 8, King 2). Coordinates are logical 320x568:

- Shop `(30,540)`; Silver/Golden/Magical `(55/160/265,445)`; confirm `(160,330)`.
  Reveal successive rewards with `(160,300)`, stopping when the Shop returns.
  Extra reveal taps after it returns can reopen another product popup: those
  taps must not be mistaken for a second purchase or a successful home-tab tap.
- For Gold packs drag `(160,480)` to `(160,140)`, then product
  `(55/160/265,380)` and confirm `(160,330)`. Inspect screenshots and native
  save fields, not only captured-step counts.

Local evidence lives under `/var/tmp/hvm-clash-store-qa` and task `outputs/`:

- Actual Silver purchase: Gems 87→67, native Gold 75→94 and original cards;
  completed journal absent, purchased chest absent. Initial Silver was v1;
  the final v2 additionally qualifies Silver interrupted recovery below.
- Final v2 Golden purchase/reveal/relaunch: Gems 87→27, Gold 75→163.
  Final v2 Magical purchase/reveal/relaunch: isolated funded Gems 400→110,
  Gold 75→325, original card animations/rewards. The 400-Gem fixture is an
  explicit component fixture, not earned progress or an owner-save grant.
- Pouch purchase/relaunch: Gems 87→27, Gold 75→1075, FreeGold remains 75.
  Bucket 600→100 Gems / Gold 75→10075 and Wagon 5000→500 Gems / Gold
  75→100075 both retain FreeGold 75 and pass ordinary relaunch. These use
  isolated funded fixtures to exercise the higher costs;
  those grants never modify the continuing earned or phone-failing saves.
- Controlled process interruption **before** atomic replacement: the complete
  original economy remains unchanged; two ordinary relaunches do not grant a
  reward or debit Gems. **After** replacement: saved Gems 67, original
  unclaimed purchased chest and GRJ1 source 4 are present; two ordinary
  relaunches apply the immutable native reward once, remove chest/journal and
  preserve exact resulting economy. Independently decoded reward is 20 Gold,
  zero Gems and three card stacks; actual Gold delta is exactly 20.
- Fresh original Trainer Earl starts with zero wins and no shop seed/offers.
  Existing King-3 component clan opens Shop and clan conversation without
  changing human resources, cards, identity or level. Screenshots are retained
  in `regression-training` and `regression-clan`.
- Three isolated invalid pending fixtures (header/source mismatch, invalid
  source 5, missing source chest) abort with the original whole-file SHA256
  unchanged. Expected rejection is not a normal-play crash-free verdict.

Private helpers: `outputs/store_qa.py`, `store_more.py`, `store_recovery.py`,
`store_cut.c`, `store_guards.py`, `store_regression.py`. The interruption helper
preloads a **host-only** shim which delegates the real fsync/rename, stops only
the matching source-4 checkpoint before/after replacement, records the stopped
PID and lets the controller inspect and kill it. Ordinary recovery has no shim.
These are controlled process-kill tests, not physical power-loss/filesystem
durability guarantees. The alpha host's existing fsync error propagation needs
a separate shared-runtime TestLab-backed investigation; it was not changed here.
No binary, assets, checkpoint, funded fixture or host shim is committed.

Final candidate `/var/tmp/hvm-clash-store-v2.ipa` SHA256:
`471da685dcef140e29fecc801ca5d78b069dcb7f9dd3c48ddcf7c2cc4b7dc165`;
executable:
`6b9eb25cd4bc922c28de268eff1f9b180dabd6b243ec2231c1ef7736ec109485`.
Mixed audio capture is enabled in normal UI runs; it is not an audio dropout,
stutter, phone-performance or visual-fidelity pass.

## Next work and owner priorities

Keep all seven original training names, decks, AI and tutorial sequence intact.
The owner defers advanced/main-opponent AI refinement and modern-bot comparisons
until all other game features and bug fixes are ready. Existing bounded native
opponents stay in place. Next qualify later-arena progression and native
partial/draw results/profile accounting, then continue the full button,
save/lifecycle and iPhone audits. Preserve unresolved princess facing/presence,
cannon anchoring, health bars, scenery, text centering, audio dropout and stutter
reports; this shop stage does not close them or establish that every app works.
