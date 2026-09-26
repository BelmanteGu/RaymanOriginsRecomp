package io.github.belmantegu.raymanrecomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.text.TextPaint;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Home screen: game file status, PLAY, and importing the player's own copy of
 * Rayman Origins (Xbox 360) into the app: a folder with default.xex and the
 * .ipk bundles, or a .zip of it (tools/make_game_pack.py makes one from your
 * files). Nothing from the game ships with the app.
 *
 * Optional art (a personal build's assets/launcher/background.jpg and
 * logo.png, see android/build_apk.sh) replaces the default look.
 */
public class LauncherActivity extends Activity {
    private static final int PICK_FOLDER = 1, PICK_ZIP = 2, PICK_SAVES = 3;

    private TextView status;
    private Button play;
    private ProgressBar progress;
    private TextView graphics;
    private volatile boolean busy;

    private File gameDir() { return new File(getExternalFilesDir(null), "game"); }
    private File spirvDir() { return new File(getExternalFilesDir(null), "spirv"); }
    // ReXGlue's user data root: $HOME/.local/share/rayman, HOME = internal files dir.
    private File saveDir() { return new File(getFilesDir(), ".local/share/rayman"); }

    private boolean gameReady() {
        return new File(gameDir(), "default.xex").isFile() && new File(gameDir(), "bootsequence_X360.ipk").isFile();
    }

    private float dp(float v) {
        return TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    /** Set when the home screen is opened on purpose (in-game settings), so it doesn't skip to the game. */
    static final String EXTRA_HOME = "home";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // With the game files in place the app icon goes straight to the game;
        // the home screen is reached from the in-game settings (⚙ → Home screen).
        if (!getIntent().getBooleanExtra(EXTRA_HOME, false) && gameReady()) {
            extractShadersIfUpdated();
            startActivity(new Intent(this, RaymanActivity.class));
            finish();
            return;
        }
        FrameLayout root = new FrameLayout(this);
        root.setBackground(new GradientDrawable(GradientDrawable.Orientation.TL_BR,
                new int[] {0xFF0B3B2E, 0xFF12674F, 0xFF2BA37A}));

        Bitmap background = loadAsset("launcher/background.jpg");
        if (background != null) {
            ImageView bg = new ImageView(this);
            bg.setImageBitmap(background);
            bg.setScaleType(ImageView.ScaleType.CENTER_CROP);
            root.addView(bg, new FrameLayout.LayoutParams(-1, -1));
        }
        // Darken the left side so the text stays readable over any art.
        View shade = new View(this);
        shade.setBackground(new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
                new int[] {0xE0101418, 0x90101418, 0x00101418}));
        root.addView(shade, new FrameLayout.LayoutParams(-1, -1));

        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setGravity(Gravity.CENTER_VERTICAL);
        int pad = (int) dp(40);
        column.setPadding(pad, (int) dp(16), pad, (int) dp(16));

        Bitmap logo = loadAsset("launcher/logo.png");
        if (logo != null) {
            ImageView logoView = new ImageView(this);
            logoView.setImageBitmap(logo);
            logoView.setAdjustViewBounds(true);
            logoView.setMaxHeight((int) dp(120));
            column.addView(logoView, new LinearLayout.LayoutParams((int) dp(300), -2));
        } else {
            column.addView(titleText("RAYMAN", 48, 0xFFFFA31A));
            column.addView(titleText("ORIGINS", 26, 0xFFFF6A13));
        }

        TextView subtitle = text("Native Android port", 18, 0xFFFFD6A0, false);
        subtitle.setLetterSpacing(0.05f);
        column.addView(subtitle);

        status = text("", 16, Color.WHITE, false);
        status.setPadding(0, (int) dp(10), 0, (int) dp(14));
        column.addView(status);

        play = pillButton("PLAY", true);
        play.setOnClickListener(v -> startGame());
        column.addView(play, buttonParams());

