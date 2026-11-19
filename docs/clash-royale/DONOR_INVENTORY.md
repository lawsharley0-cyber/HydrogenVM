# Persistent local NPC inventory provenance

The subsequent [reciprocal donation implementation](RECIPROCAL_DONATIONS.md)
now supplies real player-to-NPC UI receipts and replenishment. The remaining
sections preserve this prerequisite stage's format and regression evidence.

Continuation after `3c07405`, on `codex/clash-offline-continuation`. This prepares
reciprocal donations without making a spent card permanently unreceivable.
It does **not** implement the player's Donate button/NPC request conversation,
invitations, automatic stock supply, weekly statistics, bot variety or daily shop.
The guest remains ARMv7 2.1306.1 inside HydrogenVM, with its own offline transport
and save; no host Clash server is needed.

## Failure and correction

The previous fulfilled-human-request validator required the requested NPC card
to remain at stock0 forever. Receiving one of that card through original211
legitimately makes stock1. The old validator then aborts at login with
`fulfilled request differs from finite donor inventory/history`.

The new record conserves each original starter material:
`current stock = 1 + received - sent`. Historical fulfillment requires actual
sent history; it no longer freezes current stock at zero. Original native total
donations must equal the sum of sent **cards**, not weighted request capacity
(one common plus one rare is two cards). All six original card identities and
actual deck/collection extent are checked. Native inventory remains authoritative;
the ledger does not set a card's count, currency, XP, rank or level.

The six pinned starter IDs are Knight26000000, Archers26000001, Giant26000003,
Bomber26000013, Fireball28000000 and Arrows28000001. Each begins with one
material. Current production transfers increment sent only after verified
original203/211 changes. No production receive/refill path has been added yet.
Future supply must execute real native receipt and increment received in the
same checkpoint. New NPC card acquisition/upgrade rules need explicit schema
and validation work, rather than adopting arbitrary inventory as a new seed.

## File format and migration

GNI1 is a 100-byte extension in `Documents/ClashGuestOffline-v1.bin`, alongside
the human CRG1 and GDN1 native bodies. Seven little-endian u32 words /28 bytes:

| Word | Meaning |
| --- | --- |
| 0 | tag `0x31494e47` / GNI1 |
| 1 | version1 |
| 2 | exact row count6 |
| 3 | human native-body FNV |
| 4 | NPC native-body FNV |
| 5 | FNV of the72-byte row payload |
| 6 | FNV of the preceding24-byte header |

Six ordered rows follow, each three LEu32 words: card ID, cumulative sent,
cumulative received. Counters, resulting stock and aggregate sent are bounded
at one billion, without signed overflow/negative stock. FNV detects accidental
corruption and cross-record mismatches; it is not authentication.

GCL1 adds required-inventory bit `0x200`, alongside required-donor `0x100` and
the low-byte status. Inventory requires a donor and active/pending-leave/settings
clan status2..4. Deleting an entire required GNI1 rejects, rather than silently
reconstructing provenance. Unknown/duplicate/truncated records reject.
The NPC-body binding is checked after all extensions have loaded, allowing
extension order independence. Every native/inbox/conversation/ledger validation
still precedes any friendly migration or checkpoint rewrite.

Legacy GDN1-v1/v2 donors without GNI1 are adopted only when each original card
is stock0/1, there are exactly six cards, and native lifetime donations equal
the number of missing starter materials. The ledger derives sent=1-stock,
received=0 in memory. Migration does not refill anything and writes only through
the existing atomic checkpoint after full validation. Retired NPCs retain their
inventory history and cannot rejoin/refill on login. Whole-clan disband clears
the NPC and ledger together; a subsequently paid new clan receives its own seed.
Older guests reject the new required-child flag: **do not downgrade saves**.

## Reproduction and qualified scope

Use the pinned IPA/executable hashes in [DONATIONS.md](DONATIONS.md). Build:

```sh
python3 -m unittest discover -s tools/tests -p 'test_*clash_guest*.py' -v
python3 tools/build_clash_guest.py /var/tmp/clash-client.ipa \
  /var/tmp/hvm-clash-inventory-v2.ipa \
  --build-dir /var/tmp/hvm-clash-inventory-v2-build
python3 -m zipfile -e /var/tmp/hvm-clash-inventory-v2.ipa \
  /var/tmp/hvm-clash-inventory-v2-run
```

