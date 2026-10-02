#!/usr/bin/env bash
#
# Builds libadrenotools (+ its 4 hook libraries) for arm64-v8a with the NDK
# and copies the resulting .so files into the Android app's jniLibs directory
# so Gradle packages them into the APK.
#
# Layout produced (packaged with useLegacyPackaging = true so the hooks exist
# as real files inside nativeLibraryDir at runtime — REQUIRED by
# adrenotools_open_libvulkan):
#
#   os/android/app/src/main/jniLibs/arm64-v8a/
#     libadrenotools.so        (dlopen'd by vulkan_instance.cpp)
#     libmain_hook.so          (driver redirect hook)
#     libhook_impl.so          (hook shared implementation)
#     libfile_redirect_hook.so (file redirect feature — unused but packaged)
#     libgsl_alloc_hook.so     (GSL allocator hook   — unused but packaged)
#
# Environment: ANDROID_HOME or ANDROID_SDK_ROOT with an installed NDK (any
# 27.x works; the app pins 27.2.12479018), CMake >= 3.22.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/thirdparty/libadrenotools"
ABI="arm64-v8a"
API="28"   # linkernsbypass requires android-28 (libdl namespace APIs)
BUILD_DIR="$ROOT/build-adrenotools/$ABI"
OUT_JNI="$ROOT/os/android/app/src/main/jniLibs/$ABI"

# ── Locate the NDK ───────────────────────────────────────────────────────────
NDK="${ANDROID_NDK_HOME:-}"
if [ -z "$NDK" ]; then
  SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
  if [ -n "$SDK" ] && [ -d "$SDK/ndk" ]; then
    # Prefer the version the app pins, fall back to whatever is installed.
    if [ -d "$SDK/ndk/27.2.12479018" ]; then
      NDK="$SDK/ndk/27.2.12479018"
    else
      NDK="$(ls -d "$SDK"/ndk/* 2>/dev/null | sort | tail -1)"
    fi
  fi
fi
if [ -z "$NDK" ] || [ ! -d "$NDK" ]; then
  echo "::error::Android NDK not found (set ANDROID_HOME or ANDROID_NDK_HOME)"; exit 1
fi
echo "Using NDK: $NDK"

# ── Ensure the nested linkernsbypass submodule is present ────────────────────
# tools/setup_repo.py initializes submodules without --recursive, so the
# nested one may be missing on CI.
if [ ! -f "$SRC/lib/linkernsbypass/CMakeLists.txt" ]; then
  echo "Initializing nested linkernsbypass submodule..."
  (cd "$SRC" && git submodule update --init --recursive) || \
    git clone --depth 1 https://github.com/bylaws/linkernsbypass.git "$SRC/lib/linkernsbypass"
fi

# ── Configure + build ────────────────────────────────────────────────────────
# BUILD_SHARED_LIBS=ON is MANDATORY: upstream's add_library(adrenotools) is
# typeless; without it the library becomes a static archive and the
# -Wl,--exclude-libs link option below is skipped (that option keeps the
# linkernsbypass symbols out of the exported table).
cmake -S "$SRC" -B "$BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$ABI" \
  -DANDROID_PLATFORM="android-$API" \
  -DANDROID_STL=c++_static \
  -DBUILD_SHARED_LIBS=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DGEN_INSTALL_TARGET=OFF
cmake --build "$BUILD_DIR" -j"$(nproc)"

# ── Collect the 5 shared libraries (wherever CMake put them) ─────────────────
mkdir -p "$OUT_JNI"
EXPECTED=(libadrenotools.so libmain_hook.so libhook_impl.so libfile_redirect_hook.so libgsl_alloc_hook.so)
for so in "${EXPECTED[@]}"; do
  found="$(find "$BUILD_DIR" -type f -name "$so" | head -1)"
  if [ -z "$found" ]; then
    echo "::error::$so was not built — inspect $BUILD_DIR"; exit 1
  fi
  cp -v "$found" "$OUT_JNI/$so"
done

echo "── jniLibs content ──"
ls -la "$OUT_JNI"
echo "adrenotools build OK"
