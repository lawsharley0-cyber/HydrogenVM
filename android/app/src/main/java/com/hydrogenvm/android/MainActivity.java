package com.hydrogenvm.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.ProgressDialog;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.List;

/** The game list: import an .ipa or .apk, tap a game to play, long-press it for its settings. */
public class MainActivity extends Activity {
    private static final int PICK_GAME = 1;

    private ArrayAdapter<Games.Game> adapter;
    private TextView empty;
    private final Handler main = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        installCrashCatcher(getApplicationContext());
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (16 * getResources().getDisplayMetrics().density);
        root.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText("HydrogenVM");
        title.setTextSize(26);
        title.setTextColor(Color.WHITE);
        root.addView(title);

        TextView hint = new TextView(this);
        hint.setText("Classic 32-bit iPhone and Android games. Import a game file you own (.ipa or .apk). "
                + "Tap a game to play; touch and hold it for its settings.");
        hint.setPadding(0, pad / 2, 0, pad / 2);
        root.addView(hint);

        Button importButton = new Button(this);
        importButton.setText("Import game");
        importButton.setOnClickListener(v -> pickGame());
        root.addView(importButton);

        empty = new TextView(this);
        empty.setText("No games yet.");
        empty.setGravity(Gravity.CENTER);
        empty.setPadding(0, pad * 2, 0, 0);
        root.addView(empty);

        ListView list = new ListView(this);
        adapter = new ArrayAdapter<Games.Game>(this, 0) {
            @Override public View getView(int position, View convert, ViewGroup parent) {
                return gameRow(getItem(position), convert);
            }
        };
        list.setAdapter(adapter);
        list.setOnItemClickListener((parent, view, position, id) -> play(adapter.getItem(position)));
        list.setOnItemLongClickListener((parent, view, position, id) -> {
            settings(adapter.getItem(position));
            return true;
        });
        root.addView(list, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        setContentView(root);
        refresh();
        reportLastCrash();
        testHooks(getIntent());
    }

    /**
     * Debug builds only, for testing from a computer: "importPath" imports a file the app can read,
     * "play" starts the imported game with that bundle id.
     */
    private void testHooks(Intent i) {
        if ((getApplicationInfo().flags & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) == 0 || i == null) return;
        String path = i.getStringExtra("importPath");
        if (path != null) importGame(Uri.fromFile(new File(path)));
        String play = i.getStringExtra("play");
        if (play != null) for (Games.Game g : Games.list(this)) if (g.bundleId.equals(play)) play(g);
    }

    private final java.util.Map<String, Bitmap> icons = new java.util.HashMap<>();

    /** One row: the game's icon (made from its own icon files the first time) and its title. */
    private View gameRow(Games.Game g, View convert) {
        float dp = getResources().getDisplayMetrics().density;
        LinearLayout row = convert instanceof LinearLayout ? (LinearLayout) convert : null;
        ImageView icon;
        TextView label;
        if (row == null) {
            row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setPadding((int) (4 * dp), (int) (8 * dp), (int) (4 * dp), (int) (8 * dp));
            icon = new ImageView(this);
            icon.setScaleType(ImageView.ScaleType.FIT_CENTER);
            row.addView(icon, new LinearLayout.LayoutParams((int) (56 * dp), (int) (56 * dp)));
            label = new TextView(this);
            label.setTextSize(18);
            label.setPadding((int) (14 * dp), 0, 0, 0);
            row.addView(label);
        } else {
            icon = (ImageView) row.getChildAt(0);
            label = (TextView) row.getChildAt(1);
        }
        label.setText(g.title);
        Bitmap b = icons.get(g.bundlePath);
        if (b == null && g.iconFile().isFile()) {
            b = BitmapFactory.decodeFile(g.iconFile().getAbsolutePath());
            if (b != null) icons.put(g.bundlePath, b);
        }
        if (b != null) icon.setImageBitmap(b);
        else icon.setImageResource(R.drawable.icon);
        return row;
    }

