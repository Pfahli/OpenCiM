package io.github.pfahli.opencim;

// The native side of the setup screen (src/platform/android_jni.cpp): OpenCiM's Steam client (src/core/steam_client.h),
// the game folder check and QR codes.
final class SteamBridge {
    static {
        System.loadLibrary("main");
    }

    static native long nCreate(String gameDir, String tokenFile);
    static native void nDestroy(long handle);
    static native void nStartPassword(long handle, String account, String password);
    static native void nStartQr(long handle);
    static native boolean nStartSaved(long handle);
    static native void nSubmitCode(long handle, String code);
    static native void nCancel(long handle);
    static native String nStatus(long handle);
    static native String nSavedAccount(String tokenFile);
    static native void nForgetLogin(String tokenFile);
    static native String nCheckGame(String dir);
    static native byte[] nQr(String text);

    // the downloader's state (nStatus: "key=value" lines)
    static final class Status {
        String phase = "idle", message = "", qr = "", code = "", hint = "", account = "";
        boolean allowCode, wrong;
        long done, total;
        int files, seq = -1;

        static Status parse(String s) {
            Status st = new Status();
            if (s == null) return st;
            for (String line : s.split("\n")) {
                int eq = line.indexOf('=');
                if (eq < 0) continue;
                String k = line.substring(0, eq), v = line.substring(eq + 1);
                switch (k) {
                    case "phase": st.phase = v; break;
                    case "message": st.message = v; break;
                    case "qr": st.qr = v; break;
                    case "code": st.code = v; break;
                    case "allowCode": st.allowCode = v.equals("1"); break;
                    case "hint": st.hint = v; break;
                    case "wrong": st.wrong = v.equals("1"); break;
                    case "account": st.account = v; break;
                    case "done": st.done = parseLong(v); break;
                    case "total": st.total = parseLong(v); break;
                    case "files": st.files = (int) parseLong(v); break;
                    case "seq": st.seq = (int) parseLong(v); break;
                }
            }
            return st;
        }

        private static long parseLong(String v) {
            try {
                return Long.parseLong(v);
            } catch (NumberFormatException e) {
                return 0;
            }
        }
    }

    private SteamBridge() {}
}
