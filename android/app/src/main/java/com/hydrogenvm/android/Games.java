package com.hydrogenvm.android;

import android.content.Context;
import android.content.SharedPreferences;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Properties;

/** The imported games: files/apps/<bundle id>/ holds the game and its saves, plus hvm-game.properties. */
final class Games {
    static final class Game {
        final String title, bundlePath, bundleId;
        final File folder;

        Game(String title, String bundlePath, String bundleId, File folder) {
            this.title = title;
            this.bundlePath = bundlePath;
            this.bundleId = bundleId;
            this.folder = folder;
        }

        /** The game's home (Documents, Library, ...): the folder that holds it, as on iPhone. */
        String homePath() {
            return new File(bundlePath).getParent();
        }

        @Override public String toString() { return title; }
    }

    private static final String META = "hvm-game.properties";

    private Games() {}

    static File appsRoot(Context c) {
        File root = new File(c.getFilesDir(), "apps");
        //noinspection ResultOfMethodCallIgnored
        root.mkdirs();
        return root;
    }

    static List<Game> list(Context c) {
        List<Game> out = new ArrayList<>();
        File[] dirs = appsRoot(c).listFiles();
        if (dirs == null) return out;
        for (File dir : dirs) {
            File meta = new File(dir, META);
            if (!meta.isFile()) continue;
            Properties p = new Properties();
            try (FileInputStream in = new FileInputStream(meta)) {
                p.load(in);
            } catch (IOException e) {
                continue;
            }
            String bundle = p.getProperty("bundlePath", "");
            if (bundle.isEmpty() || !new File(bundle).exists()) continue;
            out.add(new Game(p.getProperty("title", dir.getName()), bundle, p.getProperty("bundleId", dir.getName()), dir));
        }
        Collections.sort(out, (a, b) -> a.title.compareToIgnoreCase(b.title));
        return out;
    }

    /** Records an import; the game's folder is the one that holds its bundle. */
    static void remember(String bundlePath, String title, String bundleId) throws IOException {
        File folder = new File(bundlePath).getParentFile();
        if (folder == null) throw new IOException("no folder for " + bundlePath);
        Properties p = new Properties();
        p.setProperty("bundlePath", bundlePath);
        p.setProperty("title", title.isEmpty() ? bundleId : title);
        p.setProperty("bundleId", bundleId);
        try (FileOutputStream out = new FileOutputStream(new File(folder, META))) {
            p.store(out, "HydrogenVM game");
        }
    }

    static void delete(File f) {
        File[] children = f.listFiles();
        if (children != null) for (File child : children) delete(child);
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }

    // Per-game switches, as on iPhone: off unless the player turns them on.
    static SharedPreferences prefs(Context c) {
        return c.getSharedPreferences("games", Context.MODE_PRIVATE);
    }

    static boolean flag(Context c, String key, String bundleId) {
        return prefs(c).getBoolean(key + "." + bundleId, false);
    }

    static void setFlag(Context c, String key, String bundleId, boolean on) {
        prefs(c).edit().putBoolean(key + "." + bundleId, on).apply();
    }

    static final String HIGHER_RESOLUTION = "higherResolution";
    static final String FILL_SCREEN = "fillScreen";
    static final String UNLOCK_FPS = "unlockFPS";
}
