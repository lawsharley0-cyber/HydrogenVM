# Android gap ledger (25-APK corpus)

25 games scanned (`tools/android_gaps.py scan --hvm build/hvm --run 10`).

- **Before (`origin/main`):** 79 missing native functions, 0 corpus APKs reaching title screen under `--phone`.
- **After (`agent/518-android-breadth`):** 0 missing native functions, 0 missing Java methods, 4 corpus APKs reaching title/game screen under `--phone` (`com.xenris.liquidwarsos`, `dev.serwin.AnarchRE`, `net.asceai.meritous`, `org.pgnapps.pk2`).

## Top 15 gaps fixed (by games unblocked)

| Rank | Needed by | Symbol / Subsystem | Games |
|---:|---:|---|---|
| 1 | 6 | `sincosf` (`libm.so`) | `com.miloshpetrov.sol2.android`, `com.roguetemple.hyperroid`, `com.xenris.liquidwarsos`, `de.saschahlusiak.freebloks`, `io.anuke.mindustry`, `org.opensurge2d.surgeengine` |
| 2 | 4 | `__strlen_chk` (`libc.so` FORTIFY) | `com.roguetemple.hyperroid`, `net.asceai.meritous`, `org.opensurge2d.surgeengine`, `org.pgnapps.pk2` |
| 3 | 4 | `stderr` (`libc.so` data) | `com.roguetemple.hyperroid`, `org.opensurge2d.surgeengine`, `org.openttd.fdroid`, `org.petero.droidfish` |
| 4 | 4 | `stdout` (`libc.so` data) | `com.roguetemple.hyperroid`, `org.opensurge2d.surgeengine`, `org.openttd.fdroid`, `org.petero.droidfish` |
| 5 | 4 | `strtoimax` (`libc.so`) | `com.roguetemple.hyperroid`, `org.opensurge2d.surgeengine`, `org.openttd.fdroid`, `org.pgnapps.pk2` |
| 6 | 4 | `strtold` (`libc.so`) | `com.roguetemple.hyperroid`, `org.opensurge2d.surgeengine`, `org.openttd.fdroid`, `org.pgnapps.pk2` |
| 7 | 4 | `strtoumax` (`libc.so`) | `com.roguetemple.hyperroid`, `org.opensurge2d.surgeengine`, `org.openttd.fdroid`, `org.pgnapps.pk2` |
| 8 | 4 | `wcstold` (`libc.so`) | `com.roguetemple.hyperroid`, `org.opensurge2d.surgeengine`, `org.openttd.fdroid`, `org.pgnapps.pk2` |
| 9 | 3 | `SL_IID_RECORD` (`libOpenSLES.so`) | `dev.serwin.AnarchRE`, `org.opensurge2d.surgeengine`, `org.pgnapps.pk2` |
| 10 | 3 | `__FD_ISSET_chk` (`libc.so` FORTIFY) | `com.roguetemple.hyperroid`, `net.asceai.meritous`, `org.pgnapps.pk2` |
| 11 | 3 | `__FD_SET_chk` (`libc.so` FORTIFY) | `com.roguetemple.hyperroid`, `net.asceai.meritous`, `org.pgnapps.pk2` |
| 12 | 3 | `__open_2` (`libc.so` FORTIFY) | `net.asceai.meritous`, `org.opensurge2d.surgeengine`, `org.pgnapps.pk2` |
| 13 | 3 | `__read_chk` (`libc.so` FORTIFY) | `net.asceai.meritous`, `org.opensurge2d.surgeengine`, `org.pgnapps.pk2` |
| 14 | 3 | `__strchr_chk` (`libc.so` FORTIFY) | `com.roguetemple.hyperroid`, `net.asceai.meritous`, `org.pgnapps.pk2` |
| 15 | 3 | `alphasort` / `scandir` (`libc.so`) | `net.asceai.meritous`, `org.opensurge2d.surgeengine`, `org.openttd.fdroid` |

