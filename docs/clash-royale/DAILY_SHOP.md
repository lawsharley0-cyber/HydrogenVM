# Original daily card shop and unchanged training

October 27, 2026, continuation after `45f5a97` on
`codex/clash-offline-continuation` / draft PR #138. Daily card offers now use
the pinned 2.1306.1 client's original picker, prices, purchase limits, inventory
changes and UI. Calendar updates and purchases persist in its native home.
The guest remains independent of a host Clash backend or network service.

**Owner requirement, October 27:** preserve the entire original training
section: all seven names, decks, AI and tutorial sequence. Authored handles,
deck variation and fair-mana/reserve overrides apply only after training.
Shop initialization/refresh requires seven earned NPC wins and a non-training
arena. Training does not receive shop seeds, offers or calendar mutations.

## Native behavior and historical reference

`LogicClientHome::randomizeShopItems` (`0x1265d0`) selects the original common,
rare and epic cards through `getRandomSpell` (`0x12651c`) and
`isSpellOkForShop` (`0x1264a4`). It checks original NotInUse/arena/rarity data
and avoids duplicates. Normal days offer one of each rarity; native weekday 1
offers two of each. Actual Sunday rendering has six cards in two rows.

Original globals specify starting prices common 3, rare 40, epic 2,000 Gold;
growth percentages 120/140/180 and daily limits 50/20/10 respectively. Native
`LogicSpellData::getCostInShop` (`0x75498`) retains its own integer rounding and
minimum increase. No modern prices, rarity corrections, free Sunday Epic,
Magic Items, Evolutions or other later shop features are inserted.

The original UI here labels Mini P.E.K.K.A. Epic and displays its 2,000-Gold
price; the shipped CSVs remain authoritative for this pinned client.
Supercell's [December 14, 2016 announcement](https://supercell.com/en/games/clashroyale/blog/news/update-sneak-peeks/)
describes reducing shop Epics from 2,000 to 1,000 Gold. That supports retaining
the older starting price, rather than applying later pricing to this build.
The [current support page](https://support.clashroyale.com/hc/en-us/articles/49484889956251-Shop-Offers)
includes later items and gifts and is not this archived shop's specification.

`LogicClientHome::setShopValues` (`0x1269a8`) calls the native picker only when
the seed/day changes or items need initializing, then starts its original timer.
The existing native fields are seed `+0xc0`, weekday `+0xc4`, weekday-seen
`+0xc8`, timer `+0xcc`, DataSlot vector `+0xd8`, count `+0xe0`. Each slot holds
the original card pointer and number bought, not an invented inventory copy.

Live rollover initially exposed a visual lifecycle defect: native
`setShopValues` notifies inventory listeners **before** storing the new weekday.
The card list updated, but entering Sunday lacked its banner, and leaving
Sunday could retain it over Monday's three cards. `shop_view.inc` now runs the
original `ShopPage::refreshSpells/createItems` after the setter completes. When
leaving Sunday it first releases the owned `+0xd4` MovieClip through the same
deleting destructor used by the original page destructor, clearing ownership
before that call. The original routine recomputes header/card/scroll layout.
No text, assets, assertions or visibility rules are replaced to mask the issue.
Before/after live screenshots verify Sunday's heading appears immediately and
Monday's normal heading/layout returns, with the original next-day countdown.

Original BuyCard command **530**, execute `0xd58dc`, already executes through
the client's normal UI. The guest consumes its exact echo (base metadata,
fixed big-endian int 1, original card reference) and checkpoints the native
result; it never charges or adds a card a second time. Original command **532**
records weekday-seen and has base metadata only. Availability, material caps,
Gold/FreeGold debit, daily limit and buy sounds stay in the original routines.

The first purchase probe caught a strict guest decoder error: the selector was
mistakenly read as a VInt. Native encode/decode `0xd5d5c/0xd5d80` uses the fixed
BE-int methods at encoder/stream vtable slots `+0x1c/+0x48`. The corrected
decoder and independent wire vectors fix it; the rejected attempt did not
persist a purchase. No client assertion or bounds check was suppressed.

