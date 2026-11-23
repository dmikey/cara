// SPDX-License-Identifier: GPL-2.0-or-later
package org.cara.emu.sync;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.IntentSenderRequest;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;

import com.google.android.gms.auth.api.identity.AuthorizationRequest;
import com.google.android.gms.auth.api.identity.AuthorizationResult;
import com.google.android.gms.auth.api.identity.Identity;
import com.google.android.gms.common.api.Scope;

import org.cara.emu.R;

import java.util.Collections;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

public final class DriveSyncController {
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static final ExecutorService WORK = Executors.newSingleThreadExecutor();
    private static final AtomicBoolean RUNNING = new AtomicBoolean();
    private static final String ACCOUNT = "account";
    private static final String STATUS = "status";
    private static final String CONTENTS = "contents";
    private static long lastAutomatic = -60000;

    private final AppCompatActivity activity;
    private final ActivityResultLauncher<IntentSenderRequest> permission;
    private DriveSaveSync.Choice choice = DriveSaveSync.Choice.NONE;

    public DriveSyncController(AppCompatActivity activity) {
        this.activity = activity;
        permission = activity.registerForActivityResult(
                new ActivityResultContracts.StartIntentSenderForResult(), result -> {
                    if (result.getResultCode() != Activity.RESULT_OK || result.getData() == null) {
                        return;
                    }
                    try {
                        manualResult(Identity.getAuthorizationClient(activity)
                                .getAuthorizationResultFromIntent(result.getData()));
                    } catch (Exception failure) {
                        message(R.string.drive_authorization_failed);
                    }
                });
    }

    public void show() {
        if (RUNNING.get()) {
            message(R.string.drive_syncing);
            return;
        }
        SharedPreferences state = state(activity);
        boolean connected = state.contains(ACCOUNT);
        LinearLayout body = new LinearLayout(activity);
        body.setOrientation(LinearLayout.VERTICAL);
        int padding = Math.round(24 * activity.getResources().getDisplayMetrics().density);
        body.setPadding(padding, padding / 2, padding, 0);
        TextView status = new TextView(activity);
        status.setText(connected ? state.getString(STATUS, activity.getString(R.string.drive_connected))
                : activity.getString(R.string.drive_description));
        status.setPadding(0, 0, 0, padding / 2);
        body.addView(status);

        CheckBox saves = new CheckBox(activity);
        CheckBox states = new CheckBox(activity);
        saves.setText(R.string.drive_game_saves);
        states.setText(R.string.drive_save_states);
        int contents = state.getInt(CONTENTS, 0);
        saves.setChecked((contents & DriveSaveSync.GAME_SAVES) != 0);
        states.setChecked((contents & DriveSaveSync.SAVE_STATES) != 0);
        body.addView(saves);
        body.addView(states);

        AlertDialog.Builder builder = new AlertDialog.Builder(activity)
                .setTitle(R.string.drive_sync).setView(body)
                .setPositiveButton(connected ? R.string.drive_sync_now : R.string.drive_connect,
                        (dialog, button) -> authorize(DriveSaveSync.Choice.NONE))
                .setNegativeButton(android.R.string.cancel, null);
        if (connected) {
            builder.setNeutralButton(R.string.drive_disconnect, (dialog, button) -> {
                state.edit().remove(ACCOUNT).remove(STATUS).remove(CONTENTS).apply();
                message(R.string.drive_disconnected);
            });
        }
        AlertDialog dialog = builder.show();
        Runnable updateContents = () -> {
            int selected = (saves.isChecked() ? DriveSaveSync.GAME_SAVES : 0) |
                    (states.isChecked() ? DriveSaveSync.SAVE_STATES : 0);
            state.edit().putInt(CONTENTS, selected).apply();
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(selected != 0);
        };
        saves.setOnCheckedChangeListener((view, checked) -> updateContents.run());
        states.setOnCheckedChangeListener((view, checked) -> updateContents.run());
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(contents != 0);
    }

    private void authorize(DriveSaveSync.Choice action) {
        choice = action;
        Identity.getAuthorizationClient(activity).authorize(request())
                .addOnSuccessListener(activity, result -> {
                    if (result.hasResolution() && result.getPendingIntent() != null) {
                        permission.launch(new IntentSenderRequest.Builder(
                                result.getPendingIntent().getIntentSender()).build());
                    } else {
                        manualResult(result);
                    }
                })
                .addOnFailureListener(activity, failure -> message(R.string.drive_authorization_failed));
    }

