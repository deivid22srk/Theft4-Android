package com.libertyrecomp;

import android.app.Activity;
import android.content.Context;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;

import org.libsdl.app.SDLActivity;

/**
 * LibertyRecomp's SDL hosting Activity.
 *
 * <p>This class is launched by {@link LibertyPickerActivity} (the launcher,
 * which owns storage permissions and game-folder selection) once a valid
 * game directory is persisted in SharedPreferences.</p>
 *
 * <p>Lifecycle contract — IMPORTANT:
 * <ol>
 *   <li>{@link #onCreate(Bundle)} reads the persisted game root from
 *       SharedPreferences and then calls {@code super.onCreate(...)} as the
 *       VERY FIRST statement chain entry. The Android framework requires
 *       every Activity's onCreate chain to reach
 *       {@code android.app.Activity.onCreate()} before returning; skipping
 *       or deferring it throws
 *       {@code android.util.SuperNotCalledException}.</li>
 *   <li>{@code SDLActivity.onCreate()} calls {@link #loadLibraries()},
 *       which System.loadLibrary()s libLibertyRecomp.so (firing JNI_OnLoad)
 *       and then pushes paths + Activity + game root into the native side.</li>
 *   <li>SDL only spawns the SDL_main thread from
 *       {@code SDLActivity.handleNativeState()} when the activity is RESUMED
 *       with a ready surface — strictly after loadLibraries() completed, so
 *       native code is guaranteed to see a valid game root before main()
 *       runs.</li>
 * </ol></p>
 */
public class LibertySDLActivity extends SDLActivity {

    private static final String TAG = "LibertyRecomp";

    // MUST match LibertyPickerActivity's prefs so the picked folder carries
    // over between the two activities.
    private static final String PREFS_NAME   = "liberty_recomp_prefs";
    private static final String PREF_GAME_DIR = "game_dir";
    private static final String PREF_GAME_ISO = "game_iso";

    // Game source resolved by LibertyPickerActivity and persisted in prefs.
    // Exactly one of these is set: a folder (default.xex + *.rpf dumped
    // together) OR a single .iso file (XenDroid-style in-place delivery).
    private String mGameDir;
    private String mGameIso;

    // ─── Native bridges defined in LibertyRecomp/os/android/jni_glue.cpp ───
    // JNI symbol names embed this class name — do not move these
    // declarations to another class without renaming the C exports.
    private static native void nativeSetActivity(Activity activity, int apiLevel);
    private static native void nativeSetPaths(String internalPath, String obbPath);
    private static native void nativeSetGameRoot(String gameRoot);
    private static native void nativeSetGameIso(String isoPath);
    // vibration_android.cpp
    private static native void nativeSetContext(Context context);

    // ─── SDLActivity overrides ─────────────────────────────────────────────

    @Override
    protected String[] getLibraries() {
        // libLibertyRecomp.so links SDL3 statically; no intermediate libs.
        return new String[] { "LibertyRecomp" };
    }

    @Override
    protected String getMainFunction() {
        // SDL3's SDL_main.h macro-renames main → SDL_main in main.cpp on Android.
        return "SDL_main";
    }

    // ─── Lifecycle ─────────────────────────────────────────────────────────

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Cache the game source before anything else, then satisfy the
        // Activity contract IMMEDIATELY: super.onCreate() must run on every
        // launch before onCreate() returns, with no deferral and no early
        // returns. (Skipping this is what crashed the previous build with
        // SuperNotCalledException on first run.)
        android.content.SharedPreferences p =
            getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        mGameDir = p.getString(PREF_GAME_DIR, null);
        mGameIso = p.getString(PREF_GAME_ISO, null);
        super.onCreate(savedInstanceState);
    }

    @Override
    public void loadLibraries() {
        // Parent loads libLibertyRecomp.so via System.loadLibrary, which fires
        // JNI_OnLoad → captures g_androidJavaVM in jni_glue.cpp. We then push
        // paths + Activity + game-root BEFORE SDL.setupJNI() runs so native
        // code reading g_androidActivity / g_androidGameRoot from main() is
        // guaranteed to see valid values.
        super.loadLibraries();
        try {
            // Caches g_androidActivity + g_androidApiLevel and also initialises
            // the ReXGlue Android filesystem + achievement bridge.
            nativeSetActivity(this, Build.VERSION.SDK_INT);

            String internal = getFilesDir() != null ? getFilesDir().getAbsolutePath() : "";
            String obb      = getObbDir()   != null ? getObbDir().getAbsolutePath()   : "";
            nativeSetPaths(internal, obb);

            // Vibrator JNI bridge.
            nativeSetContext(this);

            // Game source chosen in LibertyPickerActivity. Exactly one mode
            // is active:
            //   folder → nativeSetGameRoot(): files are read from the picked
            //            directory in place.
            //   ISO    → nativeSetGameIso(): the disc image is mounted
            //            read-only IN PLACE by ReXGlue's DiscImageDevice;
            //            only default.xex is staged into internal storage.
            if (mGameDir != null && !mGameDir.isEmpty()) {
                nativeSetGameRoot(mGameDir);
            } else if (mGameIso != null && !mGameIso.isEmpty()) {
                nativeSetGameIso(mGameIso);
            }
            Log.i(TAG, "Native context wired: game_dir=" + mGameDir
                + ", game_iso=" + mGameIso
                + ", internal=" + internal + ", obb=" + obb);
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "Native path setters unavailable: " + e.getMessage());
        } catch (Throwable t) {
            Log.e(TAG, "Native path setup failed", t);
        }
    }
}
