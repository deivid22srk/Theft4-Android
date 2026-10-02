#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// embedded_assets.h
//
// Build-time asset manifest for platforms that cannot use the installer wizard
// (iOS, Android, PS4, Nintendo Switch).
//
// When LIBERTY_RECOMP_EMBEDDED_ASSETS is defined at compile time, game files
// are bundled directly into the package at the path specified by
// LIBERTY_RECOMP_EMBEDDED_GAME_PATH.  Runtime code should call
// EmbeddedAssets::IsAvailable() first; if it returns true, the installer UI
// should be skipped and EmbeddedAssets::GetGameRoot() used instead.
// ─────────────────────────────────────────────────────────────────────────────

#include <filesystem>
#include <string>
#include <cstdint>

namespace EmbeddedAssets
{
    struct Manifest
    {
        std::string  gameVersion;        // e.g. "1.0.0"
        std::string  titleUpdateVersion; // e.g. "TU8", empty if none
        bool         hasTLAD    = false;
        bool         hasTBOGT   = false;
        uint32_t     buildTimestamp = 0; // Unix timestamp of the build
    };

    /// Returns true when the build was configured with LIBERTY_RECOMP_EMBEDDED_ASSETS.
    bool IsAvailable();

    /// Root directory where embedded game files live at runtime.
    /// Platform-specific:
    ///   iOS    → <Bundle>/Resources/game/
    ///   Android→ <internal_storage>/game/   (extracted from OBB on first boot)
    ///   PS4    → /app0/game/
    ///   Switch → romfs:/game/
    std::filesystem::path GetGameRoot();

    /// Android only: path of the user-selected game ISO (XenDroid-style
    /// delivery), or empty when running in folder/OBB mode. In ISO mode
    /// GetGameRoot() resolves to the internal storage game dir and only
    /// default.xex is staged there — the disc content itself is mounted
    /// in place from the ISO by ReXGlue's DiscImageDevice.
    std::filesystem::path GetGameIsoPath();

    /// Android only: when an ISO is selected, extract the host-side payload
    /// (default.xex) from it into GetGameRoot(). Cheap (a few MB, XDVDFS is
    /// parsed in place via mmap); the RPF content is NOT copied. Returns
    /// true immediately in folder/OBB mode, true on successful staging and
    /// false when the ISO is missing/invalid or lacks default.xex.
    bool EnsureIsoPayload();

    /// Returns the path of the embedded DLC, or empty if not present.
    std::filesystem::path GetDLCRoot();

    /// Reads the manifest JSON written at build time.
    Manifest ReadManifest();

    /// On Android, copies OBB content to internal storage on first boot.
    /// No-op on other platforms.
    bool ExtractObbIfNeeded(const std::filesystem::path& obbPath,
                            const std::filesystem::path& destPath);
}
