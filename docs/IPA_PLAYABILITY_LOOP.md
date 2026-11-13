# Making an IPA playable, without a phone

How an agent takes a 32-bit iOS game Lawson hands over and gets it playable in
HydrogenVM on the Linux bench, with no test results from his iPhone. It is the
procedure Claude used for PvZ, Minion Rush, the Talking apps, Pocket Ants and
the rest in November 2026. Read `AGENTS.md` first for the build, the gates and
the PR rules; this file is the per-game loop.

## 0. Ground rules

- Never commit an IPA, an extracted app, or any of its files. Keep them outside
  the repo (for example `~/bench/apps`).
- Fix the emulator, not the game. A fix should be what iOS really does, so every
  game that uses the same API gains. A per-bundle special case needs a reason
  in the code and in the PR, and is the last resort.
- Every fix gets a test in `tests/runner/TestRunner.cpp` (or `SupportTests.cpp`
  for registry contracts) that fails without it.
- No network leaves the emulator. A game that needs a server must work through
  the offline answers (section 6), never through a real connection.
- One game at a time, one branch per batch of fixes, a draft PR per batch.

## 1. Build once

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release   # see AGENTS.md for the compiler and packages
ninja -C build hvm hvm_tests hvm_support_tests
```

Never run two `ninja` builds at the same time on one build directory (stale
inline objects). A change to `UIKit.hpp`, `HostGL.hpp` or `Process.hpp`
rebuilds most of the tree (about 10 minutes). Do not run `tools/hvm_report.py`
without `--skip-build`: it reconfigures the build directory.

## 2. Unpack the IPA

```sh
name=MyGame_1.0                       # folder name: game and version
mkdir -p ~/bench/apps/$name
unzip -q MyGame.ipa -d ~/bench/apps/$name
APP=$(find ~/bench/apps/$name -maxdepth 3 -name '*.app' | head -1)
plutil -p "$APP/Info.plist" 2>/dev/null || python3 -c "import plistlib,sys,pprint;pprint.pprint(plistlib.load(open(sys.argv[1],'rb')))" "$APP/Info.plist"
```

Note `CFBundleExecutable`, `MinimumOSVersion`, the supported orientations and
whether the binary is encrypted (`python3 tools/verify_ipa.py` or
`tools/check_macho.py`). An encrypted (FairPlay) binary cannot run; say so and
move on. There is no decryption.

## 3. First launch

```sh
H=$(mktemp -d)          # a fresh home folder = a fresh install
./build/hvm run "$APP" --home $H --keep-going --run-for 60 --gpu \
    --report r.json --screenshot s.png > run.log 2>&1
