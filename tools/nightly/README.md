# HydrogenVM Nightly Corpus Run

Runs every `.ipa` and `.apk` across one or more fixture folders through `hvm run --phone`, classifies each game, compares against the previous night, and writes `status.json`, `summary.md`, and per-game screenshots.

## Usage

### Run manually in WSL / Linux

```sh
# Single or multiple folders (positional or --fixtures)
bash tools/nightly/run_nightly.sh \
  /mnt/c/Users/<you>/Desktop/IPA \
  /mnt/c/Users/<you>/Desktop/APK \
  [--publish]

# Or invoke nightly.py directly against an existing build
python3 tools/nightly/nightly.py \
  --fixtures /mnt/c/Users/<you>/Desktop/IPA \
  --fixtures /mnt/c/Users/<you>/Desktop/APK \
  --hvm build/hvm
```

If two folders contain files with the same name, `nightly.py` qualifies them as `<folder>/<file>` (and `<folder>_<stem>.png` for screenshots) so game names stay unique across folders.

### Schedule on Windows (one command)

From PowerShell on the Windows workstation:

```powershell
powershell -ExecutionPolicy Bypass -File tools\nightly\install_windows_task.ps1
```

This registers a daily Task Scheduler entry at **03:17** with **WakeToRun** enabled, invoking `wsl.exe -e bash -lc ...` over `%USERPROFILE%\Desktop\IPA` (or `test_ipa` if `IPA` is absent) and `%USERPROFILE%\Desktop\APK` with `--publish`. Override defaults with `-IpaDir`, `-ApkDir`, `-Time`, `-Distro`, or `-NoPublish`.

`--publish` uses `gh auth` inside WSL — no tokens are stored in the repository or task definition.

### Sidecar files

Next to any `<game>.ipa` or `<game>.apk`:
- `<game>.taps`: `seconds x y` per line in portrait points (passed as `--tap x,y@seconds`)
- `<game>.skip`: presence skips the game for the night

## `status.json` Schema (G1 Compatibility List)

`nightly.py` writes `<state>/nights/<YYYY-MM-DD_HHMM>/status.json` and copies it to `<state>/latest.json`.

### Top-level object

| Field | Type | Description |
|---|---|---|
| `schemaVersion` | integer | Schema version (`1`). |
| `date` | string | Local ISO-8601 timestamp (`YYYY-MM-DDTHH:MM:SS`). |
| `commit` | string | 40-character Git commit SHA of the tested `HydrogenVM` build. |
| `seconds` | integer | Per-game run duration budget in seconds. |
| `games` | array | List of per-game result objects (see below). |

### Each entry in `games`

| Field | Type | Description |
|---|---|---|
| `game` | string | Unique game identifier (filename, or `<folder>/<filename>` if duplicated across folders). |
| `platform` | string | `"iOS"` (`.ipa`) or `"Android"` (`.apk`). |
| `engine` | string | Detected engine from `hvm --report` (e.g. `"Unity (Mono)"`, `"EA Blast"`, or `""`). |
| `status` | string | Internal nightly status used by `compare.py`: `"runs"`, `"no-picture"`, `"hang"`, `"crash"`, or `"load-fail"`. |
| `compatStatus` | string | G1 public compatibility status: `"Boots"`, `"No picture"`, `"Hangs"`, or `"Crashes"`. |
| `detail` | string | First line of crash/load error or `"no frames presented"` / `"did not stop when asked"`; `""` when `runs`. |
| `frames` | integer | Presented frames (`perf.framesPresented`). |
| `sound` | boolean | `true` when `mixer.audibleFrames > 0` or `audio.nonSilentFrames > 0`. |
| `firstFrameMs` | integer | Time to first frame in milliseconds (`perf.timeToFirstFrameMs`). |
| `missingApis` | array[string] | Sorted distinct missing APIs reported by `hvm`. |
| `defaultedApis` | array[string] | Sorted distinct defaulted/placeholder APIs reported by `hvm`. |
| `screenshot` | string | Local path to the final PNG screenshot, or `""` if none was written. |
