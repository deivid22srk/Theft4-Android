/**
 * @file        rex/main_android.h
 * @brief       Minimal Android platform helpers for the ReXGlue runtime.
 *
 * Provides the API-level query and JNI lifecycle hooks referenced by the
 * POSIX/platform threading and memory backends when compiled for Android.
 */

#ifndef REX_MAIN_ANDROID_H_
#define REX_MAIN_ANDROID_H_

#include <cstdint>

#if defined(__ANDROID__)
#include <android/api-level.h>
#include <cstdlib>
#include <sys/system_properties.h>
#endif

namespace rex {

/// Returns the runtime Android API level (e.g. 26 for Android 8.0).
/// On non-Android platforms this returns 0, so callers can keep a single
/// code-path: `if (rex::GetAndroidApiLevel() >= 26) { ... }`.
inline int32_t GetAndroidApiLevel() {
#if defined(__ANDROID__)
#if __ANDROID_API__ >= 29
  return android_get_device_api_level();
#else
  // android_get_device_api_level() is only available on API 29+; targets
  // below that (the Android port compiles with android-26) must read the
  // system property instead.
  char value[PROP_VALUE_MAX] = {0};
  int length = __system_property_get("ro.build.version.sdk", value);
  if (length > 0) {
    int level = std::atoi(value);
    if (level > 0) {
      return level;
    }
  }
  return 0;
#endif
#else
  return 0;
#endif
}

}  // namespace rex

#endif  // REX_MAIN_ANDROID_H_
