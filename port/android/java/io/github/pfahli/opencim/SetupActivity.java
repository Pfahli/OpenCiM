package io.github.pfahli.opencim;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.BitmapDrawable;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.DocumentsContract;
import android.text.InputType;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;

// The start screen (docs/android.md): unpacks the port's data, then starts the game when Cities in Motion is there.
// Otherwise (or from the launcher's "Game files" shortcut) it gets the game: from Steam with OpenCiM's own Steam client
// (account name and password, then Steam Guard, or a QR code for the Steam app), or copied from a folder on the device.
// The look follows the Windows setup program (src/setup/setup_main.cpp): its colours, title and welcome picture.
public class SetupActivity extends Activity {
    static final int C_BG = 0xFF15100C, C_PANEL = 0xF018120E, C_EDGE = 0xFF5C5147, C_TEXT = 0xFFF1ECE4,
            C_SOFT = 0xFFD8CFC3, C_DIM = 0xFFB9AB9B, C_ORANGE = 0xFFD9772B, C_FIELD = 0xFF1A1714;
    static final int PICK_FOLDER = 1;

    // the downloader outlives a recreated activity; its thread keeps running while the screen is rebuilt
    static long steam;

    final Handler ui = new Handler(Looper.getMainLooper());
    LinearLayout card, loginBox, workBox, codeBox, buttonsBox;
    TextView heading, body, statusText, savedText, codeLabel;
    EditText account, password, code;
    Button signIn, qrButton, savedButton, forgetButton, copyButton, playButton, playDone, cancelButton, codeButton, backButton;
    ProgressBar progress;
    ImageView qrView;
    int lastSeq = -1;
    String lastQr = "";
    boolean manage, gameReady;
    volatile boolean copying, copyCancel;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        Object m = getIntent().getExtras() != null ? getIntent().getExtras().get("manage") : null;
        manage = Boolean.TRUE.equals(m) || "true".equals(m);  // (the shortcut's extra: boolean or text)
        buildUi();
        show(null);
        heading.setText("OpenCiM");
        body.setText("Getting ready…");
        new Thread(() -> {
            String error = null;
            try {
                Paths.installData(this);
            } catch (IOException e) {
                error = "The program files could not be unpacked: " + e.getMessage();
            }
            final String err = error;
            final String check = SteamBridge.nCheckGame(Paths.game(this).getAbsolutePath());
            ui.post(() -> afterInstall(err, check));
        }).start();
    }

    void afterInstall(String error, String check) {
        if (error != null) {
            heading.setText("Something went wrong");
            body.setText(error);
            return;
        }
        gameReady = check.isEmpty();
        android.util.Log.i("OpenCiM", "game in " + Paths.game(this) + ": " + (gameReady ? "ok" : check));
        if (steam != 0) {  // a download was running when the screen was rebuilt
            show(workBox);
            poll.run();
            return;
        }
        if (gameReady && !manage) {
            play();
            return;
        }
        showChoice();
    }

    void play() {
        startActivity(new Intent(this, GameActivity.class));
        finish();
    }

    // ---- the screens ----
    void showChoice() {
        show(loginBox);
        if (gameReady) {
            heading.setText("Cities in Motion is installed");
            body.setText("Update or repair it from Steam, or copy it in again. Your saves are not touched.");
        } else {
            heading.setText("Get Cities in Motion");
            body.setText("OpenCiM needs your own copy of Cities in Motion. Sign in to Steam to download it to this "
                    + "device (about 1.3 GB, the game and the DLCs your account owns), or copy the game folder from "
                    + "your PC to this device first and choose it here.");
        }
        String savedAccount = SteamBridge.nSavedAccount(Paths.steamLogin(this).getAbsolutePath());
        boolean hasSaved = savedAccount != null;
        savedButton.setVisibility(hasSaved ? View.VISIBLE : View.GONE);
        forgetButton.setVisibility(hasSaved ? View.VISIBLE : View.GONE);
        savedText.setVisibility(hasSaved ? View.VISIBLE : View.GONE);
        if (hasSaved) savedText.setText("Signed in to Steam as " + savedAccount);
        playButton.setVisibility(gameReady ? View.VISIBLE : View.GONE);
    }

    void show(View box) {
        for (View v : new View[] {loginBox, workBox}) v.setVisibility(v == box ? View.VISIBLE : View.GONE);
        boolean working = box == workBox;
        if (working) getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        else getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    void showWork(String title) {
        show(workBox);
        heading.setText(title);
        body.setText("");
        statusText.setText("");
        qrView.setVisibility(View.GONE);
        codeBox.setVisibility(View.GONE);
        progress.setIndeterminate(true);
        progress.setVisibility(View.VISIBLE);
        cancelButton.setVisibility(View.VISIBLE);
        backButton.setVisibility(View.GONE);
        playDone.setVisibility(View.GONE);
        lastSeq = -1;
        lastQr = "";
    }

    // ---- Steam ----
    void newDownloader() {
        if (steam != 0) SteamBridge.nDestroy(steam);
        steam = SteamBridge.nCreate(Paths.game(this).getAbsolutePath(), Paths.steamLogin(this).getAbsolutePath());
    }

    void startPassword() {
        String a = account.getText().toString().trim(), p = password.getText().toString();
        if (a.isEmpty() || p.isEmpty()) {
            (a.isEmpty() ? account : password).setError("Needed");
            return;
        }
        password.setText("");
        newDownloader();
        SteamBridge.nStartPassword(steam, a, p);
        showWork("Signing in to Steam");
        poll.run();
    }

    void startQr() {
        newDownloader();
        SteamBridge.nStartQr(steam);
        showWork("Sign in with the Steam app");
        poll.run();
    }

    void startSaved() {
        newDownloader();
        if (!SteamBridge.nStartSaved(steam)) {
            showChoice();
            return;
        }
        showWork("Downloading from Steam");
        poll.run();
    }

    final Runnable poll = new Runnable() {
        @Override
        public void run() {
            if (steam == 0) return;
            SteamBridge.Status s = SteamBridge.Status.parse(SteamBridge.nStatus(steam));
            if (s.seq != lastSeq) {
                lastSeq = s.seq;
                update(s);
            }
            if (!s.phase.equals("done") && !s.phase.equals("failed") && !s.phase.equals("cancelled"))
                ui.postDelayed(this, 150);
        }
    };

    void update(SteamBridge.Status s) {
        statusText.setText(sentence(s.message));
        switch (s.phase) {
            case "show_qr":
                heading.setText("Sign in with the Steam app");
                body.setText("Open the Steam app on your phone, tap the Steam Guard symbol and then “Scan a QR code”.");
                progress.setVisibility(View.GONE);
                if (!s.qr.isEmpty() && !s.qr.equals(lastQr)) {
                    lastQr = s.qr;
                    qrView.setImageDrawable(qrDrawable(s.qr));
                }
                qrView.setVisibility(View.VISIBLE);
                break;
            case "need_code":
            case "confirm_in_app": {
                heading.setText(s.phase.equals("need_code") ? "Steam Guard code" : "Confirm in the Steam app");
                body.setText(s.phase.equals("need_code")
                        ? (s.code.equals("email")
                                ? "Steam sent a code to your e-mail" + (s.hint.isEmpty() ? "" : " (" + s.hint + ")") + "."
                                : "Enter the code shown in the Steam app (Steam Guard).")
                        : "Steam asks on your phone whether this login is yours. Confirm it there"
                                + (s.allowCode ? ", or enter the code from the Steam app." : "."));
                progress.setVisibility(s.phase.equals("need_code") ? View.GONE : View.VISIBLE);
                progress.setIndeterminate(true);
                qrView.setVisibility(View.GONE);
                boolean wantsCode = s.phase.equals("need_code") || s.allowCode;
                codeBox.setVisibility(wantsCode ? View.VISIBLE : View.GONE);
                if (s.wrong) code.setError("Not accepted");
                break;
            }
            case "downloading":
                heading.setText("Downloading Cities in Motion");
                body.setText(s.account.isEmpty() ? "" : "Steam account: " + s.account);
                qrView.setVisibility(View.GONE);
                codeBox.setVisibility(View.GONE);
                progress.setVisibility(View.VISIBLE);
                if (s.total > 0) {
                    progress.setIndeterminate(false);
                    progress.setMax(1000);
                    progress.setProgress((int) (1000 * s.done / Math.max(1, s.total)));
                    statusText.setText(String.format(Locale.US, "%.0f of %.0f MB", s.done / 1048576.0, s.total / 1048576.0));
                }
                break;
            case "done":
                gameReady = true;
                heading.setText("Cities in Motion is ready");
                body.setText("");
                statusText.setText("");
                progress.setVisibility(View.GONE);
                codeBox.setVisibility(View.GONE);
                qrView.setVisibility(View.GONE);
                cancelButton.setVisibility(View.GONE);
                playDone.setVisibility(View.VISIBLE);
                getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                break;
            case "failed":
            case "cancelled":
                heading.setText(s.phase.equals("failed") ? "That did not work" : "Cancelled");
                progress.setVisibility(View.GONE);
                codeBox.setVisibility(View.GONE);
                qrView.setVisibility(View.GONE);
                cancelButton.setVisibility(View.GONE);
                backButton.setVisibility(View.VISIBLE);
                getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                break;
            default:  // connecting, signing in, preparing
                qrView.setVisibility(View.GONE);
                codeBox.setVisibility(View.GONE);
                progress.setVisibility(View.VISIBLE);
                progress.setIndeterminate(true);
                break;
        }
    }

    static String sentence(String s) {
        if (s == null || s.isEmpty()) return "";
        return Character.toUpperCase(s.charAt(0)) + s.substring(1) + (s.endsWith(".") ? "" : ".");
    }

    BitmapDrawable qrDrawable(String text) {
        byte[] m = SteamBridge.nQr(text);
        int n = (int) Math.round(Math.sqrt(m.length)), quiet = 4, scale = 8, size = (n + 2 * quiet) * scale;
        int[] px = new int[size * size];
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++) {
                int mx = x / scale - quiet, my = y / scale - quiet;
                boolean dark = mx >= 0 && my >= 0 && mx < n && my < n && m[my * n + mx] != 0;
                px[y * size + x] = dark ? Color.BLACK : Color.WHITE;
            }
        BitmapDrawable d = new BitmapDrawable(getResources(), Bitmap.createBitmap(px, size, size, Bitmap.Config.ARGB_8888));
        d.setFilterBitmap(false);
        return d;
    }

    // ---- copy from a folder (e.g. the game folder copied from a PC over USB) ----
    void pickFolder() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        startActivityForResult(i, PICK_FOLDER);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_FOLDER || result != RESULT_OK || data == null || data.getData() == null) return;
        final Uri tree = data.getData();
        showWork("Copying Cities in Motion");
        copying = true;
        copyCancel = false;
        new Thread(() -> {
            String error = null;
            try {
                File dest = Paths.game(this);
                dest.mkdirs();
                String root = DocumentsContract.getTreeDocumentId(tree);
                long[] total = {0};
                measure(tree, root, total);
                long[] done = {0};
                copyTree(tree, root, dest, done, total[0]);
            } catch (Exception e) {
                error = e.getMessage();
            }
            final String err = copyCancel ? "cancelled" : error;
            final String check = SteamBridge.nCheckGame(Paths.game(this).getAbsolutePath());
            ui.post(() -> {
                copying = false;
                SteamBridge.Status s = new SteamBridge.Status();
                gameReady = check.isEmpty();
                if (err == null && gameReady) {
                    s.phase = "done";
                } else {
                    s.phase = err != null && err.equals("cancelled") ? "cancelled" : "failed";
                    s.message = err != null ? err : check;
                }
                update(s);
            });
        }).start();
    }

    void measure(Uri tree, String doc, long[] total) {
        ContentResolver cr = getContentResolver();
        Uri kids = DocumentsContract.buildChildDocumentsUriUsingTree(tree, doc);
        try (Cursor c = cr.query(kids, new String[] {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE}, null, null, null)) {
            while (c != null && c.moveToNext()) {
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(1))) measure(tree, c.getString(0), total);
                else total[0] += c.getLong(2);
            }
        }
    }

    void copyTree(Uri tree, String doc, File dest, long[] done, long total) throws IOException {
        ContentResolver cr = getContentResolver();
        Uri kids = DocumentsContract.buildChildDocumentsUriUsingTree(tree, doc);
        byte[] buf = new byte[1 << 18];
        try (Cursor c = cr.query(kids, new String[] {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_MIME_TYPE}, null, null, null)) {
            while (c != null && c.moveToNext()) {
                if (copyCancel) return;
                String id = c.getString(0), name = c.getString(1);
                if (name == null || name.contains("/") || name.equals("..")) continue;
                File out = new File(dest, name);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(2))) {
                    out.mkdirs();
                    copyTree(tree, id, out, done, total);
                    continue;
                }
                try (InputStream in = cr.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, id));
                     OutputStream o = new FileOutputStream(out)) {
                    if (in == null) throw new IOException("cannot read " + name);
                    for (int n; (n = in.read(buf)) > 0; ) {
                        if (copyCancel) return;
                        o.write(buf, 0, n);
                        done[0] += n;
                        final long d = done[0];
                        ui.post(() -> {
                            progress.setIndeterminate(false);
                            progress.setMax(1000);
                            progress.setProgress((int) (1000 * d / Math.max(1, total)));
                            statusText.setText(String.format(Locale.US, "%.0f of %.0f MB", d / 1048576.0, total / 1048576.0));
                        });
                    }
                }
            }
        }
    }

    // ---- going back ----
    void back() {
        if (steam != 0) {
            SteamBridge.nDestroy(steam);
            steam = 0;
        }
        showChoice();
    }

    void cancelWork() {
        if (copying) copyCancel = true;
        else if (steam != 0) SteamBridge.nCancel(steam);
    }

    @Override
    public void onBackPressed() {
        if (workBox.getVisibility() == View.VISIBLE && cancelButton.getVisibility() == View.VISIBLE) cancelWork();
        else if (workBox.getVisibility() == View.VISIBLE) back();
        else super.onBackPressed();
    }

    // ---- the views ----
    int dp(float v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()));
    }

    Bitmap asset(String name) {
        try (InputStream in = getAssets().open(name)) {
            return BitmapFactory.decodeStream(in);
        } catch (IOException e) {
            return null;
        }
    }

    TextView text(float sp, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setLineSpacing(0, 1.15f);
        return t;
    }

    Button button(String label, boolean primary, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        b.setTextColor(primary ? Color.WHITE : C_TEXT);
        GradientDrawable g = new GradientDrawable();
        g.setCornerRadius(dp(4));
        if (primary) g.setColor(C_ORANGE);
        else {
            g.setColor(0x00000000);
            g.setStroke(dp(1), C_EDGE);
        }
        b.setBackground(g);
        b.setStateListAnimator(null);
        b.setPadding(dp(16), dp(10), dp(16), dp(10));
        b.setOnClickListener(l);
        return b;
    }

    EditText field(String hint, boolean secret) {
        EditText e = new EditText(this);
        e.setHint(hint);
        e.setHintTextColor(C_DIM);
        e.setTextColor(C_TEXT);
        e.setSingleLine(true);
        GradientDrawable g = new GradientDrawable();
        g.setColor(C_FIELD);
        g.setStroke(dp(1), C_EDGE);
        g.setCornerRadius(dp(4));
        e.setBackground(g);
        e.setPadding(dp(12), dp(10), dp(12), dp(10));
        e.setInputType(secret ? InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD
                              : InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        return e;
    }

    void add(LinearLayout into, View v, int topDp) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(topDp);
        into.addView(v, lp);
    }

    void buildUi() {
        getWindow().getDecorView().setBackgroundColor(C_BG);
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(C_BG);
        ImageView bg = new ImageView(this);
        bg.setScaleType(ImageView.ScaleType.CENTER_CROP);
        bg.setImageBitmap(asset("setup/bg-welcome.jpg"));
        root.addView(bg, new FrameLayout.LayoutParams(-1, -1));
        View shade = new View(this);
        shade.setBackgroundColor(0x88000000);
        root.addView(shade, new FrameLayout.LayoutParams(-1, -1));

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setFitsSystemWindows(true);
        root.addView(scroll, new FrameLayout.LayoutParams(-1, -1));
        FrameLayout center = new FrameLayout(this);
        scroll.addView(center, new ScrollView.LayoutParams(-1, -2));

        card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        GradientDrawable panel = new GradientDrawable();
        panel.setColor(C_PANEL);
        panel.setStroke(dp(1), C_EDGE);
        panel.setCornerRadius(dp(6));
        card.setBackground(panel);
        card.setPadding(dp(24), dp(20), dp(24), dp(24));
        int width = Math.min(getResources().getDisplayMetrics().widthPixels - dp(32), dp(560));
        FrameLayout.LayoutParams cp = new FrameLayout.LayoutParams(width, -2, Gravity.CENTER);
        cp.topMargin = cp.bottomMargin = dp(16);
        center.addView(card, cp);

        ImageView title = new ImageView(this);
        title.setImageBitmap(asset("setup/title.png"));
        title.setAdjustViewBounds(true);
        title.setScaleType(ImageView.ScaleType.FIT_START);
        LinearLayout.LayoutParams tp = new LinearLayout.LayoutParams(-2, dp(44));
        card.addView(title, tp);

        heading = text(22, C_TEXT, true);
        add(card, heading, 12);
        body = text(15, C_SOFT, false);
        add(card, body, 6);

        // the choice: Steam (saved login, password, QR) or a folder
        loginBox = new LinearLayout(this);
        loginBox.setOrientation(LinearLayout.VERTICAL);
        add(card, loginBox, 8);
        playButton = button("Play", true, v -> play());
        add(loginBox, playButton, 12);
        savedText = text(14, C_DIM, false);
        add(loginBox, savedText, 16);
        savedButton = button("Download or repair with this login", true, v -> startSaved());
        add(loginBox, savedButton, 8);
        forgetButton = button("Sign out of Steam", false, v -> {
            SteamBridge.nForgetLogin(Paths.steamLogin(this).getAbsolutePath());
            showChoice();
        });
        add(loginBox, forgetButton, 8);
        TextView steamHead = text(16, C_TEXT, true);
        steamHead.setText("Sign in to Steam");
        add(loginBox, steamHead, 20);
        account = field("Steam account name", false);
        account.setAutofillHints(View.AUTOFILL_HINT_USERNAME);
        add(loginBox, account, 8);
        password = field("Password", true);
        password.setAutofillHints(View.AUTOFILL_HINT_PASSWORD);
        password.setImeOptions(EditorInfo.IME_ACTION_GO);
        password.setOnEditorActionListener((v, id, ev) -> {
            startPassword();
            return true;
        });
        add(loginBox, password, 8);
        signIn = button("Sign in and download", true, v -> startPassword());
        add(loginBox, signIn, 12);
        qrButton = button("Sign in with a QR code instead", false, v -> startQr());
        add(loginBox, qrButton, 8);
        TextView note = text(12, C_DIM, false);
        note.setText("OpenCiM talks to Steam itself, like Valve's SteamCMD. It keeps only a login token on this device, "
                + "never your password.");
        add(loginBox, note, 8);
        TextView folderHead = text(16, C_TEXT, true);
        folderHead.setText("Or copy it from a folder");
        add(loginBox, folderHead, 20);
        copyButton = button("Choose the Cities in Motion folder…", false, v -> pickFolder());
        add(loginBox, copyButton, 8);

        // the work: progress, QR code, Steam Guard code
        workBox = new LinearLayout(this);
        workBox.setOrientation(LinearLayout.VERTICAL);
        add(card, workBox, 12);
        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setIndeterminate(true);
        add(workBox, progress, 8);
        statusText = text(14, C_SOFT, false);
        add(workBox, statusText, 6);
        qrView = new ImageView(this);
        qrView.setAdjustViewBounds(true);
        LinearLayout.LayoutParams qp = new LinearLayout.LayoutParams(dp(240), dp(240));
        qp.gravity = Gravity.CENTER_HORIZONTAL;
        qp.topMargin = dp(12);
        workBox.addView(qrView, qp);
        codeBox = new LinearLayout(this);
        codeBox.setOrientation(LinearLayout.VERTICAL);
        add(workBox, codeBox, 12);
        codeLabel = text(14, C_SOFT, false);
        code = field("Code", false);
        code.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        code.setAutofillHints("smsOTPCode");  // (AndroidX's name for one-time codes)
        add(codeBox, code, 0);
        codeButton = button("Send code", true, v -> {
            String c = code.getText().toString().trim();
            if (c.isEmpty() || steam == 0) return;
            code.setText("");
            SteamBridge.nSubmitCode(steam, c);
        });
        add(codeBox, codeButton, 8);
        playDone = button("Play", true, v -> play());
        add(workBox, playDone, 16);
        cancelButton = button("Cancel", false, v -> cancelWork());
        add(workBox, cancelButton, 16);
        backButton = button("Back", false, v -> back());
        add(workBox, backButton, 16);

        setContentView(root);
    }

    @Override
    protected void onDestroy() {
        ui.removeCallbacks(poll);
        super.onDestroy();
    }
}
