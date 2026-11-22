package com.hydrogenvm.android;

import android.view.Surface;

/** The emulator core (libhydrogenvm.so, built from android/app/src/main/cpp). */
final class Native {
    static {
        System.loadLibrary("hydrogenvm");
    }

    private Native() {}

    /** Imports an .ipa or .apk into appsRoot. Returns {bundlePath, title, bundleId, problem}; problem is "" when it can run. */
    static native String[] importGame(String file, String appsRoot);

    /** Starts a game; returns a session handle for the calls below. */
    static native long start(String bundlePath, String homePath, boolean higherResolution, int displayShortSidePixels,
                             boolean unlockFrameRate, int virtualScreenHeight);

    static native void setSurface(long session, Surface surface);

    /** Width / height of the picture as shown (0 before the first frame). */
    static native float aspect(long session);

    /** phase: 0 began, 1 moved, 3 ended, 4 cancelled. (u, v): position within the picture, 0..1. */
    static native void touch(long session, int phase, float u, float v, int finger);

    static native void pause(long session);

    static native void resume(long session);

    static native boolean isRunning(long session);

    static native String exitMessage(long session);

    /** Lets the game save and close, then frees the session. */
    static native void stop(long session);
}
