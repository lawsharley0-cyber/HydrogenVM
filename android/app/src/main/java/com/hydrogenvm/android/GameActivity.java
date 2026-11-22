package com.hydrogenvm.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.pm.ActivityInfo;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.DisplayMetrics;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.FrameLayout;

/**
 * Plays one game full screen. The picture keeps its shape (black bars where it doesn't fill the
 * screen), turns with the game, and passes touches to it. Back asks the game to save and close.
 */
public class GameActivity extends Activity implements SurfaceHolder.Callback {
    private long session;
    private SurfaceView surface;
    private FrameLayout frame;
    private float shownAspect;
    private boolean finishing;
    private final Handler main = new Handler(Looper.getMainLooper());

    private final Runnable watch = new Runnable() {
        @Override public void run() {
            if (session == 0) return;
            if (!Native.isRunning(session)) {
                gameEnded();
                return;
            }
            float aspect = Native.aspect(session);
            if (aspect > 0 && Math.abs(aspect - shownAspect) > 0.001f) {
                shownAspect = aspect;
                // Hold the phone the way the game is drawn.
                setRequestedOrientation(aspect > 1 ? ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
                                                   : ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT);
                layoutPicture();
            }
            main.postDelayed(this, 250);
        }
    };

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        frame = new FrameLayout(this);
        frame.setBackgroundColor(Color.BLACK);
        surface = new SurfaceView(this);
        surface.getHolder().addCallback(this);
        frame.addView(surface, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT, Gravity.CENTER));
        frame.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> {
            if (r - l != or - ol || b - t != ob - ot) layoutPicture();
        });
        // The frame takes every touch itself (not left to fall through to the activity).
        frame.setOnTouchListener((v, e) -> onGameTouch(e));
        setContentView(frame);
        hideSystemBars();

        String bundleId = getIntent().getStringExtra("bundleId");
        DisplayMetrics m = getResources().getDisplayMetrics();
        int shortSide = Math.min(m.widthPixels, m.heightPixels), longSide = Math.max(m.widthPixels, m.heightPixels);
        // Fill the screen: a 320-point-wide screen shaped like this phone (the game may or may not use it).
        int virtualHeight = Games.flag(this, Games.FILL_SCREEN, bundleId) ? (int) (320f * longSide / shortSide) : 0;
        MainActivity.installCrashCatcher(getApplicationContext());
        // Until the game closes normally, a marker says it is running: if the app dies, the next
        // start shows the log (MainActivity.reportLastCrash).
        Games.writeText(Games.runMarker(this), getIntent().getStringExtra("title") == null ? bundleId : getIntent().getStringExtra("title"));
        session = Native.start(getIntent().getStringExtra("bundlePath"), getIntent().getStringExtra("homePath"),
                Games.flag(this, Games.HIGHER_RESOLUTION, bundleId), shortSide,
                Games.flag(this, Games.UNLOCK_FPS, bundleId), virtualHeight, Games.lastLog(this).getAbsolutePath());
        main.post(watch);
    }

    private void hideSystemBars() {
        WindowInsetsController c = getWindow().getInsetsController();
        if (c != null) {
            c.hide(WindowInsets.Type.systemBars());
            c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }

    /** Sizes the picture to the largest rectangle of its shape that fits, centred. */
    private void layoutPicture() {
        int w = frame.getWidth(), h = frame.getHeight();
        if (w <= 0 || h <= 0 || shownAspect <= 0) return;
        int pw = w, ph = Math.round(w / shownAspect);
        if (ph > h) { ph = h; pw = Math.round(h * shownAspect); }
        FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) surface.getLayoutParams();
        if (lp.width != pw || lp.height != ph) {
            lp.width = pw;
            lp.height = ph;
            lp.gravity = Gravity.CENTER;
            surface.setLayoutParams(lp);
        }
    }

    /**
     * Touches on the whole game screen (the picture and the black bars around it), in the frame's
     * coordinates; the picture's position within the frame turns them into 0..1 of the picture.
     */
    private boolean onGameTouch(MotionEvent e) {
        if (session == 0) return true;
        float ox = surface.getLeft(), oy = surface.getTop();
        float w = surface.getWidth(), h = surface.getHeight();
        if (w <= 0 || h <= 0) return true;
        int action = e.getActionMasked();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                int i = e.getActionIndex();
                int phase = (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) ? 0 : 3;
                Native.touch(session, phase, (e.getX(i) - ox) / w, (e.getY(i) - oy) / h, e.getPointerId(i));
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++)
                    Native.touch(session, 1, (e.getX(i) - ox) / w, (e.getY(i) - oy) / h, e.getPointerId(i));
                break;
            case MotionEvent.ACTION_CANCEL:
                for (int i = 0; i < e.getPointerCount(); i++)
                    Native.touch(session, 4, (e.getX(i) - ox) / w, (e.getY(i) - oy) / h, e.getPointerId(i));
                break;
            default:
                break;
        }
        return true;
    }

    @Override public void surfaceCreated(SurfaceHolder holder) {
        if (session != 0) Native.setSurface(session, holder.getSurface());
    }

    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        if (session != 0) Native.setSurface(session, holder.getSurface());
    }

    @Override public void surfaceDestroyed(SurfaceHolder holder) {
        if (session != 0) Native.setSurface(session, null);
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (session != 0 && !finishing) Native.pause(session);
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemBars();
        if (session != 0 && !finishing) Native.resume(session);
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        new AlertDialog.Builder(this)
                .setMessage("Close the game? It gets the chance to save first.")
                .setPositiveButton("Close", (d, w) -> closeGame())
                .setNegativeButton("Keep playing", null)
                .show();
    }

    private void closeGame() {
        if (finishing) return;
        finishing = true;
        main.removeCallbacks(watch);
        final long s = session;
        session = 0;
        new Thread(() -> {
            Native.stop(s);
            //noinspection ResultOfMethodCallIgnored
            Games.runMarker(this).delete();
            main.post(this::finish);
        }).start();
    }

    private void gameEnded() {
        final long s = session;
        session = 0;
        finishing = true;
        String message = Native.exitMessage(s);
        new Thread(() -> Native.stop(s)).start();
        //noinspection ResultOfMethodCallIgnored
        Games.runMarker(this).delete();
        if (message != null && !message.isEmpty())
            message += "\n\nLast lines of the log:\n" + Games.readText(Games.lastLog(this), 1500);
        if (message == null || message.isEmpty()) {
            finish();
            return;
        }
        new AlertDialog.Builder(this).setMessage(message).setCancelable(false)
                .setPositiveButton("OK", (d, w) -> finish()).show();
    }

    @Override
    protected void onDestroy() {
        main.removeCallbacks(watch);
        if (session != 0) {
            final long s = session;
            session = 0;
            new Thread(() -> Native.stop(s)).start();
        }
        super.onDestroy();
    }

    @Override public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }
}
