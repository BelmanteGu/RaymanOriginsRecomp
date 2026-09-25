package io.github.belmantegu.raymanrecomp;

import android.app.AlertDialog;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.ViewGroup;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;

import org.libsdl.app.SDLActivity;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/**
 * Starts the recompiled game: SDLActivity loads the native libraries below and
 * calls SDL_main() in librayman.so.
 *
 * The game data is not part of the APK. It is read from
 * /sdcard/Android/data/io.github.belmantegu.raymanrecomp/files/game/
 * (default.xex and the .ipk bundles from the player's own copy).
 *
 * Extra runtime options can be put one per line in
 * /sdcard/Android/data/io.github.belmantegu.raymanrecomp/files/args.txt;
 * lines starting with # are ignored.
 */
public class RaymanActivity extends SDLActivity implements TouchControls.SettingsListener {
    private static final String TAG = "RaymanRecomp";

    private TouchControls touchControls;

    @Override
    protected String[] getLibraries() {
        // Dependencies first: the linker resolves them from the app's lib dir,
        // but loading them explicitly gives clearer errors.
        return new String[] {
            "c++_shared",
            "rexruntime",
            "rayman",
        };
    }

    @Override
    protected String[] getArguments() {
        List<String> args = new ArrayList<>();
        SharedPreferences prefs = getSharedPreferences(TouchControls.PREFS, MODE_PRIVATE);
        // Native renderer (default): no GPU emulation; the game's draws go to
        // Vulkan with its own shaders as SPIR-V (files/spirv). Otherwise the
        // Xenos GPU emulation renders.
        args.add(nativeRenderer(prefs) ? "--gpu_plugin=null" : "--gpu_plugin=xenos");
        // Physical keyboards map to the controller (same bindings as desktop).
        args.add("--mnk_mode=true");
        if (prefs.getBoolean(TouchControls.KEY_FILL, false)) {
            // Stretch to the whole screen: a 720p guest video mode with the
            // display's aspect ratio. The game still frames a 16:9 scene, so the
            // picture is stretched horizontally, not widened.
            DisplayMetrics m = getResources().getDisplayMetrics();
            int w = Math.max(m.widthPixels, m.heightPixels);
            int h = Math.min(m.widthPixels, m.heightPixels);
            int width = Math.round(720f * w / h / 8f) * 8;
            args.add("--video_mode_width=" + width);
            args.add("--video_mode_height=720");
        }
        File extra = new File(getExternalFilesDir(null), "args.txt");
        if (extra.isFile()) {
            try (BufferedReader reader = new BufferedReader(new FileReader(extra))) {
                String line;
                while ((line = reader.readLine()) != null) {
                    line = line.trim();
                    if (!line.isEmpty() && !line.startsWith("#")) {
                        args.add(line);
                    }
                }
            } catch (IOException e) {
                Log.w(TAG, "Could not read " + extra, e);
            }
        }
        Log.i(TAG, "Arguments: " + args);
        return args.toArray(new String[0]);
    }

    static final String KEY_NATIVE = "native_renderer";

    private boolean nativeRenderer(SharedPreferences prefs) {
        return prefs.getBoolean(KEY_NATIVE, true);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Read by the native libraries (rex/src/native_renderer.cpp), so it has
        // to be set before SDLActivity loads them.
        SharedPreferences prefs = getSharedPreferences(TouchControls.PREFS, MODE_PRIVATE);
        try {
            if (nativeRenderer(prefs)) {
                Os.setenv("RAYMAN_NATIVE_RENDER", "main", true);
                Os.setenv("RAYMAN_NATIVE_SPIRV", new File(getExternalFilesDir(null), "spirv").getPath(), true);
            } else {
                Os.unsetenv("RAYMAN_NATIVE_RENDER");
            }
        } catch (ErrnoException e) {
            Log.w(TAG, "setenv failed", e);
        }
        super.onCreate(savedInstanceState);
        if (mLayout == null) {
            return;  // SDL failed to load and is showing its error dialog
        }
        touchControls = new TouchControls(this);
        touchControls.setSettingsListener(this);
        mLayout.addView(touchControls, new ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        touchControls.applyPreferences();
    }

    // Touches in flight when the window loses focus (notification shade, system
    // gestures, dialogs, app switch) never deliver their "up": let go of them.
    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus && touchControls != null) {
            touchControls.releaseAll();
        }
    }

    @Override
    protected void onPause() {
        if (touchControls != null) {
            touchControls.releaseAll();
        }
        super.onPause();
    }

    @Override
    public void onOpenSettings() {
        final SharedPreferences prefs = getSharedPreferences(TouchControls.PREFS, MODE_PRIVATE);
        final boolean fillBefore = prefs.getBoolean(TouchControls.KEY_FILL, false);
        final boolean nativeBefore = nativeRenderer(prefs);
        int pad = Math.round(20 * getResources().getDisplayMetrics().density);

        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(pad, pad / 2, pad, 0);

        final CheckBox visible = new CheckBox(this);
        visible.setText(R.string.controls_visible);
        visible.setChecked(prefs.getBoolean(TouchControls.KEY_VISIBLE, true));
        box.addView(visible);

        final SeekBar opacity = addSlider(box, R.string.controls_opacity,
            prefs.getInt(TouchControls.KEY_OPACITY, 55) - 10, 90);
        final SeekBar size = addSlider(box, R.string.controls_size,
            prefs.getInt(TouchControls.KEY_SCALE, 100) - 60, 90);

        final CheckBox fill = new CheckBox(this);
        fill.setText(R.string.fill_screen);
        fill.setChecked(fillBefore);
        box.addView(fill);

        final CheckBox nativeBox = new CheckBox(this);
        nativeBox.setText(R.string.native_renderer);
        nativeBox.setChecked(nativeBefore);
        box.addView(nativeBox);

        new AlertDialog.Builder(this)
            .setTitle(R.string.settings_title)
            .setView(box)
            .setPositiveButton(android.R.string.ok, (dialog, which) -> {
                prefs.edit()
                    .putBoolean(TouchControls.KEY_VISIBLE, visible.isChecked())
                    .putInt(TouchControls.KEY_OPACITY, opacity.getProgress() + 10)
                    .putInt(TouchControls.KEY_SCALE, size.getProgress() + 60)
                    .putBoolean(TouchControls.KEY_FILL, fill.isChecked())
                    .putBoolean(KEY_NATIVE, nativeBox.isChecked())
                    .apply();
                touchControls.applyPreferences();
                if (fill.isChecked() != fillBefore || nativeBox.isChecked() != nativeBefore) {
                    new AlertDialog.Builder(this)
                        .setMessage(R.string.restart_needed)
                        .setPositiveButton(android.R.string.ok, null)
                        .show();
                }
            })
            .setNegativeButton(android.R.string.cancel, null)
            .show();
    }

    private SeekBar addSlider(LinearLayout box, int label, int value, int max) {
        TextView title = new TextView(this);
        title.setText(label);
        title.setPadding(0, title.getPaddingTop() + 16, 0, 0);
        box.addView(title);
        SeekBar bar = new SeekBar(this);
        bar.setMax(max);
        bar.setProgress(Math.max(0, Math.min(max, value)));
        box.addView(bar);
        return bar;
    }
}
