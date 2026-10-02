// Low-noise, crash-surviving boot breadcrumbs for the GPU/video init path.
//
// Why not spdlog/LOG_INFO? spdlog's android sink is fine while the process is
// healthy, but we are hunting a silent stop and a driver-side SIGSEGV during
// pipeline creation — synchronous __android_log_print writes survive both
// (no buffering, no worker thread), so the LAST crumb in logcat is the exact
// call that hung or died. Every crumb is boot-time only; nothing here runs
// per-frame. Tag "LibertyGPU" keeps them trivially filterable:
//   adb logcat -s LibertyGPU
#pragma once

#if defined(__ANDROID__)
#include <android/log.h>
#define LIBERTY_GPU_CRUMB(...) \
    __android_log_print(ANDROID_LOG_INFO, "LibertyGPU", __VA_ARGS__)
#else
#define LIBERTY_GPU_CRUMB(...) do {} while (0)
#endif
