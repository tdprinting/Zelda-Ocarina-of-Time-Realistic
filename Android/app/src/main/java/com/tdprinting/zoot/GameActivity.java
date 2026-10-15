package com.tdprinting.zoot;

import android.content.pm.ActivityInfo;
import android.os.Bundle;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

/** Runs the game. LauncherActivity has already put soh.o2r and oot.o2r in getExternalFilesDir(). */
public class GameActivity extends SDLActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    @Override
    protected String[] getLibraries() {
        // SDL is linked statically into the game; libc++_shared comes in as its dependency
        return new String[] { "soh" };
    }

    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        // SDL picks portrait or landscape from the window size; the game is always landscape
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }
}
