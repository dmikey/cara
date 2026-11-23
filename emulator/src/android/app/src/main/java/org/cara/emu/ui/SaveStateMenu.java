// SPDX-License-Identifier: GPL-2.0-or-later
package org.cara.emu.ui;

import android.text.format.DateFormat;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Toast;

import androidx.appcompat.app.AlertDialog;

import java.util.Date;

import org.cara.emu.NativeLibrary;
import org.cara.emu.R;

final class SaveStateMenu {
    private static final int LIST = 0;
    private static final int SAVE = 1;
    private static final int LOAD = 2;

    // Callbacks can outlive an activity. Each request identifies its own game screen.
    private static int sNextRequestId;
    private final EmulationActivity mActivity;
    private int mRequestId;
    private boolean mSave;
    private AlertDialog mDialog;

    SaveStateMenu(EmulationActivity activity) {
        mActivity = activity;
    }

    void open(boolean save) {
        if (mRequestId != 0)
            return;
        mSave = save;
        request(LIST, 0, R.string.cara_state_checking);
    }

    private void request(int operation, int slot, int message) {
        if (mRequestId != 0)
            return;
        if (sNextRequestId == Integer.MAX_VALUE)
            sNextRequestId = 0;
        mRequestId = ++sNextRequestId;
        if (!NativeLibrary.requestSaveState(mRequestId, operation, slot)) {
            mRequestId = 0;
            showError(mActivity.getString(R.string.cara_state_not_ready));
            return;
        }
        mDialog = new AlertDialog.Builder(mActivity)
                        .setMessage(message)
                        .setCancelable(false)
                        .show();
    }

    void complete(int request, int operation, int slot, String error, long[] timestamps) {
        if (request != mRequestId || mActivity.isFinishing() || mActivity.isDestroyed())
            return;
        mRequestId = 0;
        close();
        if (!error.isEmpty()) {
            showError(error);
        } else if (operation == LIST && timestamps != null) {
            showSlots(timestamps);
        } else if (operation == SAVE || operation == LOAD) {
            int message = operation == SAVE ? R.string.cara_state_saved : R.string.cara_state_loaded;
            Toast.makeText(mActivity, mActivity.getString(message, slot), Toast.LENGTH_SHORT).show();
        }
    }

    private void showSlots(long[] timestamps) {
        String[] names = new String[timestamps.length];
        boolean anyUsable = false;
        for (int i = 0; i < timestamps.length; i++) {
            long seconds = timestamps[i];
            String status;
            if (seconds > 0) {
                Date date = new Date(seconds * 1000);
                status = DateFormat.getDateFormat(mActivity).format(date) + " "
                        + DateFormat.getTimeFormat(mActivity).format(date);
                anyUsable = true;
            } else {
                status = mActivity.getString(seconds < 0 ? R.string.cara_state_incompatible
                                                        : R.string.cara_state_empty);
            }
            names[i] = mActivity.getString(R.string.cara_state_slot, i + 1, status);
        }
        if (!mSave && !anyUsable) {
            showError(mActivity.getString(R.string.cara_state_no_usable));
            return;
        }
        ArrayAdapter<String> rows = new ArrayAdapter<String>(
                mActivity, android.R.layout.simple_list_item_1, names) {
            @Override
            public boolean areAllItemsEnabled() {
                return mSave;
            }

            @Override
            public boolean isEnabled(int position) {
                return mSave || timestamps[position] > 0;
            }

            @Override
            public View getView(int position, View convertView, ViewGroup parent) {
                View row = super.getView(position, convertView, parent);
                row.setEnabled(isEnabled(position));
                return row;
            }
        };
        mDialog = new AlertDialog.Builder(mActivity)
                .setTitle(mSave ? R.string.cara_state_save : R.string.cara_state_load)
                .setAdapter(rows, (dialog, selected) -> {
                    int slot = selected + 1;
                    if (mSave) {
                        confirmSave(slot);
                    } else {
                        request(LOAD, slot, R.string.cara_state_loading);
                    }
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    private void confirmSave(int slot) {
        mDialog = new AlertDialog.Builder(mActivity)
                .setTitle(mActivity.getString(R.string.cara_state_save_to, slot))
                .setMessage(R.string.cara_state_replace)
                .setPositiveButton(R.string.cara_state_save,
                        (dialog, button) -> request(SAVE, slot, R.string.cara_state_saving))
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    void close() {
        if (mDialog != null) {
            mDialog.dismiss();
            mDialog = null;
        }
    }

    private void showError(String error) {
        mDialog = new AlertDialog.Builder(mActivity)
                .setTitle(R.string.cara_state_title)
                .setMessage(error)
                .setPositiveButton(android.R.string.ok, null)
                .show();
    }
}
