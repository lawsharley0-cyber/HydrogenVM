# Dead Space (Android, armeabi) — first Android game

The first Android title HydrogenVM runs. This copy is the 1.2.0 Amazon build, a 325 MB APK
with all the game data inside it.

## What the APK holds

- **One native library:** `lib/armeabi/libDeadSpace.so` (ARMv5TE + VFPv2, not stripped).
- **Libraries it needs:** libc, libm, libstdc++, liblog, libGLESv1_CM. It needs no EGL,
  OpenSL or libandroid.
- **A thin Java layer.** It creates the GLSurfaceView and calls the natives:
  `NativeOnCreate`, `NativeOnResume`, `NativeOnSurfaceCreated/Changed`, `NativeOnDrawFrame`
  and `NativeOnPointerEvent`. The native code calls back into Java for about 15 methods
  (AssetManager, InputStream, AudioTrack and EA's helper delegates).
- **Game data** in `assets/published`. The game reads it from the SD card at
  `Android/data/com.ea.deadspace/files/published`. That path uses a package name the APK
  itself doesn't have.

## How it runs (runtime/android)

- **`ElfLoader::loadElfShared`:** maps the library, applies R_ARM_RELATIVE, lists imports,
  and collects `.init_array`.
- **`Bionic.cpp`:** wherever Android's libc differs from iOS's (errno, open/fcntl flags,
  stat/dirent, 32-bit off_t, one-word pthread types, sem_t, `__sF`, ctype tables). Every
  other import binds to the iOS function. Arguments are read with AAPCS 64-bit alignment.
- **`Jni.cpp`:** a JavaVM/JNIEnv whose objects and methods are host code. A method with no
  host code that the library exports as `Java_*` is called there.
- **`App.cpp`:** the activity lifecycle, plus a host-GL surface (framebuffer 0 is remapped
  to it) and the frame loop. It also handles touches and AudioTrack through the mixer. The
  SD-card data folder is linked to the assets for whatever package name the game looks in.

## Status (bench)

`hvm run game.apk --gpu`:
- loads, shows the hint screen, takes taps, and plays the 3D intro;
- 25 rarely used GLES 1.1 OES entry points are still unimplemented.

The iOS app doesn't import APKs yet.