Candidate IPA SHA256:
`77d396c384c52b3f53b5ff0ce314ed1fca1026ce74479668fe4c87b2231dbab6`.
Executable SHA256:
`e0aa623669095cfd2dcaba19fc31a69f51f01884301e709b948f2402596ebd7b`.
The earlier v1-named private candidate differs only in handling a zero-valued
loaded checksum; final qualification uses v2. Neither is committed to Git.

Use immutable `/var/tmp/hvm-clan-core-work/build/hvm`, qualified912a87e,
unchanged shared source tree `9b29155c175d76eda08111072700403272a19d7e`.
There are no shared `src/` edits in this stage. Earlier fulltest260/260,
TestLab18/18 and compatibility results remain the runtime evidence; these
guest-only results do not constitute a new runtime/device/performance gate.

Private reproducible QA helpers are under the task's `outputs/inventory_qa.py`
and its read-only inspector dependencies. They copy sandboxes before mutation,
verify source bytes afterward, and never run normalized inspection files.
They are not distributed game assets or repository fixtures.

- `positive`: `/var/tmp/hvm-clash-inventory-qualification/summary.json` records
  joined legacy-v1, retired-v2 and common+rare legacy donor migration, two normal
  returns each, exact full card maps/currency/XP/levels/trophies/gems/NPC counters
  unchanged. Original component sources remain intact.
- `corruption`: `/var/tmp/hvm-clash-inventory-corruption-v2/summary.json` records
  fourteen exit70 rejections with each save byte-identical: header/row CRC,
  version/count, human/NPC binding, duplicate/truncated/missing-required GNI1,
  wrong card, negative/unearned stock, erased sent history and unexplained
  legacy stock2 with all applicable checksums recomputed.
- `supply_probe` and `supply_old_reader`: private build only, original211 on a
  detached NPC, Arrows stock0→1, sent1/received0→1/1, then one atomic checkpoint
  and intentional abort. Human snapshot, NPC rewards/counters and source remain
  unchanged. Default guest returns twice with stock1/history intact. A separate
  older-format copy retaining that exact native supply reproduces the old
  fulfilled-request login abort, unchanged. This is an injected **component
  fixture**, not a player donation, an earned supply source or completed UI.
- `crash_transfer`: private intentional-abort builds wrap the existing
  NPC-to-human native transfer immediately before/after `capture_native`.
  Evidence belongs in `/var/tmp/hvm-clash-inventory-transfer-crashes-v2`.
  The source's naturally elapsed request cooldown permits the original Request
  Cards dialog/default Archers. Pre-commit abort retains the unfulfilled request,
  old stocks and ledger; a default-guest return then fulfills exactly once.
  Post-commit abort retains the fulfilled request, both changed inventories and
  sent history; subsequent returns do not repeat the transfer or rewards.
  Both cases pass with exactly one transfer/reward after two normal18-second
  returns each; the immutable source remains unchanged. These deliberate guest
  aborts do not qualify filesystem power-loss recovery.

The26-test suite includes independent authored native-C format/conservation
vectors: exact LE writer bytes, every truncation/one-bit corruption and
checksummed semantic errors, including stock and aggregate-counter overflow.

An ordinary GPU UI probe on a separate legacy copy showed the actual native
Archers receipt toast, stock2→3 and request1/10, NPC stock1→0, Gold105→110,
XP1→2, with ledger Archers sent1 plus the previous Arrows sent1. Its first
screenshots remain0/10 before the five-second donor tick; review the final
image, not just `captured` counts. The existing cooldown-labelled plan was used
as input; its expected blocked-request label was stale after actual wall time
elapsed, and is **not** a passing cooldown-block scenario. The final-build
interrupted-transfer/relaunch trials exercise the same new transfer path.

No iPhone qualification, sound-quality pass, frame-time improvement or arena
visual correction is claimed. Component fixtures are synthetic King3 clan
states; original earned progression and phone-failing saves remain preserved.

## Next implementation

The private original524 probe succeeds on the actual NPC without tutorial
bypasses: result0, native request cooldown28800, immediate repeat3, and no live
save mutation. Implement a durable namespace3 NPC request using this native
command, then handle original14310 Donate messages with both actors, original
203/211 rewards/capacity checks, this inventory ledger and request donor proof
in one commit. Preserve trainers and the ordered names/decks/styles/daily-shop
work, then later-arena and full UI/audio/visual/performance/device qualification.
