// SPDX-License-Identifier: GPL-2.0-or-later
package org.cara.emu.settings;

import android.content.ComponentName;
import android.content.Context;
import android.content.pm.PackageManager;
import android.graphics.drawable.Drawable;
import android.os.Build;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.CheckedTextView;
import android.widget.Toast;

import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.content.res.AppCompatResources;

import org.cara.emu.R;

import java.util.ArrayList;
import java.util.List;

final class AppIconPicker {
    private enum Icon {
        CITRUS("Citrus", R.string.cara_icon_citrus, R.mipmap.ic_cara),
        SILVER("Silver", R.string.cara_icon_silver, R.mipmap.ic_cara_silver),
        AMBER("Amber", R.string.cara_icon_amber, R.mipmap.ic_cara_amber),
        LIME("Lime", R.string.cara_icon_lime, R.mipmap.ic_cara_lime),
        ONYX("Onyx", R.string.cara_icon_onyx, R.mipmap.ic_cara_onyx);

        final String alias;
        final int label;
        final int artwork;

        Icon(String alias, int label, int artwork) {
            this.alias = alias;
            this.label = label;
            this.artwork = artwork;
        }

        ComponentName component(Context context) {
            return new ComponentName(context.getPackageName(), "org.cara.emu.ui.CaraLauncher" + alias);
        }
    }

    static void show(Context context) {
        Icon[] icons = Icon.values();
        PackageManager packages = context.getPackageManager();
        int current = Icon.CITRUS.ordinal();
        for (Icon icon : icons) {
            if (packages.getComponentEnabledSetting(icon.component(context)) ==
                    PackageManager.COMPONENT_ENABLED_STATE_ENABLED) {
                current = icon.ordinal();
                break;
            }
        }

        ArrayAdapter<Icon> rows = new ArrayAdapter<Icon>(
                context, android.R.layout.select_dialog_singlechoice, icons) {
            @Override
            public View getView(int position, View convertView, ViewGroup parent) {
                CheckedTextView row = (CheckedTextView)super.getView(position, convertView, parent);
                Icon icon = icons[position];
                row.setText(icon.label);
                Drawable preview = AppCompatResources.getDrawable(context, icon.artwork);
                if (preview != null) {
                    float density = context.getResources().getDisplayMetrics().density;
                    int side = Math.round(42 * density);
                    preview.setBounds(0, 0, side, side);
                    row.setCompoundDrawablePadding(Math.round(14 * density));
                    row.setCompoundDrawablesRelative(preview, null, null, null);
                }
                return row;
            }
        };
        new AlertDialog.Builder(context).setTitle(R.string.cara_app_icon)
                .setSingleChoiceItems(rows, current, (dialog, index) -> {
                    try {
                        select(context, icons[index]);
                        dialog.dismiss();
                    } catch (RuntimeException error) {
                        Toast.makeText(context, R.string.cara_icon_error, Toast.LENGTH_SHORT).show();
                    }
                }).setNegativeButton(android.R.string.cancel, null).show();
    }

    private static void select(Context context, Icon selected) {
        PackageManager packages = context.getPackageManager();
        int flags = PackageManager.DONT_KILL_APP;
        if (Build.VERSION.SDK_INT >= 33) {
            List<PackageManager.ComponentEnabledSetting> changes = new ArrayList<>();
            for (Icon icon : Icon.values()) {
                changes.add(new PackageManager.ComponentEnabledSetting(icon.component(context),
                        icon == selected ? PackageManager.COMPONENT_ENABLED_STATE_ENABLED
                                         : PackageManager.COMPONENT_ENABLED_STATE_DISABLED, flags));
            }
            packages.setComponentEnabledSettings(changes);
        } else {
            // Keep an entry point available throughout the launcher transition.
            packages.setComponentEnabledSetting(selected.component(context),
                    PackageManager.COMPONENT_ENABLED_STATE_ENABLED, flags);
            for (Icon icon : Icon.values()) {
                if (icon != selected) {
                    packages.setComponentEnabledSetting(icon.component(context),
                            PackageManager.COMPONENT_ENABLED_STATE_DISABLED, flags);
                }
            }
        }
    }
}
