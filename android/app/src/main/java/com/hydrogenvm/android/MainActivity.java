package com.hydrogenvm.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.ProgressDialog;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Button;
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
        adapter = new ArrayAdapter<>(this, android.R.layout.simple_list_item_1);
        list.setAdapter(adapter);
        list.setOnItemClickListener((parent, view, position, id) -> play(adapter.getItem(position)));
        list.setOnItemLongClickListener((parent, view, position, id) -> {
            settings(adapter.getItem(position));
            return true;
        });
        root.addView(list, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        setContentView(root);
        refresh();
    }

    private void refresh() {
        List<Games.Game> games = Games.list(this);
        adapter.clear();
        adapter.addAll(games);
        empty.setVisibility(games.isEmpty() ? TextView.VISIBLE : TextView.GONE);
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
                if (!r[0].isEmpty()) Games.remember(r[0], r[1], r[2]);
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
        startActivity(i);
    }

    private void settings(Games.Game g) {
        final String[] labels = {
                "Higher resolution (sharper 3D, up to 3x)",
                "Fill the screen (taller screen; not every game supports it)",
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
}