```

Read, in this order:

1. `r.json`: `crashed`, `crashReport`, `loadError`, `missingFunctions`,
   `missingApis`, `unimplementedImports`, `stubHits`, `warnings`, `audio`,
   `mixer`, `display`, `perf`.
2. `run.log`: `[hvm:warn]` lines (missing classes and methods), anything the
   game itself prints, `Orientation:` and `Screen:` lines.
3. `s.png`: the screenshot. It is the portrait framebuffer in pixels (a
   landscape game is drawn sideways). Open it and look; a black or frozen
   picture is a bug to chase even when the report looks clean.

A crash or a missing function is the first thing to fix. Then a black screen.
Then a game that stops at a loading screen (section 7).

## 4. Play it on the bench

There is no phone, so drive the game from the command line and look at the
results.

- `--tap X,Y@SECONDS` taps at points (the screenshot's pixels divided by 2, in
  the portrait frame) at a time after launch. `--drag X1,Y1,X2,Y2@SECONDS`
  swipes. Several of each are fine.
- `--input-file FILE` reads typed text and keys while the game runs. Use
  `tools/bench/input_feed.py FILE 34:type:Lawson 37:backspace 38:return &`.
  The run log prints `Keyboard: shown/hidden` when a game asks for a keyboard.
- `--screenshot` saves the last frame; `tools/bench/contact_sheet.py --out DIR
  --from 12 --to 26 -- ./build/hvm run "$APP" --home $H --gpu --tap ...` saves
  every frame presented in a window and a 16-frame sheet. Use it for
  animations: frozen, skipping or wrong pictures show up at once.
- `--mic-voice START,SECONDS[,NOISE[,FROM]]` feeds a voice-like sound to the
  microphone. Give room noise (`0.01`) to match a real phone; `FROM` delays the
  microphone's first sound like an iPhone does.
- `--send SELECTOR@SECONDS` sends a message to the app delegate's view
  controller (or the delegate), to open a screen no tap reaches.
- Sound: `python3 tools/bench/audio_capture.py --log run.log -- ./build/hvm run
  "$APP" ...` prints when the game was audible. Tap a button and check a
  sound appears at that time. `--wav out.wav` saves it to listen to.

Work step by step: launch, screenshot, find the next button in the picture, add
a `--tap` at the right time, run again. Keep the command in your notes; it is
the reproduction for the PR.

### What "playable" means

A game is playable when all of these are true on the bench, from a fresh home:

1. It reaches its main menu and you can start a level / a round.
2. The picture is right: no missing or black textures, right way up, the
   whole screen used, text readable.
3. Input works: taps, drags, multi-touch and typing do what the game expects.
4. Sound effects and music play (`audio_capture.py` shows audible stretches
   when they should happen; the bench has no AAC decoder, so an AAC-only game
   may be silent here and fine on the phone - note it).
5. It keeps running for three minutes of play without a crash or a hang.
6. Saving works: quit (end the run), launch again with the same `--home`, and
   progress or settings are still there.
7. Speed: `perf` in the report shows the average frame under 33 ms; note
   anything slower.

## 5. Diagnosis tools

| Symptom | Switch |
|---|---|
| Anything audio | `HVM_AUDIO_TRACE=1` (queues, units, OpenAL, files) |
| A file the game cannot find, or a loader waiting | `HVM_FILE_TRACE=1` (every open and lookup, with errno) |
| GL errors, wrong textures | `HVM_GL_DEBUG=2`, `--gl-trace` |
| Views in the wrong place, missing UIKit pieces | `HVM_DUMP_VIEWS=1` |
| A hang | `--thread-liveness` (per-thread state in the report every second) |
| What the game is calling | `--trace` (every import call with its caller; huge, so pipe it through `grep` and `sort | uniq -c`) |
| Which code is looping | the caller address from `--trace`, then `llvm-objdump -d --triple=thumbv7-apple-ios --start-address=... --stop-address=... "$APP/<exe>"` |

Read the binary's imports and strings (`strings -a "$APP/<exe>" | grep ...`)
to see which frameworks and classes a game uses before guessing.

## 6. Online parts

Every game is offline: `SCNetworkReachability`, sockets and URL loading report
no connection, and nothing leaves the process. Games should take their offline
path (ads, analytics, social, leaderboards fail quietly). When a game waits
forever for a server, find what it waits for (`HVM_FILE_TRACE`, `--trace`): it
is usually reachability reporting "reachable" while every request fails, or a
download it polls for. The fix is to make the emulator's answer consistent and
offline, never to fake the server's content.

## 7. Stuck on a loading screen

1. `HVM_FILE_TRACE=1` and count repeated failed lookups: a file polled
   hundreds of times is what it waits for.
2. `--thread-liveness` and the `--trace` caller counts: which thread spins, on
   what.
3. Compare with what iOS does for that API with no network and a fresh install.

## 8. Fix, test, gate

For each fix:

1. Change the runtime. Write the test first when you can.
2. If you changed a function registered with `p.def<>` or `supportDef<>`, label
   it: `supportDef<&fn>(p, "_Symbol", SupportStatus::Real|Approx|Stub, "what it does")`.
   The audit check fails otherwise.
3. Run the gates (all must pass):

```sh
ninja -C build hvm hvm_tests hvm_support_tests
stdbuf -oL ./build/hvm_tests fulltest           # all pass
./build/hvm_support_tests
python3 tools/lint_stubs.py --baseline tools/stub_lint_baseline.json   # 0 new/changed
python3 -m pytest -q tools/tests
python3 tools/hvm_testlab.py run --hvm build/hvm --out build/testlab-report
python3 tools/must_launch.py --apps ~/bench/apps --jobs 3                # every listed game still launches
```

4. Rerun the game's reproduction and confirm the symptom is gone.
5. Once the game is playable, add it to the launch list:
   `python3 tools/must_launch.py --apps ~/bench/apps --only <folder> --record`.

`app/**` (Swift and Objective-C) cannot be compiled on Linux. For a change
there, run `gh workflow run ipa.yml --ref <branch>` and wait for it to pass
before asking for review.

## 9. PR and record

- Branch `codex/<game-or-topic>`, draft PR, body with: the symptom, the cause,
  the fix, the reproduction command, the gate results, and what could not be
  checked without a phone (GPU driver, microphone, headphones, real speed).
- Update `docs/COMPAT.md` (or the game's row there) with the game's state:
  playable / playable with notes / blocked and why.
- Do not merge your own PR (see `AGENTS.md`).

## 10. What only the phone can tell

The bench uses Mesa for GL, has no real microphone or headphones, and runs at a
different speed. When a fix touches those, say so in the PR so Lawson can check
it on the iPhone. His "Test All Games" export (report.json, logs/, play-logs/)
is the evidence from the phone when it comes.
