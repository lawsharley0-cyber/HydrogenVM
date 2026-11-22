# HydrogenVM for Android

The Android app around the same emulator core the iPhone app uses. It plays the classic 32-bit
iPhone (.ipa) and Android (.apk) games HydrogenVM supports on an arm64 Android phone running
Android 12 or later.

- `app/src/main/cpp/CMakeLists.txt` builds the core from `../../../../../src` with the NDK, plus
  SQLite (the official amalgamation, fetched and hash-checked at build time).
- `app/src/main/cpp/hvm_android.cpp` is the JNI bridge: one session per game. OpenGL ES runs on the
  phone's GPU through EGL (`src/runtime/gl/HostGLEgl.cpp`); each composed frame is turned upright
  and copied into a SurfaceView; touches map back to the game's points; AAudio plays the mixer.
- `app/src/main/java/com/hydrogenvm/android/`: the game list (import, per-game switches: Higher
  resolution, Fill the screen, Unlock frame rate) and the full-screen player.

Build: `.github/workflows/apk.yml` (manual), or locally with the Android SDK, NDK 27.2.12479018
and CMake 3.22.1: `cd android && gradle assembleRelease`. The release APK is signed with the debug
key so it installs by sideloading.

Testing without a phone: `hvm_android_test` (build the CMake project with `-DHVM_ANDROID_TEST=ON`)
drives the same session code from the command line on a device or an emulator (`adb shell`):
`hvm_android_test game.ipa <work dir> <seconds> [u,v@seconds ...]` imports, plays with scripted
touches and writes the last frame and the log; `--decode file.m4a` checks the AAC decoder. Debug
builds of the app also take `--es importPath <file>` and `--es play <bundle id>` on `am start`.
If the app dies during a game, its next start shows the game's log and the app's system log lines,
with a Share button.

Differences from the iPhone app, for now: no internet for games, and regular expressions use
std::regex (the NDK's ICU has none). AAC/M4A music is decoded by Android's own decoders
(MediaExtractor/MediaCodec); AAC inside CAF files isn't read yet.
