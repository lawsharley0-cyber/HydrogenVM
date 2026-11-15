# Groove Coaster Zero (Taito, 2012) offline

Findings for running the iOS 1.0.0 client (`jp.co.taito.groovecoasterzero`, armv7, IPA build
"ver:20120920-A") without its shut-down server, gczero.com. Everything here was worked out by
reading the IPA's files and by watching the game in HydrogenVM (requests, files it opens and
writes, names it compares replies against with `HVM_TRACE_COMPARE=1`). The game's code was not
disassembled. The IPA is only read, never changed.

Status labels: **verified** (seen happen), **observed** (seen in data, effect not tested),
**open** (not known yet).

## Server protocol (stand-in: `src/runtime/foundation/ServerStandIn.cpp`)

Request parameters are AES-encrypted by the game (`aesManager encryptCBC:`); the stand-in answers
by path only. Replies are XML; the root element's name is not checked. `code` 0 is success.

| Request | When | Stand-in reply | Status |
|---|---|---|---|
| `start.php` | launch | `code message en ja my_coin event_id taito_id m4a_path is_tutorial` | verified: accepted |
| `event.php` | launch | unreachable (connection fails) | verified: game logs an error and carries on. A successful reply (even `code` 0 and `event_id` 0 only) sends the game to a "Download : n/3 [0%]" screen that never finishes: an event comes with three pak downloads (`stage_pak model_pak tuneFile_pak`, format open) |
| `shop_list.php` | launch | unreachable | verified: with any successful reply (even empty lists) the game stops on a "Download" screen; when unreachable it carries on |
| `sync.php` | after TOUCH TO START | `code my_coin friend_num m4a_path cm_stage buy_id play_id friend_info` | verified: accepted; `my_coin` shows on the menu |
| `<m4a_path><name>.m4a.zip` | starting a song | a zip holding `<name>.m4a` (see below) | verified |
| `result.php`, `save.php`, `score.php`, `load.php`, ... (listed in the bundle's `setting.cfg`) | after play, saving, events | none yet | open |

Names the game looks up in `shop_list.php` / `sync.php` items (nesting not yet worked out):
`item id gp badge description cm_description marquee product_id num title artist icon_no
play_time difficulty bpm model_pak tuneFile_pak stage_pak coin stage avatar`; localized texts
are looked up as `en` / `ja`. **open**: structure, and what `buy_id` / `play_id` must contain.

The "通信をキャンセルしました" ("connection cancelled") line after `sync.php` is the game cancelling
its own earlier connection before starting the next one. **verified**, harmless.

## Song music download

Seen in HydrogenVM with a generated test tone (`plot3_bgm.m4a`, 93 s 440 Hz AAC, 128 kb/s):

1. The game logs `LoadSoundData_BGM:plot3_bgm.m4a, 0`, builds
   `ConvertURL="<m4a_path>plot3_bgm.m4a.zip"` and downloads it with NSURLConnection.
   **verified**
2. Saved as `Library/Caches/plot3_bgm.m4a.zip` (with `m4a_path` "0" the local name was
   `0plot3_bgm.m4a.zip`: the local name is the address minus the scheme/host part the game
   strips). **verified**
3. Extracted with the bundled minizip ("HetimaUnZip") to `Library/Caches/plot3_bgm.m4a`.
   A zip holding one entry named `plot3_bgm.m4a`, **stored (no compression)**, is accepted.
   **verified**. Deflate, a folder inside the zip, or a different entry name: **open**.
4. Next the game downloads `<m4a_path>plot3_shot.m4a.zip` (the song's hit sound, named in the
   chart) the same way. Without it the song does not start ("失敗", failure). **verified**
5. Both files cached: the song starts from the debug menu and plays against the chart in the
   bundled `stage.pak`; the chart advances with the music. **verified** on the bench after
   OpenAL `AL_SAMPLE_OFFSET` was implemented (the game reads it as a float to time the chart),
   and **verified on an iPhone** (iOS 27.3, 2026-11-15): download, unzip, the tone heard, the
   track moving, smooth.
6. Played to the end (bench), the song shows its RESULT screen (GREAT / COOL / GOOD / MISS,
   MAX CHAIN, SCORE, RATING, LEVEL). Started from GAMEDEBUG, the game sends no result or save
   request. **verified**

Not checked yet: sample rate other than 44.1 kHz (the debug menu offers a "44.1kHz" setting),
mono vs stereo, the duration the game expects (the song list says "(1:33)"), and whether a
cached file is ever re-validated (no checksum or size was requested by the game in these runs).

Player-supplied files go in `Stand-in Files/gczero.com/` at the top of HydrogenVM's Documents
(Files app: On My iPhone > HydrogenVM > Stand-in Files > gczero.com). Not the game's own
Documents: the game clears that at launch (**verified**: a test file put there was gone after
one run). A file named `<name>.m4a.zip` is sent unchanged; a bare `<name>.m4a` is wrapped in a
stored zip by the stand-in. HydrogenVM ships no music.

## Bundled data (1.0.0 IPA)

| File | Contents | Status |
|---|---|---|
| `stage.pak` (7,278,147 bytes) | charts: `<song>_easy.dat`, `_normal.dat`, `_hard.dat` per song, e.g. `plot3_easy.dat`. Chart records name their music (`plot3_bgm`) and hit sound (`plot3_shot`). About 44 `_easy` charts, more than the 14 songs in the menu | observed (plot3 charts verified by playing) |
| `tuneFile.pak` (8,108,149 bytes) | song list and jackets: e.g. "Music Plot Type Three", COSIO, "(1:33)", one number byte 0x29 (41), the three chart names; other songs also name a `<name>_sample` preview | observed |
| `model.pak`, `skin.pak` | 3D models, UI skin | observed |
| `Library/Caches/<pak>` | the game looks there first for each pak (downloaded updates) | verified (opens attempted) |

Pak layout (big-endian): u32 file size - 4, then header words, then an index of 13-byte entries
from offset 0x1c (u32, u32 data offset, u32 size, u8 0), data contiguous. The first field
matches the name position for the first two entries only, so names are not yet mapped to
entries reliably. **open**.

## Songs (music names from `stage.pak`; titles from the official 2012 soundtrack)

`plot3_bgm` (Music Plot Type Three), `eclipse_bgm` (Eclipse Landscape), `sleep_bgm` (Sleep),
`freestyle_bgm` (Freestyle Beats), `planet_bgm` (Planet Connection), `inv2003_bgm`
(Invader Disco), `shadow_bgm` (Shadow), `kannan_bgm` (Kannanshinku), `star_bgm`, `hosoe_bgm`,
`kiki_bgm`, `niji_bgm`, `backon_bgm`, `treeclimb_bgm` (title matches for the last six are
**open**). Each also needs `<name minus _bgm>_shot` if its chart names one.

## Song menu (after TOUCH TO START)

The menu shows the first song ("Music Plot Type Three", COSIO, (1:33)) dimmed and does not
answer taps, on the bench and on an iPhone, with the stand-in replies above. **open**. Not
changed by: listing songs in `sync.php`'s `buy_id` / `play_id` (`my_coin` does show), the song's
music being cached, the "connection cancelled" line (harmless). After `sync.php` the game looks
up `my_coin friend_num m4a_path cm_stage buy_id play_id coin gp badge cm_description description
friend_info en product_id num title artist icon_no play_time difficulty bpm stage_pak model_pak
tuneFile_pak`, then names that are not part of any reply (pieces of the game's own symbol
strings), so the lookup list runs past its end. No UIKit dialog is involved (the menu draws in
GL only).

## Song files (bench sweep, 2026-11-15)

Every song in the GAMEDEBUG picker was started with generated test tones; each one asked the
stand-in for exactly these files (`<name>.m4a.zip`, served from `Stand-in Files/gczero.com/`)
and started without a crash. **verified** unless marked.

Zero's own 14 songs (lower case):

| Music | Hit sound | Notes |
|---|---|---|
| `plot3_bgm.m4a` | `plot3_shot.m4a` | Music Plot Type Three; played to the RESULT screen; also verified on an iPhone |
| `eclipse_bgm.m4a` | `eclipse_shot.m4a` | Eclipse Landscape |
| `sleep_bgm.m4a` | `sleep_shot.m4a` | Sleep |
| `shadow_bgm.m4a` | `shadow_shot.m4a` | Shadow |
| `kannan_bgm.m4a` | `kannan_shot.m4a` | Kannanshinku |
| `inv2003_bgm.m4a` | `inv2003_shot.m4a` | Invader Disco |
| `hosoe_bgm.m4a` | `hosoe_shot.m4a` | |
| `niji_bgm.m4a` | `niji_shot.m4a` | |
| `treeclimb_bgm.m4a` | `treeclimb_shot.m4a` | |
| `backon_bgm.m4a` | `backon_shot.m4a` | |
| `freestyle_bgm.m4a` | `freestyle_shot.m4a` (observed in `tuneFile.pak`, not yet requested in the sweep) | Freestyle Beats |
| `kiki_bgm.m4a` | `kiki_shot.m4a` (observed, not yet requested) | |
| `star_bgm.m4a` (observed) | `star_shot.m4a` (observed) | not reached by the sweep |
| `planet_bgm.m4a` (observed) | not known | Planet Connection; not reached by the sweep |

The picker also lists the original Groove Coaster's songs, whose files use other names and
upper case: `8bit_BGM / 8bit_SHOT`, `Acid2_BGM / Acid2_SHOT`, `Aprils_BGM / Aprils_SHOT`,
`Arkanoid_BGM_1 / Arkanoid_SHOT_1`, `Cardiac_BGM_1 / Cardiac_SHOT_2`,
`Contemporary_BGM_1 / Contemporary_SHOT_2`, `Dreamer_BGM / Dreamer_SHOT`,
`DrumnBass_BGM / DrumnBass_SHOT`, `GBME_BGM_1 / GBME_SHOT_2`, `GEKI_BGM_1 / GEKI_SHOT_2`,
`HELLO31337_BGM / HELLO31337_SHOT`, `Happy_BGM_1 / Happy_SHOT_2`, `Hiphop_BGM_1 / Hiphop_SHOT_2`,
`House_BGM_1 / House_SHOT_2`, `ICNA_BGM / ICNA_SHOT`, `INVGENEMIX_BGM / INVGENEMIX_SHOT`,
`InvadeYou_BGM_1 / InvadeYou_SHOT_2`, `JET_BGM_1 / JET_SHOT_2`, `JNGBELL_BGM / JNGBELL_SHOT`,
`MUSICREV_BGM / MUSICREV_SHOT`, `Neptune_BGM_1 / Neptune_SHOT_2`, `PlanetRock_BGM / PlanetRock_SHOT_FIX`,
`Poly_BGM / Poly_SHOT`, `SROCK_BGM / SROCK_SHOT`, `ShtStr_BGM / ShtStr_SHOT`, `Ska_BGM / Ska_SHOT`,
`Spacefunk_BGM_1 / Spacefunk_SHOT_2`, `TRIP1_BGM_1 / TRIP1_SHOT_2`, `Visio_BGM / Visio_SHOT`
(all `.m4a`). The stand-in log line ("no song file X") names any file still missing.

## How to reach a song quickly

The title screen has a **GAMEDEBUG** button (top left): a debug menu with song, difficulty and
sample-rate pickers and START. That starts a song directly, without the shop or menu. Its song
picker lists every song in `tuneFile.pak`, including original Groove Coaster songs that are
not in Zero's menu; their files are named differently (e.g. `PlanetRock_BGM.m4a` and
`PlanetRock_SHOT_FIX.m4a`), and the stand-in log line names each file a song asks for.
