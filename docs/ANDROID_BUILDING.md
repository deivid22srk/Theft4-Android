# Building Theft4 for Android

Theft4 Android targets **arm64-v8a** devices running **Android 8.0+ (API 26)**
with **Vulkan 1.1** support. The renderer is Vulkan (via the RexGlue runtime /
volk), windowing, audio and input are SDL3, and the game code is statically
recompiled ahead of time — no JIT, no emulator, no game files at build time.

## Repository layout (Android-relevant)

| Path | Purpose |
| --- | --- |
| `os/android/` | Gradle project (single `:app` module) that wraps `libLibertyRecomp.so` in an APK |
| `os/android/app/src/main/java/com/libertyrecomp/LibertyPickerActivity.java` | Launcher activity: game-folder picker + storage permissions (plain Activity) |
| `os/android/app/src/main/java/com/libertyrecomp/LibertySDLActivity.java` | SDL hosting activity: JNI wiring, pushes game root to native before SDL_main starts |
| `LibertyRecomp/os/android/` | JNI glue (`jni_glue.cpp`), logger/media/process/user/vibration backends |
| `toolchains/android.cmake` | CMake toolchain wrapper (chain-loads the NDK toolchain, forces Vulkan) |
| `glue/rexglue-sdk-main/` | RexGlue SDK runtime (SDL3, volk, VMA, FFmpeg, glslang, …) |
| `glue/rexglue-sdk-main/gta4-recomp/generated/` | Pre-generated recompiled game sources (committed; the XEX is NOT needed to build) |

## CI build

`.github/workflows/build-android.yml` builds the release APK (debug-signed so
it can be sideloaded directly) on every push to `main` and publishes it as the
`Theft4-Android-APK` artifact. No repository secrets are required because the
build never touches game files.

## Local build (Linux/macOS host)

Requirements: JDK 17+, Android SDK with **NDK 27.2.12479018** and
**CMake >= 3.29** (e.g. `sdkmanager "ndk;27.2.12479018" "cmake;3.31.1"`),
`platforms;android-35` and `build-tools` resolved automatically by AGP 8.7.3
/ Gradle 8.11.1 (the wrapper jar is committed).

```sh
cd os/android
./gradlew assembleRelease          # APK at app/build/outputs/apk/release/
./gradlew assembleDebug            # debuggable build for adb logcat work
./gradlew assembleRelease -PlibertyRecompEmulator=true   # add x86_64 ABI
```

Optional custom signing:

```sh
./gradlew assembleRelease \
  -PlibertyRecompKeystore=/path/to/keystore.jks \
  -PlibertyRecompKeystorePassword=... \
  -PlibertyRecompKeyAlias=... \
  -PlibertyRecompKeyPassword=...
```

Without a keystore the release build falls back to the debug key, which is
fine for sideloading.

A pure CMake configure (no APK) mirrors the Gradle arguments:

```sh
cmake -S . -B out/build/android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=toolchains/android.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_static \
  -DLIBERTY_RECOMP_TARGET_PLATFORM=android \
  -DLIBERTY_RECOMP_ANDROID_RUNTIME_ASSETS=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

## Runtime game files (bring your own copy)

The APK does **not** contain any game data. On first launch the app shows a
launcher screen: grant **"All files access"** (MANAGE_EXTERNAL_STORAGE) when
prompted, then select the folder that contains an extracted Xbox 360 dump of
GTA IV. The folder is validated against the same required-file list as the
build-time check in the root `CMakeLists.txt` and is remembered across
launches.

Required in the selected folder:

| File | Required | Notes |
| --- | --- | --- |
| `default.xex` | yes | Game executable |
| `common.rpf` | yes | Game archive |
| `xbox360.rpf` | yes | Game archive |
| `audio.rpf` | yes | Game archive |
| `dlc/TLAD/` | optional | Episodes from Liberty City — The Lost and Damned |
| `dlc/TBOGT/` | optional | Episodes from Liberty City — The Ballad of Gay Tony |

A convenient location is any folder on shared storage, e.g.
`/sdcard/Theft4/`. Do not commit or redistribute these files — they are
copyrighted by Rockstar Games.

## Input

SDL3 enumerates Android gamepads (Bluetooth and USB) automatically; a
physical controller is the recommended way to play. On-screen touch input is
forwarded as pointer events. Vibration is wired through
`vibration_android.cpp`.

## Troubleshooting

* `adb logcat -s LibertyRecomp SDL/APP` shows the native startup logs.
* Black screen after PLAY usually means the picked folder is missing one of
  the four required files — the launcher status line lists the first missing
  one.
* Vulkan validation: the manifest requires `android.hardware.vulkan.version`
  1.1 (`0x400003`), so unsupported devices cannot install the APK.
