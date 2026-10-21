# Local desktop testing

The Windows workstation has Ubuntu/WSL, WSLg, Clang, CMake, Ninja and Python
available. `tools/local_test.sh` builds the current local source without fetching,
resetting, or modifying the branch. GitHub Actions is not needed.

On this workstation, the task's `outputs/` folder contains Windows launchers:

- **Play Clash Royale.cmd**: build and open a live desktop window.
- **Open Any IPA.cmd**: choose another IPA, build and open its desktop window.
- **Test an App Folder.cmd**: select a folder and scan its IPAs/`.app` bundles.
- **Run Runtime Checks.cmd**: build and run the focused runtime suite.
- **Run Regression Probes.cmd**: run Clash and the three older-game probes.
- **Capture Startup Trace.cmd**: capture 30 seconds of trace, shaders, report and
  final screenshot in a separate diagnostic sandbox.

`HydrogenVM Local Test.ps1` resolves the Windows checkout, IPA and output paths
through `wslpath`. The launchers are local workstation files, not bundled IPAs.

## Desktop controls

Click and hold the left mouse button to touch/drag. Input is mapped from the
resizable, letterboxed window to the guest's screen points. Press **S** to save
a screenshot, **B** to background the guest, **F** to foreground it, and **Esc**
or close the window to request graceful save/stop. Relaunch with the same launcher
to reuse the sandbox. Guest audio now plays through the shared 44.1 kHz stereo
mixer and SDL. On Ubuntu/WSLg install `libpulse0` (`sudo apt-get install libpulse0`);
the viewer selects PulseAudio when `PULSE_SERVER` is set. Device initialization
failures are printed and saved in `audio.json`; video can still be investigated.
`--mute` disables audio; `--capture-audio` also saves delivered PCM as `audio.wav`.
These are options for `tools/desktop.py`. The Windows play launchers enable sound.

Audio has its own clock, independent of frame capture. An inherited local
datagram socket carries 2048-frame PCM blocks; bounded kernel/SDL queues and
three-block startup priming prevent unbounded lag. Slow consumers drop complete
blocks rather than blocking the guest. Session `audio.json` records device format,
received blocks, underruns and errors; engine `report.json.audio` records mixer
frames, non-silent frames, peak, transport drops and errors. These measurements
establish signal delivery, not a guarantee that every game sound is correct.

Every desktop session gets its own dated output directory with `runtime.log`,
`report.json`, `final.png`, and `viewer-last.png`. Pressing S adds screenshots.
The window remains open after a crash so its last frame and exit code can be
inspected. A stop watchdog reports and terminates a hung host rather than waiting
forever. Real guest errors still remain failures; intentional shutdown no longer
turns into a spurious Halt/HostError crash.

The pinned IPA checksum is checked on each launch. The first launch imports the
bundle; later launches run the installed `.app` directly. **The raw CLI IPA
importer replaces an app container**, so the viewer deliberately does not import
over an existing sandbox. A different IPA hash requires a different home. The
launcher also prevents two desktop/trace sessions from using the same state root.
Generic IPA launches use a separate sandbox under `/var/tmp/hvm-local/apps/`
for each client hash. They also record app/engine hashes, commit, dirty-tree state
and sandbox path in `session.json`. See `DEVELOPMENT_AND_RELEASE.md` for the
shared-runtime development loop, app support limits and iPhone packaging gates.

Workstation paths (Linux/WSL):

| Item | Location |
|---|---|
| Automatic WSL build cache | `/var/tmp/hvm-local/build-<checkout-key>` |
| Automatic WSL source mirror | `/var/tmp/hvm-local/source-<checkout-key>` |
| Legacy/custom build cache | `/var/tmp/hvm-clash-build` |
| Viewer Python + pygame 2.6.1 | `/var/tmp/hvm-local-python` |
| Persistent app container | `/var/tmp/hvm-local/home/com.supercell.scroll` |
| Regression IPA fixtures | `/var/tmp/hvm-fixtures` |

These are persistent WSL directories, not the volatile `/tmp` build used in some
early experiments. They may be overridden through `HVM_LOCAL_BUILD`,
`HVM_LOCAL_STATE` and `HVM_LOCAL_FIXTURES`. Logs/screenshots are copied directly
into the Windows task's `outputs/local-test/` folder. Deleting that output folder
does not delete the app sandbox.

Mounted Windows checkouts use a Linux-native source mirror and build cache by
default. The helper prints both paths and performs a fresh first build. Content
checksums include current uncommitted edits and prevent clock differences from
leaving stale objects. The original Git checkout and app saves are preserved.
Rsync is required for this path. Set `HVM_LOCAL_NATIVE_SOURCE=0` to use mounted
sources; an explicit `HVM_LOCAL_BUILD` also retains its existing source pairing.
This improves build IO; it does not establish a gameplay FPS improvement. The
live frame/input bridge is already in a Linux temporary directory.

## Linux entry points

```sh
bash tools/local_test.sh play /path/to/pinned-client.ipa /path/to/results
bash tools/local_test.sh app /path/to/another-client.ipa /path/to/results
bash tools/local_test.sh scan /path/to/app-collection /path/to/results
bash tools/local_test.sh check /path/to/pinned-client.ipa /path/to/results
bash tools/local_test.sh regress /path/to/pinned-client.ipa /path/to/results
bash tools/local_test.sh trace /path/to/pinned-client.ipa /path/to/results
```

The `play` command creates a Python virtual environment and installs pinned
pygame if needed. Build prerequisites must already be installed. A Linux desktop
or WSLg is needed for the visible window; automation can use
`SDL_VIDEODRIVER=dummy` and run `tools/desktop.py` with `--seconds` and
`--exit-on-stop`. Its optional `--tap X,Y@seconds` exercises the live input bridge.

The CLI preview interface uses `--frame-file` and `--input-file`. Preview frames
are atomic replacements at up to 10 Hz: `HVF1`, little-endian uint32 width/height,
float32 screen scale, and RGBA bytes. The regular input file receives newline
commands `touch PHASE X Y`, `background`, `foreground`, or `stop`. Phases are
0 down, 1 move, 3 up, and 4 cancel. No socket or guest networking is involved.

## Verification and current game limit

The local runtime suite passes **71 checks**. The viewer smoke test receives live
frames, delivers down/up through the file bridge, and shuts down with exit 0 and
`crashed=false`. A pre-existing file in the sandbox's Documents directory was
preserved across launch; this tests host sandbox retention, not game progression.

Clash now renders its loading artwork, logo, training-camp hint and 50% bar,
then shows a connection-error panel. The shared GLES2 interpreter executes a
bounded straight-line shader subset; all 18 captured shader sources compile.
This does not initialize native offline play. Slow startup can require longer
than the short scanner window to reach this artwork. NPC battles and offline avatar/home
persistence remain unverified. See `CLASH_ROYALE_RESTORATION.md` for the current
runtime commit, evidence and next blocker.

A separate **opt-in protocol diagnostic** can accept the pinned client's login
through loopback TCP. It is not enabled by the Play launcher and supplies no home
state or battles. See `CLASH_ROYALE_RESTORATION.md` for commands and evidence.
