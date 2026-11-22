// Modified for Cara on 2026-11-12; see NOTICE for attribution.
package org.cara.emu.settings.view;

import org.cara.emu.settings.model.BooleanSetting;
import org.cara.emu.settings.model.FloatSetting;
import org.cara.emu.settings.model.IntSetting;
import org.cara.emu.settings.model.Setting;

public final class CheckBoxSetting extends SettingsItem {
    private boolean mDefaultValue;

    public CheckBoxSetting(String key, String section, int titleId, int descriptionId,
                           boolean defaultValue, Setting setting) {
        super(key, section, setting, titleId, descriptionId);
        mDefaultValue = defaultValue;
    }

    public boolean isChecked() {
        Setting setting = getSetting();
        if (setting instanceof BooleanSetting) {
            return ((BooleanSetting) setting).getValue();
        }
        // Cara's native INI reader also accepts numeric zero and one.
        if (setting instanceof IntSetting) {
            int value = ((IntSetting) setting).getValue();
            return value == 1 || (value != 0 && mDefaultValue);
        }
        if (setting instanceof FloatSetting) {
            float value = ((FloatSetting) setting).getValue();
            return value == 1 || (value != 0 && mDefaultValue);
        }
        return mDefaultValue;
    }

    public BooleanSetting setChecked(boolean checked) {
        Setting existing = getSetting();
        if (existing instanceof BooleanSetting) {
            ((BooleanSetting) existing).setValue(checked);
            return null;
        }
        BooleanSetting setting = new BooleanSetting(getKey(), getSection(), checked);
        setSetting(setting);
        return setting;
    }

    @Override
    public int getType() {
        return TYPE_CHECKBOX;
    }
}
