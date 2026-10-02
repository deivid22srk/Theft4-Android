package com.libertyrecomp;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.util.Map;

/**
 * Launcher activity: storage-permission gate + game-folder picker.
 *
 * <p>This is a PLAIN {@link Activity} — deliberately NOT an SDLActivity
 * subclass. The previous design hosted the picker inside
 * {@link LibertySDLActivity} and tried to defer {@code super.onCreate()}
 * until the user picked a folder. That is impossible under the Android
 * Activity contract: if {@link #onCreate(Bundle)} returns without the
 * framework's {@code Activity.onCreate()} having run somewhere up the
 * chain, ActivityThread throws
 * {@code android.util.SuperNotCalledException ... did not call through to
 * super.onCreate()} and the app crashes on launch. Deferring the super
 * call to a later lifecycle stage can never satisfy that check.</p>
 *
 * <p>The correct split — used by every SDL port with a launcher menu — is:
 * <ol>
 *   <li>This plain activity validates storage access and the game folder,
 *       persisting the choice in SharedPreferences.</li>
 *   <li>PLAY starts {@link LibertySDLActivity}, whose {@code onCreate()}
 *       calls {@code super.onCreate(...)} immediately (satisfying the
 *       contract) and whose SDL thread only starts once the activity
 *       reaches RESUMED with a surface — by which time
 *       {@link LibertySDLActivity#loadLibraries()} has already pushed the
 *       game root to the native side.</li>
 * </ol></p>
 *
 * <p>The picked path is persisted under the SAME preferences file the
 * previous single-activity build used ("liberty_recomp_prefs" /
 * "game_dir"), so folders selected on older installs carry over.</p>
 */
public class LibertyPickerActivity extends Activity {

    private static final String TAG = "LibertyRecomp";

    private static final String PREFS_NAME   = "liberty_recomp_prefs";
    private static final String PREF_GAME_DIR = "game_dir";
    private static final String PREF_GAME_ISO = "game_iso";

    private static final int REQ_PICK_FOLDER      = 1001;
    private static final int REQ_PICK_ISO         = 1004;
    private static final int REQ_MANAGE_STORAGE   = 1002;
    private static final int REQ_LEGACY_STORAGE   = 1003;
    private static final int REQ_PICK_DRIVER      = 1005;

    // Minimum plausible size for a GTA IV Xbox 360 disc image (~7.3 GB
    // officially, but trimmed/DVDF rips exist). Anything below 64 MiB is
    // certainly not a usable disc and the picker rejects it up front.
    private static final long MIN_ISO_BYTES = 64L * 1024L * 1024L;

    // Files every extracted GTA IV dump must contain before the native side
    // is willing to boot. Matches root CMakeLists.txt for parity.
    private static final String[] REQUIRED_FILES = new String[] {
        "default.xex",
        "common.rpf",
        "xbox360.rpf",
        "audio.rpf",
    };

    // ─── Colour palette (mirrors picker strings.xml / colors.xml) ──────────
    private static final int C_BG    = 0xFF0D1117;
    private static final int C_CARD  = 0xFF161B22;
    private static final int C_TEXT  = 0xFFF0F6FC;
    private static final int C_MUTED = 0xFF8B949E;
    private static final int C_ERR   = 0xFFFF6B6B;

    private String   mGameDir;
    private String   mGameIso;

    private TextView mPathLabel;
    private TextView mStatusLabel;
    private TextView mIsoLabel;
    private TextView mDriverInfoLabel;
    private LinearLayout mDriverList;
    private Button   mPlayBtn;

    // ─── Lifecycle ─────────────────────────────────────────────────────────

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Activity contract: the framework super MUST run before anything
        // else in this method, and MUST run on every launch — this is the
        // exact call the previous design skipped and crashed on.
        super.onCreate(savedInstanceState);