## Calendar, integrity and save compatibility

The **offline calendar policy** is UTC midnight, Sunday=1 through Saturday=7.
The retired service's precise reset time zone is not established by the pinned
data. Seed `1,000,000,000 + Unix day index` is a positive native integer and a
durable generation marker. The native RNG uses that seed to select daily items;
remaining seconds run to the next UTC midnight, using the existing measured
home-clock path. All local instances use the same day's deterministic pool.

Same-day returns retain card identities and bought counts. A forward date
change rerandomizes once for the current day, resetting only native shop sold
counts. It does not undo earlier purchased inventory, refund Gold, add rewards,
change gems/XP/trophies or grant missed days' offers. A backward calendar
retains the newer generation, preventing another reroll or free stock reset.

The native home already serializes all these fields. There is no separate shop
wallet or GSH extension: the existing CRG1 body checksum, atomic fsync/rename
and current body bindings protect them together with the player's inventory.
Legacy guest homes with an empty seed 0/-1 and no items initialize normally.
Foreign, nonempty shop generations without this policy's marker are rejected
and retained instead of guessing dates or wiping paid purchase counts.

Before **any** login migration or write, the original decoder validates saved
calendar/cardinality, real card references, nonduplicate offers, rarities and
native bought-count limits. Freshly generated offers also validate their arena
unlocks. A later demotion does not invalidate an already offered card/history.
Live refresh runs after the delivered home is associated and UI loading is
complete, inside the existing home capture path; pending rewards, initial-name
work and active friendly intents defer it. Paid battles never refresh home.

Older guests do not implement these purchase echoes or calendar updates.
Do not downgrade a continuing save. HydrogenVM host updates and this separately
installed guest IPA retain their existing distinct bundle/save ownership.

## Validation

```sh
python3 -m unittest discover -s tools/tests -p 'test_*clash_guest*.py' -v
python3 tools/build_clash_guest.py /var/tmp/clash-client.ipa \
  /var/tmp/hvm-clash-shop-v9.ipa --build-dir /var/tmp/hvm-clash-shop-v9-build
python3 -m zipfile -e /var/tmp/hvm-clash-shop-v9.ipa /var/tmp/hvm-clash-shop-v9-run
```

36 focused tests pass, including independent UTC/division/boundary/generation
vectors, backward-date preservation, strict training exclusion, original BE
purchase wire/truncation/bounds vectors, original banner ownership/refresh
ordering, native save/clan/friendly regressions
and ARMv7 import checks. The trainer fallback test now checks all seven IDs
against all three original CSV mana getters and each original reserve field,
even with a stale paid-mode flag. Name assignment retains original training,
human and clan behavior. The final build's 18 arena sectors and seven original
trainer sectors still match the independent private C++ oracle; its explicitly
analysis-only zero-card-UI-flag fixture limitation remains documented in
[ARENA_OPPONENTS.md](ARENA_OPPONENTS.md).

Actual local evidence in `/var/tmp/hvm-clash-shop-qa`:

- Normal earned-copy UI: Archers/Giant/Mini P.E.K.K.A., original countdown and
  confirmations. Five real common purchases cost 3/4/5/6/7 Gold, card count 3→8,
  next price 8 and 45 left. One rare purchase costs 40 Gold, Giant material
  2→3, next price 56 and 19 left. Gold/FreeGold 140→75, bought counts 5/1/0.
  Repeated returns retain these values, gems 87, XP 8, King 2, trophies 59,
  earned wins 9/losses 8. No earned player funding or rewards were injected.
- `epic-component-*`: explicitly isolated test funding sets Gold/FreeGold to
  2,000 only in a copied sandbox. Actual original Epic purchase costs 2,000,
  Mini P.E.K.K.A. material 1→2, bought count 1, next price 3,600, nine left;
  zero Gold and the purchase retain through return. This is component evidence,
  not an earned 2,000-Gold purchase or a change to the continuing player.
