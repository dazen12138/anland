package com.anland.consumer;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;

/**
 * Launcher entry point.
 *
 * <p>A tap returns to the last active Anland desktop window if one is still
 * alive (even while it is in the background); otherwise it hands off to
 * {@link MainActivity}, which keeps the original default-socket / Settings
 * fallback.
 *
 * <p>This is a dedicated activity instead of logic inside MainActivity because
 * the launcher must run the decision on every tap. With MainActivity as the
 * launcher target, Android can reuse the Settings task left by a previous
 * "daemon missing" bounce without ever calling MainActivity.onCreate(), so the
 * active window was never restored.
 */
public final class LauncherActivity extends Activity {
    private static final String TAG = "Anland";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        MainActivity reusable = MainActivity.findReusableWindowForLauncher();
        if (reusable != null) {
            Log.i(TAG, "launcher: returning to the active Anland window");
            ActivityManager am = getSystemService(ActivityManager.class);
            if (am != null) {
                try {
                    am.moveTaskToFront(reusable.getTaskId(), 0);
                } catch (RuntimeException e) {
                    // The task can disappear between the liveness check and the
                    // move; never crash the launcher entry over it.
                    Log.w(TAG, "launcher: moveTaskToFront failed", e);
                }
            }
            finish();
            return;
        }

        Log.i(TAG, "launcher: no active Anland window");
        // No window to restore: let MainActivity apply its normal logic (open
        // the default-socket desktop when available, otherwise Settings).
        startActivity(new Intent(this, MainActivity.class));
        finish();
    }
}
