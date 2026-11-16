# Android support: what's there

Status of Android (.apk) support. Each game brought up adds what it needed; `hvm android-imports
<apk>` lists what an APK's libraries import that nothing provides yet.

## Libraries (runtime/android)

| Android library | Where | Coverage |
|---|---|---|
| libc / libm (bionic) | Bionic.cpp, BionicExtra.cpp | Every import of the games below. Layout differences (stat, dirent, FILE, pthread types, errno) are translated; the rest binds to the iOS functions. |
| liblog | Bionic.cpp | Logging goes to the game log. |
| libGLESv1_CM / libGLESv2 | runtime/gl (shared with iOS) | ES 1.1 or ES 2 context, chosen by DT_NEEDED. ETC1 textures are decoded to RGB (Etc1.cpp). |
| libOpenSLES | OpenSLES.cpp | Engine, output mix, PCM buffer-queue players on the mixer, volume. No URI/asset players, MIDI or 3D. |
| libandroid | LibAndroid.cpp | AAssetManager / AAsset / AAssetDir, AConfiguration. Not yet: ALooper, AInputQueue, ANativeWindow, ASensor (NativeActivity games). |
| libEGL | — | Not yet (NativeActivity / Unity games). |
| libdl | — | Not yet: one library per game is loaded. |

## Java side (Jni.cpp, Java.cpp, App.cpp)

A host JNIEnv. Java methods are host code; an unimplemented one the library exports as a native is
called there. `HVM_JNI_TRACE=1` logs each method used.

- **Platform:** Context/Activity folders and package, File, ClassLoader, UUID, Environment,
  AssetManager / InputStream, AudioTrack. Analytics and ads SDKs answer quietly.
- **Engine drivers:** EA Blast (Dead Space) and Ubisoft UAF (Rayman Adventures). Each driver
  plays its activity's lifecycle, renderer and touch calls.

## CPU

- ARMv7 generic timer reads (CNTVCT/CNTPCT/CNTFRQ), which OpenSSL's startup probe uses.

## Games

| Game | Engine | Status |
|---|---|---|
| Dead Space 1.2.0 | EA Blast, ES 1.1 | Plays: picture, ETC1 textures, sound, touch. |
| Rayman Adventures 1.0.0 | Ubisoft UAF, ES 2, OpenSL ES | Runs without crashing, music and effects play, 22 draws a frame. The picture is white: a rendering issue still open. |
| Alto's Adventure 1.25 | Unity 4 + Mono, NativeActivity, EGL | Needs multiple libraries + libdl, libandroid input/window/looper, EGL. |