    /** Makes icons for games imported before icons were kept, off the main thread. */
    private void makeMissingIcons(List<Games.Game> games) {
        new Thread(() -> {
            boolean made = false;
            for (Games.Game g : games) {
                if (g.iconFile().isFile()) continue;
                made |= Native.writeIcon(g.bundlePath, g.iconFile().getAbsolutePath());
            }
            if (made) main.post(() -> adapter.notifyDataSetChanged());
        }).start();
    }

    /** If the app died during a game (or crashed anywhere), show what happened and offer to share it. */
    private void reportLastCrash() {
        File marker = Games.runMarker(this), appCrash = Games.appCrash(this);
        if (!marker.isFile() && !appCrash.isFile()) return;
        // Reading the logs takes a moment: off the main thread, then shown.
        new Thread(() -> {
            String report = buildCrashReport();
            if (!report.isEmpty()) main.post(() -> showReport(report));
        }).start();
    }

    private String buildCrashReport() {
        File marker = Games.runMarker(this), log = Games.lastLog(this), appCrash = Games.appCrash(this);
        StringBuilder report = new StringBuilder();
        if (marker.isFile()) {
            report.append("HydrogenVM closed while playing ").append(Games.readText(marker, 500).trim()).append(".\n\n");
            report.append("Last lines of the game's log:\n").append(Games.readText(log, 12000)).append("\n");
            report.append("System log for HydrogenVM:\n").append(systemLog());
            //noinspection ResultOfMethodCallIgnored
            marker.delete();
        }
        if (appCrash.isFile()) {
            report.append("\nApp error:\n").append(Games.readText(appCrash, 8000));
            //noinspection ResultOfMethodCallIgnored
            appCrash.delete();
        }
        return report.toString();
    }

    /** The app's own lines in the system log (Android lets an app read its own), including native crash lines. */
    static String systemLog() {
        try {
            Process p = Runtime.getRuntime().exec(new String[]{"logcat", "-d", "-t", "400", "-v", "brief"});
            // InputStream.readAllBytes is Android 13+; this app runs from Android 12.
            java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
            try (InputStream in = p.getInputStream()) {
                byte[] buf = new byte[8192];
                for (int n; (n = in.read(buf)) > 0; ) bytes.write(buf, 0, n);
            }
            String text = bytes.toString("UTF-8");
            p.waitFor();
            return text.length() > 20000 ? text.substring(text.length() - 20000) : text;
        } catch (Exception e) {
            return "(unavailable: " + e.getMessage() + ")";
        }
    }

    private void showReport(String report) {
        TextView text = new TextView(this);
        text.setText(report);
        text.setTextIsSelectable(true);
        text.setTextSize(11);
        int pad = (int) (12 * getResources().getDisplayMetrics().density);
        text.setPadding(pad, pad, pad, pad);
        android.widget.ScrollView scroll = new android.widget.ScrollView(this);
        scroll.addView(text);
        new AlertDialog.Builder(this)
                .setTitle("The game closed unexpectedly")
                .setView(scroll)
                .setPositiveButton("Share report", (d, w) -> {
                    Intent send = new Intent(Intent.ACTION_SEND);
                    send.setType("text/plain");
                    send.putExtra(Intent.EXTRA_SUBJECT, "HydrogenVM Android crash report");
                    send.putExtra(Intent.EXTRA_TEXT, report.length() > 90000 ? report.substring(report.length() - 90000) : report);
                    startActivity(Intent.createChooser(send, "Share report"));
                })
                .setNegativeButton("Close", null)
                .show();
    }

    private void refresh() {
        List<Games.Game> games = Games.list(this);
        adapter.clear();
        adapter.addAll(games);
        empty.setVisibility(games.isEmpty() ? TextView.VISIBLE : TextView.GONE);
        makeMissingIcons(games);
    }

