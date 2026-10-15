package com.tdprinting.zoot;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Gets the game's files in place before the game starts, since there is no in-game ROM extractor on
 * Android: copies soh.o2r out of the APK after each install or update, and has the player pick the
 * oot.o2r (and optionally oot-mq.o2r) they made from their own ROM with the desktop build.
 *
 * Everything lives in getExternalFilesDir(null), which is the folder libultraship reads on Android
 * (SDL_AndroidGetExternalStoragePath), so saves, settings and mods/ end up there too.
 */
public class LauncherActivity extends Activity {
    private static final int REQUEST_PICK_ARCHIVES = 1;
    private static final String PREFS = "launcher";
    private static final String PREF_INSTALLED_ASSETS = "installedAssetsFrom";

    private final Handler mainThread = new Handler(Looper.getMainLooper());
    private File dataDir;
    private TextView status;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        status = new TextView(this);
        status.setGravity(Gravity.CENTER);
        status.setTextSize(20);
        status.setPadding(48, 48, 48, 48);
        setContentView(status);

        dataDir = getExternalFilesDir(null);
        if (dataDir == null) {
            fail("Shared storage isn't available, so the game has nowhere to keep its files.");
            return;
        }
        status.setText("Preparing game files...");
        new Thread(this::prepare).start();
    }

    private void prepare() {
        try {
            installBundledAssets();
        } catch (IOException e) {
            mainThread.post(() -> fail("Couldn't copy the game's files: " + e.getMessage()));
            return;
        }
        mainThread.post(() -> {
            if (hasRomArchive()) {
                startGame();
            } else {
                askForRomArchive();
            }
        });
    }

    /** Copies soh.o2r from the APK whenever this install is newer than the copy on disk. */
    private void installBundledAssets() throws IOException {
        long installedFrom;
        try {
            PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            installedFrom = info.lastUpdateTime;
        } catch (PackageManager.NameNotFoundException e) {
            installedFrom = System.currentTimeMillis();
        }
        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        File sohO2r = new File(dataDir, "soh.o2r");
        if (sohO2r.exists() && prefs.getLong(PREF_INSTALLED_ASSETS, 0) == installedFrom) {
            return;
        }
        try (InputStream in = getAssets().open("soh.o2r")) {
            copy(in, sohO2r);
        }
        new File(dataDir, "mods").mkdirs();
        prefs.edit().putLong(PREF_INSTALLED_ASSETS, installedFrom).apply();
    }

    private boolean hasRomArchive() {
        return new File(dataDir, "oot.o2r").exists() || new File(dataDir, "oot-mq.o2r").exists();
    }

    private void askForRomArchive() {
        status.setText("Waiting for oot.o2r...");
        new AlertDialog.Builder(this)
            .setTitle("Pick your game archive")
            .setMessage("This app doesn't include the game. Make oot.o2r from your own Ocarina of Time ROM "
                + "with the Windows or Linux build, copy it to this device, then pick it here.\n\n"
                + "You can pick oot-mq.o2r (Master Quest) at the same time.")
            .setCancelable(false)
            .setPositiveButton("Pick file", (d, w) -> openPicker())
            .setNegativeButton("Quit", (d, w) -> finish())
            .show();
    }

    private void openPicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        startActivityForResult(intent, REQUEST_PICK_ARCHIVES);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_PICK_ARCHIVES) {
            return;
        }
        List<Uri> picked = new ArrayList<>();
        if (resultCode == RESULT_OK && data != null) {
            if (data.getClipData() != null) {
                for (int i = 0; i < data.getClipData().getItemCount(); i++) {
                    picked.add(data.getClipData().getItemAt(i).getUri());
                }
            } else if (data.getData() != null) {
                picked.add(data.getData());
            }
        }
        if (picked.isEmpty()) {
            askForRomArchive();
            return;
        }
        status.setText("Copying game archive...");
        new Thread(() -> importArchives(picked)).start();
    }

    private void importArchives(List<Uri> picked) {
        String problem = null;
        for (Uri uri : picked) {
            String name = displayName(uri);
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                if (in == null || !isZip(in)) {
                    problem = name + " isn't an .o2r archive. Pick the oot.o2r made by the desktop build, not the ROM itself.";
                    continue;
                }
            } catch (IOException e) {
                problem = "Couldn't read " + name + ": " + e.getMessage();
                continue;
            }
            // The game only looks for these two names; anything else is treated as the vanilla game
            String target = name.toLowerCase(Locale.ROOT).contains("mq") ? "oot-mq.o2r" : "oot.o2r";
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                copy(in, new File(dataDir, target));
            } catch (IOException e) {
                problem = "Couldn't copy " + name + ": " + e.getMessage();
            }
        }
        final String message = problem;
        mainThread.post(() -> {
            if (hasRomArchive() && message == null) {
                startGame();
                return;
            }
            new AlertDialog.Builder(this)
                .setTitle(hasRomArchive() ? "Some files were skipped" : "That file didn't work")
                .setMessage(message)
                .setCancelable(false)
                .setPositiveButton(hasRomArchive() ? "Start game" : "Try again",
                    (d, w) -> { if (hasRomArchive()) startGame(); else askForRomArchive(); })
                .show();
        });
    }

    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                return c.getString(0);
            }
        } catch (RuntimeException ignored) {
            // fall through to the uri's last segment
        }
        String last = uri.getLastPathSegment();
        return last != null ? last : "file";
    }

    private static boolean isZip(InputStream in) throws IOException {
        byte[] magic = new byte[4];
        return in.read(magic) == 4 && magic[0] == 'P' && magic[1] == 'K' && magic[2] == 3 && magic[3] == 4;
    }

    /** Writes to a temp file first so a failed copy never leaves a truncated archive behind. */
    private static void copy(InputStream in, File dest) throws IOException {
        File tmp = new File(dest.getPath() + ".part");
        try (OutputStream out = new FileOutputStream(tmp)) {
            byte[] buf = new byte[1 << 20];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
        }
        if (!tmp.renameTo(dest)) {
            tmp.delete();
            throw new IOException("couldn't write " + dest.getName());
        }
    }

    private void startGame() {
        startActivity(new Intent(this, GameActivity.class));
        finish();
    }

    private void fail(String message) {
        status.setText(message);
        new AlertDialog.Builder(this)
            .setTitle("Can't start")
            .setMessage(message)
            .setCancelable(false)
            .setPositiveButton("Quit", (d, w) -> finish())
            .show();
    }
}
