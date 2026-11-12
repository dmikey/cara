// Modified for Cara on 2026-11-12; see NOTICE for attribution.
package org.cara.emu.settings.view;

import org.cara.emu.settings.MenuTag;
import org.cara.emu.settings.model.Setting;

public final class SubmenuSetting extends SettingsItem {
    private MenuTag mMenuKey;

    public SubmenuSetting(String key, Setting setting, int titleId, int descriptionId,
                          MenuTag menuKey) {
        super(key, null, setting, titleId, descriptionId);
        mMenuKey = menuKey;
    }

    public MenuTag getMenuKey() {
        return mMenuKey;
    }

    @Override
    public int getType() {
        return TYPE_SUBMENU;
    }
}
