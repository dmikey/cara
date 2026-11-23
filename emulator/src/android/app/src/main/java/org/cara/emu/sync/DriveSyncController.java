// SPDX-License-Identifier: GPL-2.0-or-later
package org.cara.emu.sync;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
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
        AlertDialog.Builder dialog = new AlertDialog.Builder(activity)
                .setTitle(R.string.drive_sync)
                .setMessage(connected ? state.getString(STATUS,
                        activity.getString(R.string.drive_connected))
                        : activity.getString(R.string.drive_description))
                .setPositiveButton(connected ? R.string.drive_sync_now : R.string.drive_connect,
                        (d, button) -> authorize(DriveSaveSync.Choice.NONE))
                .setNegativeButton(android.R.string.cancel, null);
        if (connected) {
            dialog.setNeutralButton(R.string.drive_disconnect, (d, button) -> {
                state.edit().remove(ACCOUNT).remove(STATUS).apply();
                message(R.string.drive_disconnected);
            });
        }
        dialog.show();
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
        if (!state.contains(ACCOUNT) || RUNNING.get() ||
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
                DriveStorage drive = new DriveStorage(token);
                String account = DriveSaveSync.accountKey(drive.account());
                String connected = preferences.getString(ACCOUNT, null);
                if (connected != null && !connected.equals(account)) {
                    preferences.edit().putString(STATUS, context.getString(R.string.drive_account_changed)).apply();
                } else if (connected != null || manual) {
                    preferences.edit().putString(ACCOUNT, account).apply();
                    outcome = new DriveSaveSync(context, drive).sync(account, choice);
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
