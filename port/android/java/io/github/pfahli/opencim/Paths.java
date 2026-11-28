package io.github.pfahli.opencim;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.res.AssetManager;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

// Where OpenCiM keeps things on Android (docs/android.md). In the app's own folder on the shared storage
// (Android/data/io.github.pfahli.opencim/files, reachable from a PC over USB):
//   Game/              the Cities in Motion files (downloaded from Steam or copied in)
//   OpenCiM/           saves, maps, settings, mods, logs and crash reports (the "Documents/OpenCiM" of the PC)
//   Cities In Motion/  saves and maps of the original game, when copied there from a PC
// In the app's private folder: data/ (the port's own files, unpacked from the APK) and the saved Steam login.
final class Paths {
    static File root(Context c) {
        File f = c.getExternalFilesDir(null);
        return f != null ? f : c.getFilesDir();
    }

    static File game(Context c) { return new File(root(c), "Game"); }

    static File documents(Context c) { return root(c); }

    static File data(Context c) { return new File(c.getFilesDir(), "data"); }

    static File steamLogin(Context c) { return new File(c.getFilesDir(), "steam-login.txt"); }

    // the port's data files from the APK's assets/data (listed in assets/data.list), unpacked again after an update
    static void installData(Context c) throws IOException {
        File dir = data(c);
        File stamp = new File(dir, ".installed");
        String version;
        try {
            PackageInfo pi = c.getPackageManager().getPackageInfo(c.getPackageName(), 0);
            version = pi.getLongVersionCode() + "-" + pi.lastUpdateTime;
        } catch (Exception e) {
            version = "?";
        }
        if (stamp.exists() && version.equals(readText(stamp))) return;
        deleteTree(dir);
        AssetManager am = c.getAssets();
        byte[] buf = new byte[1 << 16];
        try (BufferedReader list = new BufferedReader(new InputStreamReader(am.open("data.list"), StandardCharsets.UTF_8))) {
            for (String name; (name = list.readLine()) != null; ) {
                if (name.isEmpty()) continue;
                File out = new File(dir, name);
                File parent = out.getParentFile();
                if (parent != null) parent.mkdirs();
                try (InputStream in = am.open("data/" + name); OutputStream o = new FileOutputStream(out)) {
                    for (int n; (n = in.read(buf)) > 0; ) o.write(buf, 0, n);
                }
            }
        }
        try (OutputStream o = new FileOutputStream(stamp)) {
            o.write(version.getBytes(StandardCharsets.UTF_8));
        }
    }

    static String readText(File f) {
        try (InputStream in = new java.io.FileInputStream(f)) {
            byte[] b = new byte[(int) Math.min(f.length(), 1 << 20)];
            int n = in.read(b);
            return n > 0 ? new String(b, 0, n, StandardCharsets.UTF_8) : "";
        } catch (IOException e) {
            return "";
        }
    }

    static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        f.delete();
    }

    private Paths() {}
}
