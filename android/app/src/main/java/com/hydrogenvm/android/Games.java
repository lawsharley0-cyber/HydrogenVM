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

        File iconFile() { return new File(folder, "hvm-icon.png"); }

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

    // Crash reporting: a marker written when a game starts and removed when it closes normally.
    // Found on the next start of the app, it means the app died during the game.
    static File runMarker(Context c) { return new File(c.getFilesDir(), "game-running.txt"); }
    static File lastLog(Context c) { return new File(c.getFilesDir(), "last-game.log"); }
    static File appCrash(Context c) { return new File(c.getFilesDir(), "app-crash.txt"); }

    static String readText(File f, int maxChars) {
        try (java.io.FileInputStream in = new java.io.FileInputStream(f)) {
            byte[] b = new byte[(int) Math.min(f.length(), 4L << 20)];
            int n = 0;
            for (int r; n < b.length && (r = in.read(b, n, b.length - n)) > 0; ) n += r;
            String text = new String(b, 0, n, java.nio.charset.StandardCharsets.UTF_8);
            return text.length() > maxChars ? text.substring(text.length() - maxChars) : text;
        } catch (IOException e) {
            return "";
        }
    }

    static void writeText(File f, String text) {
        try (FileOutputStream out = new FileOutputStream(f)) {
            out.write(text.getBytes(java.nio.charset.StandardCharsets.UTF_8));
        } catch (IOException ignored) {
        }
    }

    static final String HIGHER_RESOLUTION = "higherResolution";
    static final String FILL_SCREEN = "fillScreen";
    static final String UNLOCK_FPS = "unlockFPS";
}
