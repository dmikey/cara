// Modified for Cara on 2026-11-12; see NOTICE for attribution.
package org.cara.emu.settings.view;

import org.cara.emu.settings.model.Setting;

public final class HeaderSetting extends SettingsItem {
    public HeaderSetting(String key, Setting setting, int titleId, int descriptionId) {
        super(key, null, setting, titleId, descriptionId);
    }

    @Override
    public int getType() {
        return SettingsItem.TYPE_HEADER;
    }
}