    private void manualResult(AuthorizationResult authorization) {
        String token = authorization.getAccessToken();
        if (token == null) {
            message(R.string.drive_authorization_failed);
            return;
        }
        if (run(activity.getApplicationContext(), token, true, choice, result -> {
            if (activity.isFinishing() || activity.isDestroyed()) {
                return;
            }
            if (result != null && result.conflicts > 0) {
                showConflicts(result.conflicts);
            } else {
                String status = state(activity).getString(STATUS,
                        activity.getString(R.string.drive_sync_failed));
                new AlertDialog.Builder(activity).setTitle(R.string.drive_sync)
                        .setMessage(status).setPositiveButton(android.R.string.ok, null).show();
            }
        })) {
            message(R.string.drive_syncing);
        }
    }

    private void showConflicts(int number) {
        new AlertDialog.Builder(activity).setTitle(R.string.drive_conflicts_title)
                .setMessage(activity.getResources().getQuantityString(
                        R.plurals.drive_conflicts, number, number))
                .setPositiveButton(R.string.drive_keep_device,
                        (dialog, button) -> authorize(DriveSaveSync.Choice.DEVICE))
                .setNeutralButton(R.string.drive_keep_cloud,
                        (dialog, button) -> authorize(DriveSaveSync.Choice.DRIVE))
                .setNegativeButton(android.R.string.cancel, null).show();
    }

    private void message(int text) {
        Toast.makeText(activity, text, Toast.LENGTH_LONG).show();
    }

    public static void libraryOpened(Context context) {
        automatic(context.getApplicationContext(), false);
    }

    static void afterGame(Context context) {
        MAIN.post(() -> automatic(context.getApplicationContext(), true));
    }

    private static void automatic(Context context, boolean afterGame) {
        SharedPreferences state = state(context);
        long now = SystemClock.elapsedRealtime();
        if (!state.contains(ACCOUNT) || state.getInt(CONTENTS, 0) == 0 || RUNNING.get() ||
                (!afterGame && now - lastAutomatic < 60000)) {
            return;
        }
        lastAutomatic = now;
        Identity.getAuthorizationClient(context).authorize(request())
                .addOnSuccessListener(authorization -> {
                    String token = authorization.getAccessToken();
                    if (!authorization.hasResolution() && token != null) {
                        run(context, token, false, DriveSaveSync.Choice.NONE, null);
                    } else {
                        state.edit().putString(STATUS, context.getString(R.string.drive_reconnect)).apply();
                    }
                })
                .addOnFailureListener(failure -> state.edit().putString(STATUS,
                        context.getString(R.string.drive_reconnect)).apply());
    }

    private static AuthorizationRequest request() {
        return AuthorizationRequest.builder()
                .setRequestedScopes(Collections.singletonList(new Scope(DriveStorage.SCOPE))).build();
    }

    private interface Completion {
        void finish(DriveSaveSync.Result result);
    }

    private static boolean run(Context context, String token, boolean manual,
                               DriveSaveSync.Choice choice, Completion completion) {
        if (!RUNNING.compareAndSet(false, true)) {
            return false;
        }
        WORK.execute(() -> {
            SharedPreferences preferences = state(context);
            DriveSaveSync.Result outcome = null;
            try {
                int contents = preferences.getInt(CONTENTS, 0);
                if (contents == 0) {
                    return;
                }
                DriveStorage drive = new DriveStorage(token);
                String account = DriveSaveSync.accountKey(drive.account());
                String connected = preferences.getString(ACCOUNT, null);
                if (connected != null && !connected.equals(account)) {
                    preferences.edit().putString(STATUS, context.getString(R.string.drive_account_changed)).apply();
                } else if (connected != null || manual) {
                    preferences.edit().putString(ACCOUNT, account).apply();
                    outcome = new DriveSaveSync(context, drive).sync(account, contents, choice);
                    String status = outcome.conflicts > 0
                            ? context.getString(R.string.drive_conflicts_pending)
                            : context.getString(R.string.drive_complete, outcome.uploaded, outcome.restored);
                    preferences.edit().putString(STATUS, status).apply();
                }
            } catch (DriveSaveSync.Busy busy) {
                if (manual) {
                    preferences.edit().putString(STATUS, context.getString(R.string.drive_close_game)).apply();
                }
            } catch (Exception failure) {
                preferences.edit().putString(STATUS, context.getString(R.string.drive_sync_failed)).apply();
            } finally {
                RUNNING.set(false);
                if (completion != null) {
                    DriveSaveSync.Result result = outcome;
                    MAIN.post(() -> completion.finish(result));
                }
            }
        });
        return true;
    }

    private static SharedPreferences state(Context context) {
        return context.getSharedPreferences("drive-save-sync", Context.MODE_PRIVATE);
    }
}