- `calendar-*`: exact candidate under a private **process-only host wall-clock
  fixture** two days earlier, with monotonic time unchanged. Original Sunday
  creates Goblins/Bomber/Musketeer/Valkyrie/Mini P.E.K.K.A./Witch. A real 3-Gold
  Goblin purchase completes. Removing that fixture returns to current Tuesday:
  three different offers and sold counts zero, while the bought Goblin and
  137-Gold balance remain. Another normal return retains them. Backward-date
  probing preserves the newer generation/counts/economy. No system clock was
  changed; this is not a physical two-day wait or device calendar verdict.
- `live-midnight-fixed`: unchanged final IPA starts shortly before staged
  Sunday midnight. A real Goblin purchase occurs before the boundary; while
  the Shop stays open, Monday replaces six cards with three, restores the
  normal header/layout and shows 23h59min. Material and 3-Gold debit persist.
  The earlier `live-midnight` data test passed but its screenshot exposed the
  stale banner; only the corrected visual run qualifies the live UI behavior.
- `final-v9-limit-*`: explicitly staged common sold count 50, no funded cards.
  Original UI grays the offer, shows its Sold out response and cannot buy it;
  wallet/materials remain unchanged. This does not claim 50 earned purchases.
- `final-v9-invalid-weekday` / `final-v9-invalid-purchase-count` /
  `final-v9-invalid-card-type`: actual final guest rejects a wrong weekday,
  common bought count 51 and a resource reference in a card slot with valid
  outer body/binding checksums. Each whole save remains byte-identical before
  migration. Card type is checked before any card-specific native getter.
- `training-final-v9`: final candidate displays original Trainer Earl and the
  welcome tutorial, wins 0, native seed -1/no offers and no shop-init log.
  `clan-final-v9`: existing component clan opens offers and conversation with its
  complete human economy, cards and clan identity unchanged.

Inspect screenshots, native stored values and app crash reports together;
the probe's captured-step count alone is not verification. Local private
helpers `outputs/shop_qa.py`, `shop_calendar.py`, `shop_test_clock.c`,
`shop_epic.py`, `shop_guards_v9.py`, `shop_training_v9.py`,
`shop_clan_v9.py`, `shop_oracle_v9.py` and `shop_live_midnight_final.py` record
exact procedures.
The clock shim, binaries, original assets, funded/clock/limit fixtures and saves
are private diagnostic artifacts outside Git and are never guest dependencies.
Every UI probe uses the unchanged qualified host, extracted app, exact isolated
home, `--gpu --capture-audio` and 12-second startup settling.

Final candidate IPA SHA256:
`5ac5b72f10a69b85231909f64f84425f91c7671115878d30ad08a8b7c861d670`;
executable:
`19c4cc3579a6c13af6caca10a35383969fcc2ef355e10cb7aa83adf8b4bc323e`.
The final explanatory comments do not alter the compiled payload. Shared
runtime tree remains `9b29155c175d76eda08111072700403272a19d7e`, with the prior
260/260 fulltest, 18/18 TestLab and complete compatibility report as the existing
host qualification. No shared `src/` or pinned assets changed in this stage.
Music and purchase/get-Gold effects have nonzero mixed audio captures; this is
not a dropout-free, stutter-free or new iPhone performance/audio-quality verdict.

## Next work

Daily **card** purchases are this stage's scope. Shop chest and currency-pack
purchases still need their guest command/reward paths and full UI/recovery
verification; this document does not qualify every Store control. Preserve
native costs, reward generation and pending-intent recovery when adding them.
Then continue arena-specific progression/bots, native draw/partial results and
profile counters, full button/device/save-lifecycle audits, original-version
visual comparison and the owner's princess/cannon/health-bar/scenery/text/audio/
performance queue. Training stays original throughout those changes.
