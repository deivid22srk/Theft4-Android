package com.libertyrecomp;

import android.content.Context;
import android.net.Uri;
import android.os.Build;
import android.util.Log;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipInputStream;

/**
 * Import / activation manager for custom Adreno GPU drivers (libadrenotools).
 *
 * <p>Driver layout on device (native contract — see
 * LibertyRecomp/os/android/jni_glue.cpp and
 * glue/rexglue-sdk-main/src/ui/vulkan/vulkan_instance.cpp):</p>
 * <pre>
 *   &lt;filesDir&gt;/drivers/
 *     active.txt                    "id=&lt;id&gt;\nlib=&lt;abs .so path&gt;\n"
 *     last_boot.txt                 "status=... driver=... error=..." (native writes)
 *     &lt;id&gt;/driver.json              metadata
 *     &lt;id&gt;/&lt;original .so names&gt;     driver .so + companion libs, FLAT
 * </pre>
 *
 * <p>The active.txt "lib=" line is what the native side exports as
 * REX_VULKAN_LOADER_PATH before the Vulkan instance is created. Keeping the
 * activation as a FILE (not SharedPreferences) keeps the C++ side free of any
 * Android framework dependency and mirrors the reference integration.</p>
 *
 * <p>Extraction rule (learned from the reference implementation): ALL .so
 * entries of the zip are extracted with their ORIGINAL basenames into one
 * flat directory — multi-file driver packages (adpkg companions such as
 * libgsl.so) resolve their DT_NEEDED dependencies against the directory, so
 * renaming anything breaks them.</p>
 */
public final class GpuDriverManager {

    private static final String TAG = "GpuDriver";

    private static final String DRIVERS_DIR = "drivers";
    private static final String ACTIVE_FILE = "active.txt";
    private static final String META_FILE   = "driver.json";
    public  static final String LAST_BOOT_FILE = "last_boot.txt";

    private GpuDriverManager() {}  // static-only

    // ─── Model ──────────────────────────────────────────────────────────────

    public static class DriverInfo {
        public String id;
        public String name;
        public String author;
        public String version;
        public String vendor;
        public String libName;
        public String libPath;   // absolute path of the main driver .so
        public boolean active;
    }

    // ─── Import ─────────────────────────────────────────────────────────────

    /**
     * Imports a driver .zip (AdrenoTools meta.json layout) and returns the
     * imported driver. Throws Exception with a user-readable message on any
     * validation failure. The driver is NOT auto-activated; callers should
     * call {@link #setActive} afterwards.
     */
    public static DriverInfo importFromZip(Context ctx, Uri zipUri) throws Exception {
        File tmp = new File(ctx.getCacheDir(), "driver_import_" + System.currentTimeMillis() + ".zip");
        try {
            try (InputStream in = ctx.getContentResolver().openInputStream(zipUri);
                 OutputStream out = new FileOutputStream(tmp)) {
                if (in == null) throw new Exception("Cannot open the selected file");
                copy(in, out);
            }
            return importFromZipFile(ctx, tmp);
        } finally {
            tmp.delete();
        }
    }