        mGameDir = prefs().getString(PREF_GAME_DIR, null);
        mGameIso = prefs().getString(PREF_GAME_ISO, null);
        buildPickerUI();
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Re-evaluate when returning from the system permission screen or
        // from the game itself (user may have altered files meanwhile).
        refreshPickerStatus();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == REQ_PICK_FOLDER) {
            if (resultCode == RESULT_OK && data != null && data.getData() != null) {
                Uri tree = data.getData();
                // Persist permission so we can reuse the URI across launches.
                try {
                    getContentResolver().takePersistableUriPermission(
                        tree,
                        Intent.FLAG_GRANT_READ_URI_PERMISSION);
                } catch (Exception ignored) { }

                String absPath = resolveTreeToAbsolutePath(tree);
                if (absPath != null) {
                    setGameDirAndRefresh(absPath);
                } else {
                    Toast.makeText(this,
                        "Could not resolve picker URI to a filesystem path. " +
                        "Use \"Allow access to manage all files\" and retry.",
                        Toast.LENGTH_LONG).show();
                }
            }
            return;
        }

        if (requestCode == REQ_PICK_ISO) {
            if (resultCode == RESULT_OK && data != null && data.getData() != null) {
                Uri doc = data.getData();
                try {
                    getContentResolver().takePersistableUriPermission(
                        doc,
                        Intent.FLAG_GRANT_READ_URI_PERMISSION);
                } catch (Exception ignored) { }

                String absPath = resolveDocumentToAbsolutePath(doc);
                if (absPath != null) {
                    setGameIsoAndRefresh(absPath);
                } else {
                    Toast.makeText(this,
                        "Could not resolve the picked ISO to a filesystem path. " +
                        "Use \"Allow access to manage all files\" and retry.",
                        Toast.LENGTH_LONG).show();
                }
            }
            return;
        }

        if (requestCode == REQ_PICK_DRIVER) {
            if (resultCode == RESULT_OK && data != null && data.getData() != null) {
                importAndActivateDriver(data.getData());
            }
            return;
        }

        if (requestCode == REQ_MANAGE_STORAGE) {
            refreshPickerStatus();
            return;
        }

        super.onActivityResult(requestCode, resultCode, data);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQ_LEGACY_STORAGE) {
            refreshPickerStatus();
        }
    }

    // ─── Picker UI ─────────────────────────────────────────────────────────

    private void buildPickerUI() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(C_BG);
        root.setGravity(Gravity.CENTER_HORIZONTAL);
        root.setPaddingRelative(dp(24), dp(56), dp(24), dp(40));

        ScrollView scroll = new ScrollView(this);
        scroll.addView(root, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        TextView title = new TextView(this);
        title.setText(getString(R.string.picker_title));
        title.setTextSize(48f);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setTextColor(C_TEXT);
        title.setGravity(Gravity.CENTER);
        root.addView(title);

        TextView sub = new TextView(this);
        sub.setText(getString(R.string.picker_subtitle));
        sub.setTextSize(14f);
        sub.setTextColor(C_MUTED);
        sub.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams subLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        subLp.topMargin = dp(8);
        subLp.bottomMargin = dp(32);
        sub.setLayoutParams(subLp);
        root.addView(sub);

        root.addView(buildPermissionCard());
        root.addView(buildFolderCard());
        root.addView(buildIsoCard());
        root.addView(buildDriverCard());

        mPlayBtn = new Button(this);
        mPlayBtn.setText(getString(R.string.picker_play));
        mPlayBtn.setTextSize(18f);
        mPlayBtn.setTypeface(Typeface.DEFAULT_BOLD);
        mPlayBtn.setAllCaps(false);
        LinearLayout.LayoutParams playLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(58));
        playLp.topMargin = dp(16);
        mPlayBtn.setLayoutParams(playLp);
        mPlayBtn.setOnClickListener(v -> tryStartGame());
        root.addView(mPlayBtn);

        setContentView(scroll);
        refreshPickerStatus();
    }

    private View buildPermissionCard() {
        LinearLayout card = newCard();
        GradientDrawable border = (GradientDrawable) card.getBackground();
        border.setStroke(dp(1), 0xFF30363d);

        TextView label = new TextView(this);
        label.setText(getString(R.string.perm_title));
        label.setTextSize(12f);
        label.setTypeface(Typeface.DEFAULT_BOLD);
        label.setTextColor(C_TEXT);
        card.addView(label);

        TextView body = new TextView(this);
        body.setText(getString(R.string.perm_body));
        body.setTextSize(13f);
        body.setTextColor(C_MUTED);
        LinearLayout.LayoutParams bodyLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        bodyLp.topMargin = dp(6);
        bodyLp.bottomMargin = dp(12);
        body.setLayoutParams(bodyLp);
        card.addView(body);

        Button open = new Button(this);
        open.setText(getString(R.string.perm_grant));
        open.setOnClickListener(v -> requestStorageAccess());
        card.addView(open);

        card.setId(View.generateViewId());
        card.setVisibility(haveStorageAccess() ? View.GONE : View.VISIBLE);
        card.setTag("perm-card");
        return card;
    }

    private View buildFolderCard() {
        LinearLayout card = newCard();

        TextView label = new TextView(this);
        label.setText("Game folder");
        label.setTextSize(11f);
        label.setTypeface(Typeface.DEFAULT_BOLD);
        label.setTextColor(C_TEXT);
        card.addView(label);

        mPathLabel = new TextView(this);
        mPathLabel.setTextSize(13f);
        mPathLabel.setTextColor(C_MUTED);
        LinearLayout.LayoutParams pathLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        pathLp.topMargin = dp(4);
        mPathLabel.setLayoutParams(pathLp);
        card.addView(mPathLabel);

        mStatusLabel = new TextView(this);
        mStatusLabel.setTextSize(12f);
        mStatusLabel.setTextColor(C_ERR);
        LinearLayout.LayoutParams statusLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        statusLp.topMargin = dp(4);
        mStatusLabel.setLayoutParams(statusLp);
        card.addView(mStatusLabel);

        Button pick = new Button(this);
        pick.setText(getString(R.string.picker_pick_folder));
        LinearLayout.LayoutParams pickLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        pickLp.topMargin = dp(10);
        pick.setLayoutParams(pickLp);
        pick.setOnClickListener(v -> pickFolder());
        card.addView(pick);

        card.setTag("folder-card");
        return card;
    }

    /**
     * Card letting the user pick a single .iso file instead of an extracted
     * folder (XenDroid-style delivery). The ISO is read IN PLACE — the game
     * disc is mounted read-only and nothing is copied into the app.
     */
    private View buildIsoCard() {
        LinearLayout card = newCard();

        TextView label = new TextView(this);
        label.setText(getString(R.string.iso_card_title));
        label.setTextSize(11f);
        label.setTypeface(Typeface.DEFAULT_BOLD);
        label.setTextColor(C_TEXT);
        card.addView(label);

        mIsoLabel = new TextView(this);
        mIsoLabel.setTextSize(13f);
        mIsoLabel.setTextColor(C_MUTED);
        LinearLayout.LayoutParams isoLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        isoLp.topMargin = dp(4);
        mIsoLabel.setLayoutParams(isoLp);
        card.addView(mIsoLabel);

        Button pick = new Button(this);
        pick.setText(getString(R.string.picker_pick_iso));
        LinearLayout.LayoutParams pickLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        pickLp.topMargin = dp(10);
        pick.setLayoutParams(pickLp);
        pick.setOnClickListener(v -> pickIso());
        card.addView(pick);

        card.setTag("iso-card");
        return card;
    }

    /**
     * Card managing custom Adreno GPU drivers (libadrenotools — Mesa turnip
     * and other AdrenoTools-compatible packages). Drivers are imported as
     * .zip packages, validated and extracted to internal storage by
     * {@link GpuDriverManager}; the selected one is handed to the native
     * Vulkan loader through files/drivers/active.txt.
     */
    private View buildDriverCard() {
        LinearLayout card = newCard();

        TextView label = new TextView(this);
        label.setText("GPU driver (Adreno)");
        label.setTextSize(11f);
        label.setTypeface(Typeface.DEFAULT_BOLD);
        label.setTextColor(C_TEXT);
        card.addView(label);

        mDriverInfoLabel = new TextView(this);
        mDriverInfoLabel.setTextSize(13f);
        mDriverInfoLabel.setTextColor(C_MUTED);
        LinearLayout.LayoutParams infoLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        infoLp.topMargin = dp(4);
        mDriverInfoLabel.setLayoutParams(infoLp);
        card.addView(mDriverInfoLabel);

        mDriverList = new LinearLayout(this);
        mDriverList.setOrientation(LinearLayout.VERTICAL);
        card.addView(mDriverList);

        Button importBtn = new Button(this);
        importBtn.setText("Import driver (.zip)");
        LinearLayout.LayoutParams importLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        importLp.topMargin = dp(10);
        importBtn.setLayoutParams(importLp);
        importBtn.setOnClickListener(v -> pickDriverZip());
        card.addView(importBtn);

        Button systemBtn = new Button(this);
        systemBtn.setText("Use system driver");
        systemBtn.setOnClickListener(v -> {
            GpuDriverManager.clearActive(this);
            refreshPickerStatus();
            Toast.makeText(this, "System driver selected", Toast.LENGTH_SHORT).show();
        });
        card.addView(systemBtn);

        card.setTag("driver-card");
        return card;
    }

    private void pickDriverZip() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {
            "application/zip", "application/octet-stream", "application/x-zip-compressed"});
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        startActivityForResult(i, REQ_PICK_DRIVER);
    }

    private void importAndActivateDriver(Uri zipUri) {
        try {
            GpuDriverManager.DriverInfo info = GpuDriverManager.importFromZip(this, zipUri);
            GpuDriverManager.setActive(this, info.id);
            Toast.makeText(this, "Driver imported and activated: " + info.name,
                Toast.LENGTH_LONG).show();
        } catch (Exception e) {
            Log.w(TAG, "Driver import failed", e);
            Toast.makeText(this, "Driver import failed: " + e.getMessage(),
                Toast.LENGTH_LONG).show();
        }
        refreshPickerStatus();
    }

    /** Rebuilds the driver rows inside the driver card. */
    private void refreshDriverRows() {
        if (mDriverList == null || mDriverInfoLabel == null) return;
        mDriverList.removeAllViews();

        StringBuilder summary = new StringBuilder();
        Map<String, String> boot = GpuDriverManager.lastBootOutcome(this);
        if (boot != null) {
            String status = boot.get("status");
            String driver = boot.get("driver");
            if ("custom_ok".equals(status)) {
                summary.append("Last boot: custom driver (").append(driver).append(")");
            } else if ("custom_failed".equals(status)) {
                summary.append("Last boot: custom driver FAILED (")
                       .append(boot.get("error")).append(")");
            } else if (status != null) {
                summary.append("Last boot: system driver");
            }
        }
        mDriverInfoLabel.setText(summary.length() > 0 ? summary.toString()
            : "Custom driver for Adreno GPUs (e.g. Mesa turnip). Optional — the system driver is used when none is selected.");

        for (final GpuDriverManager.DriverInfo info : GpuDriverManager.listDrivers(this)) {
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setPadding(0, dp(6), 0, dp(6));

            LinearLayout textCol = new LinearLayout(this);
            textCol.setOrientation(LinearLayout.VERTICAL);
            LinearLayout.LayoutParams textLp = new LinearLayout.LayoutParams(
                    0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
            textCol.setLayoutParams(textLp);

            TextView title = new TextView(this);
            title.setText(info.name + "  " + (info.version == null ? "" : info.version));
            title.setTextSize(13f);
            title.setTextColor(info.active ? 0xFF3FB950 : C_TEXT);
            textCol.addView(title);

            TextView sub = new TextView(this);
            sub.setText(info.author + " · " + info.vendor + " · " + info.libName);
            sub.setTextSize(11f);
            sub.setTextColor(C_MUTED);
            textCol.addView(sub);
            row.addView(textCol);

            Button useBtn = new Button(this);
            useBtn.setText(info.active ? "ACTIVE" : "USE");
            useBtn.setEnabled(!info.active);
            useBtn.setOnClickListener(v -> {
                try {
                    GpuDriverManager.setActive(this, info.id);
                    refreshPickerStatus();
                } catch (Exception e) {
                    Toast.makeText(this, "Activation failed: " + e.getMessage(),
                        Toast.LENGTH_LONG).show();
                }
            });
            row.addView(useBtn);

            Button delBtn = new Button(this);
            delBtn.setText("×");
            delBtn.setOnClickListener(v -> {
                GpuDriverManager.deleteDriver(this, info.id);
                refreshPickerStatus();
            });
            row.addView(delBtn);

            mDriverList.addView(row);
        }
    }

    private LinearLayout newCard() {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(18), dp(16), dp(18), dp(16));
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(C_CARD);
        bg.setCornerRadius(dp(8));
        card.setBackground(bg);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = dp(12);
        card.setLayoutParams(lp);
        return card;
    }

    private void refreshPickerStatus() {
        if (mPathLabel == null) return;
        View permCard = findViewWithTag("perm-card");
        if (permCard != null) {
            permCard.setVisibility(haveStorageAccess() ? View.GONE : View.VISIBLE);
        }

        boolean folderValid = mGameDir != null && validateGameDir(mGameDir) == null;
        boolean isoValid    = mGameIso != null && validateGameIso(mGameIso) == null;

        mPathLabel.setText(mGameDir == null
            ? getString(R.string.picker_no_folder)
            : getString(R.string.picker_current_path, mGameDir));

        String missing = mGameDir == null ? "" : validateGameDir(mGameDir);
        if (mGameDir == null) {
            mStatusLabel.setText(""); // hidden until user picks
            mStatusLabel.setVisibility(View.GONE);
        } else if (missing != null) {
            mStatusLabel.setVisibility(View.VISIBLE);
            mStatusLabel.setText(getString(R.string.picker_missing_files, missing));
        } else {
            mStatusLabel.setVisibility(View.GONE);
        }

        if (mIsoLabel != null) {
            mIsoLabel.setText(mGameIso == null
                ? getString(R.string.iso_none)
                : getString(R.string.picker_current_iso, mGameIso));
            mIsoLabel.setTextColor(isoValid ? C_MUTED : C_ERR);
        }

        boolean canPlay = haveStorageAccess() && (folderValid || isoValid);
        mPlayBtn.setEnabled(canPlay);
        mPlayBtn.setAlpha(canPlay ? 1f : 0.5f);

        refreshDriverRows();
    }

    private View findViewWithTag(String tag) {
        View root = findViewById(android.R.id.content);
        return root == null ? null : root.findViewWithTag(tag);
    }

    // ─── Folder picker flow ────────────────────────────────────────────────

    private void pickFolder() {
        if (!haveStorageAccess()) {
            requestStorageAccess();
            return;
        }
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                 | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        // Seed the picker near the common "extracted game" location to save
        // the user from walking the full tree on first launch.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            Uri initial = DocumentsContract.buildRootUri(
                "com.android.externalstorage.documents",
                "primary");
            i.putExtra(DocumentsContract.EXTRA_INITIAL_URI, initial);
        }
        startActivityForResult(i, REQ_PICK_FOLDER);
    }

    /**
     * Best-effort conversion of a SAF tree URI into an absolute POSIX path.
     * The native VFS uses plain open()/fopen() so we need real paths, not
     * content URIs. With MANAGE_EXTERNAL_STORAGE granted this succeeds for
     * anything under /storage/emulated/0/.
     */
    @SuppressLint("NewApi")
    private String resolveTreeToAbsolutePath(Uri treeUri) {
        try {
            String docId = DocumentsContract.getTreeDocumentId(treeUri);
            if (docId == null) return null;
            String[] parts = docId.split(":", 2);
            if (parts.length == 0) return null;
            String volume = parts[0];
            String relPath = parts.length > 1 ? parts[1] : "";
            File base;
            if ("primary".equalsIgnoreCase(volume)) {
                base = Environment.getExternalStorageDirectory();
            } else {
                base = new File("/storage/" + volume);
            }
            File resolved = relPath.isEmpty() ? base : new File(base, relPath);
            if (resolved.isDirectory() && resolved.canRead()) {
                return resolved.getAbsolutePath();
            }
            // Fall back: return the path anyway; native code will surface a
            // friendly "can't open default.xex" message if it's wrong.
            return resolved.getAbsolutePath();
        } catch (Exception e) {
            Log.w(TAG, "Failed to resolve picker URI: " + treeUri, e);
            return null;
        }
    }

    private void setGameDirAndRefresh(String path) {
        mGameDir = path;
        mGameIso = null; // last selection wins; modes are mutually exclusive
        prefs().edit().putString(PREF_GAME_DIR, path)
                      .remove(PREF_GAME_ISO).apply();
        refreshPickerStatus();
    }

    private void setGameIsoAndRefresh(String path) {
        mGameIso = path;
        mGameDir = null; // last selection wins; modes are mutually exclusive
        prefs().edit().putString(PREF_GAME_ISO, path)
                      .remove(PREF_GAME_DIR).apply();
        refreshPickerStatus();
    }

    /**
     * Returns null when the directory contains every required game file, or
     * a comma-separated list of the missing file names.
     */
    private static String validateGameDir(String path) {
        if (path == null || path.isEmpty()) return "(no path)";
        File dir = new File(path);
        if (!dir.isDirectory()) return "(not a directory)";
        StringBuilder missing = new StringBuilder();
        for (String name : REQUIRED_FILES) {
            if (!new File(dir, name).isFile()) {
                if (missing.length() > 0) missing.append(", ");
                missing.append(name);
            }
        }
        return missing.length() == 0 ? null : missing.toString();
    }

    // ─── ISO picker flow (XenDroid-style in-place delivery) ────────────────

    private void pickIso() {
        if (!haveStorageAccess()) {
            requestStorageAccess();
            return;
        }
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                 | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            Uri initial = DocumentsContract.buildRootUri(
                "com.android.externalstorage.documents",
                "primary");
            i.putExtra(DocumentsContract.EXTRA_INITIAL_URI, initial);
        }
        startActivityForResult(i, REQ_PICK_ISO);
    }

    /**
     * Converts a single-document SAF URI into an absolute POSIX path. The
     * native side opens the disc image with plain mmap()/open() so a real
     * path is required. With MANAGE_EXTERNAL_STORAGE granted this works for
     * anything under /storage/emulated/0/.
     */
    @SuppressLint("NewApi")
    private String resolveDocumentToAbsolutePath(Uri docUri) {
        try {
            String docId = DocumentsContract.getDocumentId(docUri);
            if (docId == null) return null;
            String[] parts = docId.split(":", 2);
            if (parts.length < 2) return null;
            String volume = parts[0];
            String relPath = parts[1];
            File base;
            if ("primary".equalsIgnoreCase(volume)) {
                base = Environment.getExternalStorageDirectory();
            } else {
                base = new File("/storage/" + volume);
            }
            File resolved = new File(base, relPath);
            return resolved.getAbsolutePath();
        } catch (Exception e) {
            Log.w(TAG, "Failed to resolve document URI: " + docUri, e);
            return null;
        }
    }

    /**
     * Returns null when the file is a plausible GTA IV Xbox 360 disc image,
     * or a human-readable reason why it is not. Full XDVDFS + magic
     * validation happens on the native side (ISOFileSystem::create).
     */
    private static String validateGameIso(String path) {
        if (path == null || path.isEmpty()) return "(no path)";
        File f = new File(path);
        if (!f.isFile()) return "(not a file)";
        if (!f.canRead()) return "(not readable)";
        if (f.length() < MIN_ISO_BYTES) return "(file too small for a disc image)";
        return null;
    }

    // ─── Storage-access gate ───────────────────────────────────────────────

    private boolean haveStorageAccess() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            return Environment.isExternalStorageManager();
        }
        return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE)
            == PackageManager.PERMISSION_GRANTED;
    }

    private void requestStorageAccess() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            try {
                Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName()));
                startActivityForResult(i, REQ_MANAGE_STORAGE);
            } catch (Exception e) {
                // Some OEM ROMs don't honour the package-scoped intent. Fall
                // back to the generic "Allow all files access" list screen.
                Intent i = new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION);
                startActivityForResult(i, REQ_MANAGE_STORAGE);
            }
        } else {
            requestPermissions(
                new String[] { Manifest.permission.READ_EXTERNAL_STORAGE },
                REQ_LEGACY_STORAGE);
        }
    }

    // ─── Game transition ───────────────────────────────────────────────────

    private void tryStartGame() {
        if (!haveStorageAccess()) {
            Toast.makeText(this, R.string.perm_title, Toast.LENGTH_LONG).show();
            requestStorageAccess();
            return;
        }
        boolean folderValid = mGameDir != null && validateGameDir(mGameDir) == null;
        boolean isoValid    = mGameIso != null && validateGameIso(mGameIso) == null;
        if (!folderValid && !isoValid) {
            Toast.makeText(this, R.string.picker_no_folder, Toast.LENGTH_LONG).show();
            return;
        }
        // LibertySDLActivity reads the persisted source from prefs inside its
        // own onCreate() — before super.onCreate() runs — and pushes it to
        // the native side from loadLibraries(), which SDLActivity invokes
        // strictly before the SDL_main thread can start (the thread only
        // starts once the activity is RESUMED with a surface).
        startActivity(new Intent(this, LibertySDLActivity.class));
    }

    // ─── Helpers ───────────────────────────────────────────────────────────

    private SharedPreferences prefs() {
        return getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
