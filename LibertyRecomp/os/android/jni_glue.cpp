// JNI glue — called from LibertySDLActivity.java before SDL starts
// Sets paths that the native process layer reads.
#include <jni.h>
#include <string>
#include <cstring>
#include <cstdlib>
#include <malloc.h>
#include <cstdio>

#include <android/log.h>

#include <rex/filesystem.h>
#include <rex/platform/android.h>

#include "achievement_bridge_android.h"
#include "jni_glue.h"

const char* g_androidAppInternalPath = nullptr;
const char* g_androidObbPath         = nullptr;
// Set from LibertySDLActivity's folder-picker flow. Declared in jni_glue.h
// and consumed by install/embedded_assets.cpp (::GetGameRoot) so the native
// runtime points at the user-selected game directory instead of the
// CMake-baked LIBERTY_RECOMP_EMBEDDED_GAME_PATH.
const char* g_androidGameRoot        = nullptr;
// Set from LibertySDLActivity's ISO-picker flow (XenDroid-style delivery).
// Consumed by install/embedded_assets.cpp: only default.xex is staged into
// internal storage; disc content is mounted in place via DiscImageDevice.
const char* g_androidGameIso         = nullptr;

namespace {
// Shared UTF-8 copy helper: allocates a NUL-terminated buffer via malloc so
// free() is always valid on the replaced pointer. Returns nullptr for null
// input or empty strings (treated as "unset").
const char* CopyJString(JNIEnv* env, jstring js)
{
    if (!js) return nullptr;
    const char* tmp = env->GetStringUTFChars(js, nullptr);
    if (!tmp) return nullptr;
    size_t len = strlen(tmp);
    char* buf = nullptr;
    if (len > 0) {
        buf = static_cast<char*>(malloc(len + 1));
        if (buf) memcpy(buf, tmp, len + 1);
    }
    env->ReleaseStringUTFChars(js, tmp);
    return buf;
}
} // namespace

// Global JavaVM captured at library load via JNI_OnLoad. Any native code
// running off a non-Java thread must AttachCurrentThread() to obtain a JNIEnv.
JavaVM*  g_androidJavaVM     = nullptr;
// Global reference to the hosting Activity (Context). Set from Java via
// nativeSetActivity(). Used for Context.getSystemService / getResources calls.
jobject  g_androidActivity   = nullptr;
// Cached API level (Build.VERSION.SDK_INT). 0 until set from Java.
int      g_androidApiLevel   = 0;

// SDL3 already provides JNI_OnLoad in SDL_android.c, so we can't define our
// own.  Instead we lazily capture the JavaVM the first time it's needed,
// using SDL_GetAndroidJNIEnv() (available once SDL_Init / NativeActivity has run).
#include <SDL3/SDL.h>

static JavaVM* EnsureJavaVM()
{
    if (!g_androidJavaVM) {
        JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
        if (env) env->GetJavaVM(&g_androidJavaVM);
    }
    return g_androidJavaVM;
}

// Published to ReXGlue so its Android-specific subsystems (filesystem
// content-URI resolver, keyboard dialog, etc.) can obtain a JNIEnv
// on arbitrary worker threads via vm->AttachCurrentThread().
extern "C" JavaVM* rex_android_get_jvm()
{
    return EnsureJavaVM();
}

extern "C" JNIEXPORT void JNICALL
Java_com_libertyrecomp_LibertySDLActivity_nativeSetActivity(
    JNIEnv* env,
    jclass  /*clazz*/,
    jobject activity,
    jint    apiLevel)
{
    // Capture the JavaVM from the JNIEnv if not already set.
    if (!g_androidJavaVM) env->GetJavaVM(&g_androidJavaVM);

    if (g_androidActivity)
    {
        env->DeleteGlobalRef(g_androidActivity);
        g_androidActivity = nullptr;
    }
    if (activity)
        g_androidActivity = env->NewGlobalRef(activity);
    g_androidApiLevel = static_cast<int>(apiLevel);

    // Publish the Java Context to ReXGlue so filesystem queries
    // (GetUserFolder / GetCachesFolder) resolve to the real app-private
    // paths instead of the /data/data/<progname>/files fallback.
    rex::platform::android::SetAndroidJavaContext(env, g_androidActivity);

    // Initialize the ReXGlue Android filesystem backend (content://
    // resolver refs + ParcelFileDescriptor method IDs). Must run after
    // SetAndroidJavaContext so the cached context is available.
    if (g_androidActivity) {
        rex::filesystem::AndroidInitialize();
    } else {
        rex::filesystem::AndroidShutdown();
    }

    // Bring up the Play Games achievement bridge now that we have an Activity.
    if (g_androidActivity)
        os::achievements::android::Initialize(env, g_androidActivity);
}

extern "C" JNIEXPORT void JNICALL
Java_com_libertyrecomp_LibertySDLActivity_nativeSetPaths(
    JNIEnv* env,
    jclass  /*clazz*/,
    jstring internalPath,
    jstring obbPath)
{
    free(const_cast<char*>(g_androidAppInternalPath));
    free(const_cast<char*>(g_androidObbPath));

    g_androidAppInternalPath = CopyJString(env, internalPath);
    g_androidObbPath         = CopyJString(env, obbPath);
}