        // A game pack (.zip, see tools/make_game_pack.py) is the one-file way
        // to bring your copy to a device; a folder works too.
        Button importPack = pillButton("Import game pack (.zip)", false);
        importPack.setOnClickListener(v -> pick(PICK_ZIP));
        LinearLayout.LayoutParams importParams = buttonParams();
        importParams.topMargin = (int) dp(12);
        column.addView(importPack, importParams);

        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setVisibility(View.GONE);
        column.addView(progress, new LinearLayout.LayoutParams((int) dp(320), -2));

        LinearLayout links = new LinearLayout(this);
        links.setPadding(0, (int) dp(14), 0, 0);
        links.addView(link("Game folder", v -> pick(PICK_FOLDER)));
        links.addView(text("  ·  ", 15, 0x99FFFFFF, false));
        links.addView(link("Import saves", v -> pick(PICK_SAVES)));
        links.addView(text("  ·  ", 15, 0x99FFFFFF, false));
        graphics = link("", v -> toggleGraphics());
        links.addView(graphics);
        column.addView(links);

        root.addView(column, new FrameLayout.LayoutParams(-2, -1));

        TextView hint = text("Touch controls or a controller", 14, 0xB3FFFFFF, false);
        FrameLayout.LayoutParams hintParams = new FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM | Gravity.END);
        hintParams.setMargins(0, 0, (int) dp(24), (int) dp(14));
        root.addView(hint, hintParams);

        setContentView(root);
        hideSystemBars(root);
        new Thread(this::extractShadersIfUpdated).start();
        refresh();
    }

    private void hideSystemBars(View root) {
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = root.getWindowInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
    }

    private void refresh() {
        if (busy) return;
        boolean ready = gameReady();
        status.setText(ready ? "✓  Ready to play" : "Choose your Rayman Origins (Xbox 360) folder");
        play.setEnabled(ready);
        play.setAlpha(ready ? 1f : 0.45f);
        SharedPreferences prefs = getSharedPreferences(TouchControls.PREFS, MODE_PRIVATE);
        boolean nativeRenderer = prefs.getBoolean(RaymanActivity.KEY_NATIVE, true);
        graphics.setText("Graphics: " + (nativeRenderer ? "Vulkan" : "Emulated"));
    }

    private void startGame() {
        if (!gameReady() || busy) return;
        startActivity(new Intent(this, RaymanActivity.class));
        finish();  // the app icon then goes back to the game, not to this screen
    }

    private void toggleGraphics() {
        SharedPreferences prefs = getSharedPreferences(TouchControls.PREFS, MODE_PRIVATE);
        boolean nativeRenderer = prefs.getBoolean(RaymanActivity.KEY_NATIVE, true);
        prefs.edit().putBoolean(RaymanActivity.KEY_NATIVE, !nativeRenderer).apply();
        refresh();
    }

    // ---- Importing the player's files ----

    private void pick(int request) {
        if (busy) return;
        Intent intent;
        if (request == PICK_ZIP) {
            intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("application/zip");
        } else {
            intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        }
        startActivityForResult(intent, request);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        busy = true;
        progress.setVisibility(View.VISIBLE);
        progress.setIndeterminate(true);
        status.setText(request == PICK_SAVES ? "Importing saves…" : "Copying game files…");
        new Thread(() -> {
            String error = null;
            try {
                if (request == PICK_ZIP) unzip(uri, gameDir());
                else copyTree(uri, request == PICK_SAVES ? saveDir() : gameDir());
            } catch (Exception e) {
                error = e.getMessage();
            }
            final String message = error;
            runOnUiThread(() -> {
                busy = false;
                progress.setVisibility(View.GONE);
                refresh();
                if (message != null) {
                    new AlertDialog.Builder(this).setMessage("Import failed: " + message)
                            .setPositiveButton(android.R.string.ok, null).show();
                } else if (request != PICK_SAVES && !gameReady()) {
                    new AlertDialog.Builder(this)
                            .setMessage("default.xex or bootsequence_X360.ipk not found. Pick the folder that contains the game's files.")
                            .setPositiveButton(android.R.string.ok, null).show();
                } else if (request != PICK_SAVES) {
                    startGame();  // game files in place: straight into the game
                }
            });
        }).start();
    }

    private void setStatus(String s) {
        runOnUiThread(() -> status.setText(s));
    }

    // Copies a document tree (the folder the player picked) into `dest`.
    private void copyTree(Uri tree, File dest) throws IOException {
        String rootId = DocumentsContract.getTreeDocumentId(tree);
        copyChildren(tree, rootId, dest, new long[] {0});
    }

    private void copyChildren(Uri tree, String parentId, File dest, long[] copied) throws IOException {
        if (!dest.isDirectory() && !dest.mkdirs()) throw new IOException("cannot create " + dest);
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parentId);
        String[] cols = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                         DocumentsContract.Document.COLUMN_MIME_TYPE};
        try (android.database.Cursor c = getContentResolver().query(children, cols, null, null, null)) {
            while (c != null && c.moveToNext()) {
                String id = c.getString(0), name = c.getString(1), mime = c.getString(2);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    copyChildren(tree, id, new File(dest, name), copied);
                } else {
                    Uri doc = DocumentsContract.buildDocumentUriUsingTree(tree, id);
                    try (InputStream in = getContentResolver().openInputStream(doc);
                         OutputStream out = new FileOutputStream(new File(dest, name))) {
                        copied[0] += copy(in, out);
                    }
                    setStatus("Copying " + name + "  (" + (copied[0] >> 20) + " MB)");
                }
            }
        }
    }

    private void unzip(Uri zip, File dest) throws IOException {
        long copied = 0;
        // The pack's size drives the progress bar (stored entries: about the
        // same as what gets extracted).
        long total = 0;
        try (android.database.Cursor c = getContentResolver().query(
                zip, new String[] {android.provider.OpenableColumns.SIZE}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) total = c.getLong(0);
        }
        final long size = total;
        if (size > 0) {
            if (size > dest.getParentFile().getUsableSpace()) {
                throw new IOException("not enough free space: " + (size >> 20) + " MB needed, "
                        + (dest.getParentFile().getUsableSpace() >> 20) + " MB free");
            }
            runOnUiThread(() -> {
                progress.setIndeterminate(false);
                progress.setMax(1000);
            });
        }
        try (ZipInputStream in = new ZipInputStream(getContentResolver().openInputStream(zip))) {
            ZipEntry e;
            while ((e = in.getNextEntry()) != null) {
                // Flatten a single top-level folder: game files may sit inside one.
                String name = e.getName();
                if (name.contains("..")) continue;
                File out = new File(dest, name);
                if (e.isDirectory()) { out.mkdirs(); continue; }
                out.getParentFile().mkdirs();
                try (OutputStream o = new FileOutputStream(out)) {
                    copied += copy(in, o);
                }
                final int permille = size > 0 ? (int) Math.min(1000, copied * 1000 / size) : 0;
                final String line = size > 0
                        ? "Extracting…  " + (copied >> 20) + " / " + (size >> 20) + " MB  (" + permille / 10 + "%)"
                        : "Extracting " + out.getName() + "  (" + (copied >> 20) + " MB)";
                runOnUiThread(() -> {
                    status.setText(line);
                    if (size > 0) progress.setProgress(permille);
                });
            }
        }
        // If everything landed in one subfolder, move it up.
        File[] top = dest.listFiles();
        if (top != null && top.length == 1 && top[0].isDirectory() && new File(top[0], "default.xex").isFile()) {
            File[] inner = top[0].listFiles();
            if (inner != null) for (File f : inner) f.renameTo(new File(dest, f.getName()));
            top[0].delete();
        }
    }

    private static long copy(InputStream in, OutputStream out) throws IOException {
        byte[] buf = new byte[1 << 20];
        long total = 0;
        int n;
        while ((n = in.read(buf)) > 0) {
            out.write(buf, 0, n);
            total += n;
        }
        return total;
    }

    // SPIR-V shaders packed in a personal build (assets/spirv) go where the
    // native renderer reads them: once per installed version of the APK.
    private synchronized void extractShadersIfUpdated() {
        long installed;
        try {
            installed = getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime;
        } catch (android.content.pm.PackageManager.NameNotFoundException e) {
            installed = -1;
        }
        SharedPreferences prefs = getSharedPreferences("launcher", MODE_PRIVATE);
        if (installed != -1 && prefs.getLong("shaders_from", 0) == installed) return;
        extractShaders();
        prefs.edit().putLong("shaders_from", installed).commit();
    }

    private void extractShaders() {
        try {
            String[] names = getAssets().list("spirv");
            if (names == null || names.length == 0) return;
            File dir = spirvDir();
            dir.mkdirs();
            for (String name : names) {
                File out = new File(dir, name);
                try (InputStream in = getAssets().open("spirv/" + name);
                     OutputStream o = new FileOutputStream(out)) {
                    copy(in, o);
                }
            }
        } catch (IOException ignored) {
        }
    }

    // ---- Views ----

    private Bitmap loadAsset(String path) {
        try (InputStream in = getAssets().open(path)) {
            return BitmapFactory.decodeStream(in);
        } catch (IOException e) {
            return null;
        }
    }

    private TextView text(String s, float sp, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        t.setTypeface(Typeface.create("sans-serif", bold ? Typeface.BOLD : Typeface.NORMAL));
        t.setShadowLayer(dp(3), 0, dp(1), 0xAA000000);
        return t;
    }

    // Stand-in logo (no art packed): bold orange letters with a dark outline.
    private TextView titleText(String s, float sp, int color) {
        TextView t = new TextView(this) {
            @Override
            protected void onDraw(android.graphics.Canvas canvas) {
                TextPaint p = getPaint();
                int fill = getCurrentTextColor();
                p.setStyle(Paint.Style.STROKE);
                p.setStrokeWidth(dp(5));
                setTextColor(0xFF5A1206);
                super.onDraw(canvas);
                p.setStyle(Paint.Style.FILL);
                setTextColor(fill);
                super.onDraw(canvas);
            }
        };
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        t.setTypeface(Typeface.create("sans-serif-black", Typeface.BOLD_ITALIC));
        t.setLetterSpacing(0.04f);
        return t;
    }

    private TextView link(String s, View.OnClickListener onClick) {
        TextView t = text(s, 15, 0xFFE8F4FF, false);
        t.setOnClickListener(onClick);
        t.setPadding(0, (int) dp(6), 0, (int) dp(6));
        return t;
    }

    private Button pillButton(String s, boolean primary) {
        Button b = new Button(this);
        b.setText(s);
        b.setAllCaps(false);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, primary ? 22 : 17);
        b.setTypeface(Typeface.create("sans-serif-medium", primary ? Typeface.BOLD : Typeface.NORMAL));
        if (primary) b.setLetterSpacing(0.15f);
        GradientDrawable bg = new GradientDrawable();
        bg.setCornerRadius(dp(40));
        if (primary) {
            bg.setColor(Color.WHITE);
            b.setTextColor(0xFF1B1B24);
        } else {
            bg.setColor(0x33000000);
            bg.setStroke((int) dp(2), 0xCCFFFFFF);
            b.setTextColor(Color.WHITE);
        }
        b.setBackground(bg);
        b.setStateListAnimator(null);
        return b;
    }

    private LinearLayout.LayoutParams buttonParams() {
        return new LinearLayout.LayoutParams((int) dp(320), (int) dp(primaryHeight()));
    }

    private float primaryHeight() { return 58; }
}