    private static DriverInfo importFromZipFile(Context ctx, File zipFile) throws Exception {
        // Pass 1: locate and parse meta.json.
        String metaText = null;
        try (ZipFile zip = new ZipFile(zipFile)) {
            java.util.Enumeration<? extends ZipEntry> entries = zip.entries();
            while (entries.hasMoreElements()) {
                ZipEntry e = entries.nextElement();
                if (e.isDirectory()) continue;
                String base = baseName(e.getName());
                if ("meta.json".equalsIgnoreCase(base)) {
                    metaText = readFully(zip.getInputStream(e));
                    break;
                }
            }
        }
        if (metaText == null) {
            throw new Exception("meta.json not found — not an AdrenoTools driver package");
        }
        JSONObject meta = new JSONObject(metaText);

        String libName  = firstNonEmpty(meta, "libName", "libraryName");
        String name     = firstNonEmpty(meta, "name", "description");
        String author   = firstNonEmpty(meta, "author");
        String version  = firstNonEmpty(meta, "packageVersion", "version");
        String vendor   = firstNonEmpty(meta, "vendor");
        int minApi      = meta.optInt("minAPI", meta.optInt("minApi", 0));
        if (libName == null) throw new Exception("meta.json is missing libraryName");
        if (name == null)    throw new Exception("meta.json is missing name");
        if (minApi > Build.VERSION.SDK_INT) {
            throw new Exception("Driver requires Android API " + minApi
                + " (device has " + Build.VERSION.SDK_INT + ")");
        }

        // Pass 2: extract ALL .so files with original basenames, validating.
        String slug = slug(name, 16);
        String id = slug + "_" + slug(version == null ? "0" : version, 12)
                  + "_" + Long.toString(System.currentTimeMillis(), 36);
        File destDir = new File(driversRoot(ctx), id);
        if (!destDir.isDirectory() && !destDir.mkdirs()) {
            throw new Exception("Cannot create driver directory");
        }

        String mainSoName = null;
        try (ZipFile zip = new ZipFile(zipFile)) {
            java.util.Enumeration<? extends ZipEntry> entries = zip.entries();
            while (entries.hasMoreElements()) {
                ZipEntry e = entries.nextElement();
                if (e.isDirectory()) continue;
                String base = baseName(e.getName());
                if (!base.toLowerCase().endsWith(".so") || base.contains("/")) continue;

                File outFile = new File(destDir, base);
                byte[] soBytes;
                try (InputStream in = zip.getInputStream(e)) {
                    soBytes = readFullyBytes(in);
                }
                boolean isMain = base.equals(libName);
                validateElf(base, soBytes, isMain);
                try (FileOutputStream out = new FileOutputStream(outFile)) {
                    out.write(soBytes);
                }
                if (isMain) mainSoName = base;
            }
        }

        // Locate the main library if it wasn't an exact-name match.
        if (mainSoName == null) {
            String lowered = libName.toLowerCase();
            for (File f : destDir.listFiles()) {
                String n = f.getName().toLowerCase();
                if (n.contains("vulkan") || n.contains("turnip") || n.contains("freedreno")) {
                    mainSoName = f.getName();
                    break;
                }
            }
        }
        if (mainSoName == null) {
            File[] sos = destDir.listFiles();
            if (sos != null && sos.length > 0) {
                mainSoName = sos[0].getName();
            }
        }
        if (mainSoName == null) {
            deleteRecursive(destDir);
            throw new Exception("No .so library found inside the package");
        }

        File mainSo = new File(destDir, mainSoName);
        DriverInfo info = new DriverInfo();
        info.id = id;
        info.name = name;
        info.author = author;
        info.version = version;
        info.vendor = vendor;
        info.libName = mainSoName;
        info.libPath = mainSo.getAbsolutePath();

        JSONObject persist = new JSONObject();
        persist.put("id", info.id);
        persist.put("libName", info.libName);
        persist.put("name", info.name);
        persist.put("author", info.author == null ? "" : info.author);
        persist.put("version", info.version == null ? "" : info.version);
        persist.put("vendor", info.vendor == null ? "" : info.vendor);
        persist.put("libPath", info.libPath);
        persist.put("source", "zip");
        writeAtomic(new File(destDir, META_FILE), persist.toString());

        Log.i(TAG, "Imported driver id=" + id + " lib=" + mainSoName);
        return info;
    }

    // ─── Activation ─────────────────────────────────────────────────────────

    /** Activates a driver by writing files/drivers/active.txt (atomic rename). */
    public static void setActive(Context ctx, String id) throws Exception {
        File dir = new File(driversRoot(ctx), id);
        JSONObject meta = readMeta(dir);
        String libPath = meta.optString("libPath", "");
        if (libPath.isEmpty()) throw new Exception("driver.json is missing libPath");
        File lib = new File(libPath);
        if (!lib.isFile() || !hasElfMagic(lib)) {
            throw new Exception("Driver library is missing or invalid: " + lib.getName());
        }
        String content = "id=" + id + "\nlib=" + lib.getAbsolutePath() + "\n";
        writeAtomic(new File(driversRoot(ctx), ACTIVE_FILE), content);
        Log.i(TAG, "Active driver set: id=" + id + " lib=" + lib.getAbsolutePath());
    }

    /** Deactivates the custom driver (back to the system driver). */
    public static void clearActive(Context ctx) {
        new File(driversRoot(ctx), ACTIVE_FILE).delete();
        Log.i(TAG, "Active driver cleared (system driver)");
    }

    /** Returns the active driver id, or null when the system driver is used. */
    public static String getActiveId(Context ctx) {
        File f = new File(driversRoot(ctx), ACTIVE_FILE);
        if (!f.isFile()) return null;
        try {
            for (String line : readFully(new FileInputStream(f)).split("\n")) {
                line = line.trim();
                if (line.startsWith("id=")) return line.substring(3);
            }
        } catch (Exception ignored) { }
        return null;
    }

    // ─── Listing / diagnostics ──────────────────────────────────────────────

    public static List<DriverInfo> listDrivers(Context ctx) {
        List<DriverInfo> out = new ArrayList<>();
        String activeId = getActiveId(ctx);
        File root = driversRoot(ctx);
        File[] dirs = root.listFiles();
        if (dirs == null) return out;
        for (File d : dirs) {
            if (!d.isDirectory()) continue;
            try {
                JSONObject meta = readMeta(d);
                DriverInfo info = new DriverInfo();
                info.id = meta.optString("id", d.getName());
                info.name = meta.optString("name", d.getName());
                info.author = meta.optString("author", "");
                info.version = meta.optString("version", "");
                info.vendor = meta.optString("vendor", "");
                info.libName = meta.optString("libName", "");
                info.libPath = meta.optString("libPath", "");
                info.active = info.id.equals(activeId);
                out.add(info);
            } catch (Exception e) {
                Log.w(TAG, "Skipping unreadable driver dir " + d.getName(), e);
            }
        }
        return out;
    }