    private void pickGame() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, PICK_GAME);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_GAME || result != RESULT_OK || data == null || data.getData() == null) return;
        importGame(data.getData());
    }

    @SuppressWarnings("deprecation")
    private void importGame(Uri uri) {
        ProgressDialog progress = ProgressDialog.show(this, "Importing", "Copying and unpacking the game…", true, false);
        new Thread(() -> {
            String message;
            File copy = null;
            try {
                String name = displayName(uri);
                String lower = name.toLowerCase();
                String ext = lower.endsWith(".apk") || lower.endsWith(".xapk") || lower.endsWith(".apks") || lower.endsWith(".apkm")
                        ? lower.substring(lower.lastIndexOf('.')) : ".ipa";
                copy = new File(getCacheDir(), "import" + ext);
                try (InputStream in = getContentResolver().openInputStream(uri); OutputStream out = new FileOutputStream(copy)) {
                    if (in == null) throw new java.io.IOException("can't open the file");
                    byte[] buf = new byte[1 << 16];
                    for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
                }
                String[] r = Native.importGame(copy.getAbsolutePath(), Games.appsRoot(this).getAbsolutePath());
                if (!r[0].isEmpty()) {
                    Games.remember(r[0], r[1], r[2]);
                    Native.writeIcon(r[0], new File(new File(r[0]).getParentFile(), "hvm-icon.png").getAbsolutePath());
                }
                message = r[3].isEmpty() ? "Imported " + r[1] + "." : r[3];
            } catch (Exception e) {
                message = "Import failed: " + e.getMessage();
            } finally {
                if (copy != null) //noinspection ResultOfMethodCallIgnored
                    copy.delete();
            }
            final String shown = message;
            main.post(() -> {
                progress.dismiss();
                refresh();
                new AlertDialog.Builder(this).setMessage(shown).setPositiveButton("OK", null).show();
            });
        }).start();
    }

    private String displayName(Uri uri) {
        if ("file".equals(uri.getScheme()) && uri.getLastPathSegment() != null) return uri.getLastPathSegment();
        try (Cursor c = getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0);
        } catch (Exception ignored) {
        }
        String last = uri.getLastPathSegment();
        return last == null ? "game.ipa" : last;
    }

    private void play(Games.Game g) {
        Intent i = new Intent(this, GameActivity.class);
        i.putExtra("bundlePath", g.bundlePath);
        i.putExtra("homePath", g.homePath());
        i.putExtra("bundleId", g.bundleId);
        i.putExtra("title", g.title);
        startActivity(i);
    }

    private void settings(Games.Game g) {
        final String[] labels = {
                "Higher resolution (sharper 3D, up to 3x)",
                "Fill the screen (taller screen; games made for the old screen are shown normally)",
                "Unlock frame rate (30 fps games at 60; experimental)"};
        final String[] keys = {Games.HIGHER_RESOLUTION, Games.FILL_SCREEN, Games.UNLOCK_FPS};
        boolean[] on = new boolean[keys.length];
        for (int i = 0; i < keys.length; i++) on[i] = Games.flag(this, keys[i], g.bundleId);
        new AlertDialog.Builder(this)
                .setTitle(g.title)
                .setMultiChoiceItems(labels, on, (d, which, checked) -> Games.setFlag(this, keys[which], g.bundleId, checked))
                .setPositiveButton("Done", null)
                .setNegativeButton("Delete game", (d, w) -> new AlertDialog.Builder(this)
                        .setMessage("Delete " + g.title + " and its saved data?")
                        .setPositiveButton("Delete", (d2, w2) -> {
                            Games.delete(g.folder);
                            refresh();
                        })
                        .setNegativeButton("Cancel", null)
                        .show())
                .show();
    }

    /** Java errors anywhere in the app are written down for the next start, then the app closes as usual. */
    static void installCrashCatcher(android.content.Context c) {
        final Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        if (previous instanceof CrashCatcher) return;
        Thread.setDefaultUncaughtExceptionHandler(new CrashCatcher(c, previous));
    }

    static final class CrashCatcher implements Thread.UncaughtExceptionHandler {
        private final android.content.Context context;
        private final Thread.UncaughtExceptionHandler previous;
        CrashCatcher(android.content.Context c, Thread.UncaughtExceptionHandler p) { context = c; previous = p; }
        @Override public void uncaughtException(Thread t, Throwable e) {
            java.io.StringWriter w = new java.io.StringWriter();
            e.printStackTrace(new java.io.PrintWriter(w));
            Games.writeText(Games.appCrash(context), "Thread " + t.getName() + ":\n" + w);
            if (previous != null) previous.uncaughtException(t, e);
        }
    }
}