// Called by LibertySDLActivity.loadLibraries() once the user has picked a
// game directory. Overrides the CMake-baked LIBERTY_RECOMP_EMBEDDED_GAME_PATH
// inside EmbeddedAssets::GetGameRoot() so the native VFS/installer path is
// rooted at the user-selected folder.
extern "C" JNIEXPORT void JNICALL
Java_com_libertyrecomp_LibertySDLActivity_nativeSetGameRoot(
    JNIEnv* env,
    jclass  /*clazz*/,
    jstring gameRoot)
{
    free(const_cast<char*>(g_androidGameRoot));
    g_androidGameRoot = CopyJString(env, gameRoot);
}

// Called by LibertySDLActivity.loadLibraries() when the user selected a game
// ISO file (XenDroid-style delivery) instead of an extracted folder. The
// disc image is mounted read-only and IN PLACE by ReXGlue's DiscImageDevice;
// EmbeddedAssets::EnsureIsoPayload() stages only default.xex into internal
// storage for the host-side XEX loader.
extern "C" JNIEXPORT void JNICALL
Java_com_libertyrecomp_LibertySDLActivity_nativeSetGameIso(
    JNIEnv* env,
    jclass  /*clazz*/,
    jstring isoPath)
{
    free(const_cast<char*>(g_androidGameIso));
    g_androidGameIso = CopyJString(env, isoPath);
}

// ─── Adrenotools (custom Adreno GPU driver) hand-off + env wiring ────────────
//
// vulkan_instance.cpp (ReXGlue) consults the environment BEFORE creating the
// Vulkan instance:
//   REX_VULKAN_LOADER_PATH    absolute path of the active custom driver .so
//                             (derived from files/drivers/active.txt)
//   REX_ANDROID_NATIVE_LIB_DIR applicationInfo.nativeLibraryDir — home of
//                             libadrenotools.so + the 4 hook libraries
// and memory_android.cpp uses REX_ANDROID_CACHE_DIR as the last-resort
// backing store for the guest address-space reservation.
extern "C" JNIEXPORT void JNICALL
Java_com_libertyrecomp_LibertySDLActivity_nativeSetAndroidDirs(
    JNIEnv* env,
    jclass  /*clazz*/,
    jstring cacheDir,
    jstring nativeLibDir)
{
    const char* cache = CopyJString(env, cacheDir);
    const char* libs  = CopyJString(env, nativeLibDir);

    if (cache && *cache) {
        setenv("REX_ANDROID_CACHE_DIR", cache, 1);
    }
    if (libs && *libs) {
        setenv("REX_ANDROID_NATIVE_LIB_DIR", libs, 1);
    }
    if (g_androidAppInternalPath && *g_androidAppInternalPath) {
        setenv("REX_ANDROID_FILES_DIR", g_androidAppInternalPath, 1);
    }

    // Read the driver activation file written by GpuDriverManager (Java):
    //   files/drivers/active.txt  →  "id=<id>\nlib=<abs .so path>\n"
    // When present, export the lib path so vulkan_instance.cpp routes the
    // Vulkan loader through adrenotools. Absent file ⇒ system driver.
    bool driver_active = false;
    if (g_androidAppInternalPath && *g_androidAppInternalPath) {
        char path[512];
        std::snprintf(path, sizeof(path), "%s/drivers/active.txt",
                      g_androidAppInternalPath);
        if (FILE* f = std::fopen(path, "r")) {
            char line[512];
            while (std::fgets(line, sizeof(line), f)) {
                size_t len = std::strlen(line);
                while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                    line[--len] = '\0';
                }
                if (std::strncmp(line, "lib=", 4) == 0 && len > 4 && line[4] == '/') {
                    setenv("REX_VULKAN_LOADER_PATH", line + 4, 1);
                    driver_active = true;
                    break;
                }
            }
            std::fclose(f);
        }
    }
    if (!driver_active) {
        unsetenv("REX_VULKAN_LOADER_PATH");
    }

    __android_log_print(ANDROID_LOG_INFO, "LibertyRecomp",
                        "Android dirs wired: cache=%s, native_libs=%s, gpu_driver=%s",
                        cache ? cache : "(null)", libs ? libs : "(null)",
                        driver_active ? "CUSTOM (adrenotools)" : "system");

    free(const_cast<char*>(cache));
    free(const_cast<char*>(libs));
}

// Shows a Toast from the native side on a fatal boot failure. Toasts are
// enqueued via the system NotificationManagerService, so they remain visible
// even after the process _Exit()s that follows the call — that gives the user
// a visible reason instead of the app "just closing".
extern "C" void LibertyAndroidNotifyFatal(const char* message)
{
    if (!message) return;
    JniScopedAttach attach;
    JNIEnv* env = attach.env();
    if (!env) return;

    jclass clazz = env->FindClass("com/libertyrecomp/LibertySDLActivity");
    if (!clazz) {
        env->ExceptionClear();
        return;
    }
    jmethodID mid = env->GetStaticMethodID(clazz, "showFatalToast",
                                           "(Ljava/lang/String;)V");
    if (!mid) {
        env->ExceptionClear();
        return;
    }
    jstring jmsg = env->NewStringUTF(message);
    if (!jmsg) {
        env->ExceptionClear();
        return;
    }
    env->CallStaticVoidMethod(clazz, mid, jmsg);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    __android_log_print(ANDROID_LOG_ERROR, "LibertyRecomp",
                        "[FATAL-TOAST] %s", message);
}