    public static void deleteDriver(Context ctx, String id) {
        File dir = new File(driversRoot(ctx), id);
        deleteRecursive(dir);
        if (id.equals(getActiveId(ctx))) clearActive(ctx);
    }

    /** Parses files/drivers/last_boot.txt (written by the native side). */
    public static Map<String, String> lastBootOutcome(Context ctx) {
        File f = new File(driversRoot(ctx), LAST_BOOT_FILE);
        if (!f.isFile()) return null;
        Map<String, String> out = new TreeMap<>();
        try {
            for (String line : readFully(new FileInputStream(f)).split("\n")) {
                int eq = line.indexOf('=');
                if (eq > 0) out.put(line.substring(0, eq), line.substring(eq + 1));
            }
            return out;
        } catch (Exception e) {
            return null;
        }
    }

    // ─── Helpers ────────────────────────────────────────────────────────────

    private static File driversRoot(Context ctx) {
        File root = new File(ctx.getFilesDir(), DRIVERS_DIR);
        if (!root.isDirectory()) root.mkdirs();
        return root;
    }

    private static JSONObject readMeta(File dir) throws Exception {
        File f = new File(dir, META_FILE);
        if (!f.isFile()) throw new Exception("driver.json missing in " + dir.getName());
        return new JSONObject(readFully(new FileInputStream(f)));
    }

    /** Validates the ELF header. Main driver libs must be ELF64 / AArch64 / ET_DYN. */
    private static void validateElf(String name, byte[] data, boolean isMain) throws Exception {
        if (data.length < 20 || data[0] != 0x7F || data[1] != 'E' || data[2] != 'L' || data[3] != 'F') {
            throw new Exception(name + " is not an ELF library");
        }
        if (isMain) {
            boolean elf64 = data[4] == 2;
            // Little-endian e_machine at offset 18: EM_AARCH64 = 183 (0xB7, 0x00).
            boolean aarch64 = data.length > 19
                && data[18] == (byte) 0xB7 && data[19] == 0x00;
            if (!elf64) throw new Exception(name + " is not a 64-bit library");
            if (!aarch64) throw new Exception(name + " is not an arm64-v8a library");
        }
    }

    private static boolean hasElfMagic(File f) {
        try (FileInputStream in = new FileInputStream(f)) {
            byte[] head = new byte[4];
            int n = in.read(head);
            return n == 4 && head[0] == 0x7F && head[1] == 'E' && head[2] == 'L' && head[3] == 'F';
        } catch (Exception e) {
            return false;
        }
    }

    private static String baseName(String path) {
        int slash = path.lastIndexOf('/');
        return slash >= 0 ? path.substring(slash + 1) : path;
    }

    private static String firstNonEmpty(JSONObject meta, String... keys) {
        for (String k : keys) {
            String v = meta.optString(k, "");
            if (!v.isEmpty()) return v;
        }
        return null;
    }

    private static String slug(String s, int maxLen) {
        StringBuilder sb = new StringBuilder();
        for (char c : s.toLowerCase().toCharArray()) {
            if (Character.isLetterOrDigit(c)) sb.append(c);
            if (sb.length() >= maxLen) break;
        }
        return sb.length() > 0 ? sb.toString() : "driver";
    }

    private static void writeAtomic(File target, String content) throws Exception {
        File tmp = new File(target.getParentFile(), target.getName() + ".tmp");
        try (FileOutputStream out = new FileOutputStream(tmp)) {
            out.write(content.getBytes("UTF-8"));
            out.getFD().sync();
        }
        if (!tmp.renameTo(target)) {
            tmp.delete();
            throw new Exception("Cannot write " + target.getName());
        }
    }

    private static String readFully(InputStream in) throws Exception {
        return new String(readFullyBytes(in), "UTF-8");
    }

    private static byte[] readFullyBytes(InputStream in) throws Exception {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[64 * 1024];
        int n;
        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        return out.toByteArray();
    }

    private static void copy(InputStream in, OutputStream out) throws Exception {
        byte[] buf = new byte[64 * 1024];
        int n;
        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
    }

    private static void deleteRecursive(File f) {
        if (f == null) return;
        File[] children = f.isDirectory() ? f.listFiles() : null;
        if (children != null) {
            for (File c : children) deleteRecursive(c);
        }
        f.delete();
    }
}
