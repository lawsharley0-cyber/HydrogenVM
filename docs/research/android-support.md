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
| libdl | Linker.cpp | Every library of the APK's ABI folder is loaded in dependency order and bound across libraries (system libraries first, then load order, as Android's linker does). dlopen / dlsym / dladdr / dlerror; dlclose keeps libraries loaded. |

## Java side (Jni.cpp, Java.cpp, App.cpp)

A host JNIEnv. Java methods are host code; an unimplemented one the library exports as a native is
called there. `HVM_JNI_TRACE=1` logs each method used.

- **Platform:** Context/Activity folders and package, File, ClassLoader, UUID, Environment,
  AssetManager / InputStream, AudioTrack. Analytics and ads SDKs answer quietly.
- **Engine drivers:** EA Blast (Dead Space) and Ubisoft UAF (Rayman Adventures). Each driver
  plays its activity's lifecycle, renderer and touch calls.

## Surfaces

- An Android window surface is opaque: alpha is set to 1 before each present (games clear with alpha 0).

## CPU

- ARMv7 generic timer reads (CNTVCT/CNTPCT/CNTFRQ), which OpenSSL's startup probe uses.

## Games

| Game | Engine | Status |
|---|---|---|
| Dead Space 1.2.0 | EA Blast, ES 1.1 | Plays: picture, ETC1 textures, sound, touch. |
| Rayman Adventures 1.0.0 | Ubisoft UAF, ES 2, OpenSL ES | Title scene drawn, music and effects play; waits on "Connecting to server" (no Android sockets yet). |
| Alto's Adventure 1.25 | Unity 4 + Mono, NativeActivity, EGL | All five libraries load and bind. Needs EGL, libandroid input/window/looper and a Unity driver. |
