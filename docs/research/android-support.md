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
- **Engine drivers:** EA Blast (Dead Space), Ubisoft UAF (Rayman Adventures) and Unity (Alto's
  Adventure). Each driver plays its activity's lifecycle, renderer and touch calls.
- **Framework services (JavaServices.cpp):** android.os.Build, Context service names and the
  services (window/display metrics, telephony, connectivity: offline), SharedPreferences kept in
  `Library/shared_prefs/<name>.prefs`, the launch Intent, PackageManager, Locale, boxed
  primitives, reflection (FromReflectedMethod / ToReflectedMethod), direct byte buffers,
  JNIBridge interface proxies (Unity's Runnable / listener callbacks) and FMOD's Java audio
  device (fmodProcess into an AudioTrack).

## Signals (Signals.cpp)

- Linux numbers and sa_flags; faults still go through the iOS sigaction table (Darwin numbers).
- pthread_kill / tkill / tgkill run the handler on the target thread at its next safepoint: end
  of a time slice, return from a host function, or inside a host wait (mutex, condition,
  semaphore, futex, sleep, looper poll). Per-thread masks, sigsuspend. Boehm GC's stop-the-world
  (SIGPWR / SIGXCPU) in Unity's Mono depends on this.

## libandroid events (Looper.cpp)

- ALooper (per thread; wake, poll with idents or callbacks), AInputQueue and AInputEvent /
  AMotionEvent / AKeyEvent accessors (events fed by `pushTouchEvent`, ready for a NativeActivity
  driver), ASensorManager with one accelerometer (device lying flat).

## Surfaces

- An Android window surface is opaque: alpha is set to 1 before each present (games clear with alpha 0).

## CPU

- ARMv7 generic timer reads (CNTVCT/CNTPCT/CNTFRQ), which OpenSSL's startup probe uses.
- `svc #0` Linux system calls (Syscalls.cpp). cacheflush (Mono's JIT) drops only the blocks over
  the flushed range: blocks are indexed by page, a dropped block's first op re-dispatches
  (H_Stale) so links into it stay safe, and other threads are held only when decoded code
  really overlaps.

## Games

| Game | Engine | Status |
|---|---|---|
| Dead Space 1.2.0 | EA Blast, ES 1.1 | Plays: picture, ETC1 textures, sound, touch. |
| Rayman Adventures 1.0.0 | Ubisoft UAF, ES 2, OpenSL ES | Loading scene drawn, music and effects play; stays on "Connecting". Ubisoft's servers for it are gone and the game reportedly no longer works on real phones either ([TCRF](https://new.tcrf.net/Rayman_Adventures)); it never reaches its sockets. |
| Alto's Adventure 1.25 | Unity 4 + Mono, EGL, FMOD (Java audio) | Boots through Mono and Unity to its animated title screen (Linux bench). Touch through nativeInjectEvent. |
