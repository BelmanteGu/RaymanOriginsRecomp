package io.github.belmantegu.raymanrecomp;

import org.libsdl.app.SDLActivity;

/**
 * Starts the recompiled game: SDLActivity loads the native libraries below and
 * calls SDL_main() in librayman.so.
 *
 * The game data is not part of the APK. It is read from
 * /sdcard/Android/data/io.github.belmantegu.raymanrecomp/files/game/
 * (default.xex and the .ipk bundles from the player's own copy).
 */
public class RaymanActivity extends SDLActivity {
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
        return new String[] {
            "--gpu_plugin=xenos",
        };
    }
}