## Engines

| Engine | HydrogenVM | Games |
|---|---|---|
| Java + native (custom) | none | 12: com.roguetemple.hyperroid, com.xenris.liquidwarsos, de.saschahlusiak.freebloks, dev.serwin.AnarchRE, io.anuke.mindustry, name.boyle.chris.sgtpuzzles, net.asceai.meritous, org.mattvchandler.a2050, org.opensurge2d.surgeengine, org.openttd.fdroid, org.petero.droidfish, org.pgnapps.pk2 |
| Java only | none | 8: ch.logixisland.anuto, com.dozingcatsoftware.dodge, com.gpl.rpg.AndorsTrail, de.sudoq, io.github.lunchwine.spacetraderclassic, org.olgsoft.apipepanic, org.scoutant.blokish, org.secuso.privacyfriendly2048 |
| libGDX | none | 5: com.dozingcatsoftware.bouncy, com.miloshpetrov.sol2.android, com.shatteredpixel.shatteredpixeldungeon, com.unciv.app, de.sesu8642.feudaltactics |

## Games (after)

| Game | Engine | Missing imports | Hit in a run (Java / native) | Frames presented (`--phone` / 10 s) |
|---|---|---|---|---|
| ch.logixisland.anuto | Java only | 0 | 0 / 0 | 0 |
| com.dozingcatsoftware.bouncy | libGDX | 0 | 0 / 0 | 0 |
| com.dozingcatsoftware.dodge | Java only | 0 | 0 / 0 | 0 |
| com.gpl.rpg.AndorsTrail | Java only | 0 | 0 / 0 | 0 |
| com.miloshpetrov.sol2.android | libGDX | 0 | 0 / 0 | 0 |
| com.roguetemple.hyperroid | Java + native (custom) | 0 | 0 / 0 | 0 |
| com.shatteredpixel.shatteredpixeldungeon | libGDX | 0 | 0 / 0 | 0 |
| com.unciv.app | libGDX | 0 | 0 / 0 | 0 |
| com.xenris.liquidwarsos | Java + native (custom) | 0 | 0 / 0 | 584 (title/game screen) |
| de.saschahlusiak.freebloks | Java + native (custom) | 0 | 0 / 0 | 0 |
| de.sesu8642.feudaltactics | libGDX | 0 | 0 / 0 | 0 |
| de.sudoq | Java only | 0 | 0 / 0 | 0 |
| dev.serwin.AnarchRE | Java + native (custom) | 0 | 0 / 0 | 256 (title screen) |
| io.anuke.mindustry | Java + native (custom) | 0 | 0 / 0 | 0 |
| io.github.lunchwine.spacetraderclassic | Java only | 0 | 0 / 0 | 0 |
| name.boyle.chris.sgtpuzzles | Java + native (custom) | 0 | 0 / 0 | 0 |
| net.asceai.meritous | Java + native (custom) | 0 | 0 / 0 | 489 (title screen) |
| org.mattvchandler.a2050 | Java + native (custom) | 0 | 0 / 0 | 0 |
| org.olgsoft.apipepanic | Java only | 0 | 0 / 0 | 0 |
| org.opensurge2d.surgeengine | Java + native (custom) | 0 | 0 / 0 | 0 |
| org.openttd.fdroid | Java + native (custom) | 0 | 0 / 0 | 0 |
| org.petero.droidfish | Java + native (custom) | 0 | 0 / 0 | 0 |
| org.pgnapps.pk2 | Java + native (custom) | 0 | 0 / 0 | 587 (title screen) |
| org.scoutant.blokish | Java only | 0 | 0 / 0 | 0 |
| org.secuso.privacyfriendly2048 | Java only | 0 | 0 / 0 | 0 |

## Missing native functions

| Needed by | Function | Games |
|---|---|---|

## Missing Java methods (hit in runs)

| Needed by | Function | Games |
|---|---|---|
