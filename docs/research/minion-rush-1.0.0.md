# Minion Rush 1.0.0k (Gameloft, 2013)

Findings for the iOS 1.0.0 "with data" IPA (`com.gameloft.despicableme2`, `DespicableMe`, armv7).
Everything here comes from watching the game in HydrogenVM:
- the files it opens (`HVM_FILE_TRACE=1`);
- the names it compares (`HVM_TRACE_COMPARE=1`);
- the host calls in crash reports.

The game's code was not disassembled.

Status labels:
- **verified**: seen happen.
- **observed**: seen in data, effect not tested.
- **open**: not known yet.

## Data packs

- The IPA carries a `Container/Library/Caches/dlcs/` folder next to `Payload/`. The importer
  (`AppBundle.cpp`) copies it into the game's home (`Library/Caches/dlcs/`) without overwriting
  existing files. **verified**
- The folder holds:
  - seven `.jpk` packs: basic 103, residential 70, costumes 64, four fonts;
  - `mnhtn_index_ios`, which names `mnhtn_toc_ios_2004`;
  - the TOC itself (JSON);
  - `mnhtn_hash_file_ios_1647`.
- The game reads the TOC and walks `game_versions` until it matches its own version string
  `1.0.0k`. It walks past later versions (`4.9.0i`, `4.7.0h`, ... `1.1.0m`), and DLC names from
  later updates appear in the compares (`dlc_map_2_arctic`, `dlc_special_event_2_up32spy`, ...).
  Then it reads each pack's `metadata`, `index` and `active`. **verified**
- For each pack it opens `<pack>.lzma` with `fopen` and stats `<pack>.tmp` (both missing or empty).
  It then reads the `.jpk` directly: `models/*.pig`, `textures_ios/*.tga`, `shaders/*.xml` come
  from `dlc_basic_mandatory_ios_basic_103.jpk` and `dlc_background_residential_ios_residential_70.jpk`.
  **verified**
- The zero-byte `<pack>.lzma` files in the cache folder are created by the game on its first run.
  The IPA does not contain them. **observed**

## Where it stops (bench, 2026-11-15)

1. The intro cinematic plays (in-engine 3D, "Tilt to watch in full screen").
2. The "QUICK TIP ??? LOADING..." screen comes up and level loading starts:
   - the lab and house nodes, `resi_skydome`, `mount_001`;
   - `minion_rocket` with its textures;
   - `fx_special_*`, `fx_glow_df`, `fx_ndis_ice`.
3. Right after those, the game crashes, the same way on every run:
   - `bad memory access at 0x72f`, `pc DespicableMe+0x2822ce`, `lr +0x23ccad`;
   - `r3 = 0x72b`, `r2 = 0xbab9f00d`.

The last host calls before the crash are a tight loop of `calloc` (8 or 4 bytes),
`OSAtomicAdd32Barrier` (reference counts, one shared object at about 855 references) and `free`.
No Objective-C message, file open or string compare comes between the last texture load and the
crash.

## What it is not

- **A heap block reused too early (use-after-free):** with `HVM_HEAP_QUARANTINE=20000` (the last
  20,000 freed blocks are kept out of reuse) the crash is identical, registers included.
  **verified**
- **The std::string compare over-read fixed in #474:** that was an earlier, different crash.
- **A missing file:** nothing the game opens in this phase fails, except for:
  - the `.tmp` / `.lzma` download-state files;
  - `Documents/profile*`, `debugsettings`, `QADEBUG.txt`, `filesConfig.dat`;
  - `GLIVE_RES/oconf.bar` (a first run's save and config files).

Other things seen in the same runs, not linked to the crash:
- `NSClassFromString("CMMotionManager")` returns nil (HydrogenVM has no CoreMotion), called from
  `-[MotionManager init]`. **observed**
- `GKLocalPlayer` is also nil. **observed**

**open**: what computes `0x72b`. The next step needs instruction-level work (a CPU semantics bug
in this path would also explain it).

## Speed

- After the heap change in #475 (small blocks tracked without per-block maps), intro frames on
  the Linux bench went from about 4 s to about 1.2 s, and the crash point is reached in 226 s
  instead of 690 s. **verified**
- Before that change, `malloc`/`free`/`operator new`/`delete` took about half of the main
  thread's time.
