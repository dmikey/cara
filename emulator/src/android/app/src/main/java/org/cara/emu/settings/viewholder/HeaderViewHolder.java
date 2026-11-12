// Modified for Cara on 2026-11-12; see NOTICE for attribution.
package org.cara.emu.settings.viewholder;

import android.view.View;
import android.widget.TextView;
import org.cara.emu.R;
import org.cara.emu.settings.SettingsAdapter;
import org.cara.emu.settings.view.SettingsItem;

public final class HeaderViewHolder extends SettingViewHolder {
    private TextView mHeaderName;

    public HeaderViewHolder(View itemView, SettingsAdapter adapter) {
        super(itemView, adapter);
        itemView.setOnClickListener(null);
    }

    @Override
    protected void findViews(View root) {
        mHeaderName = root.findViewById(R.id.text_header_name);
    }

    @Override
    public void bind(SettingsItem item) {
        mHeaderName.setText(item.getNameId());
    }

    @Override
    public void onClick(View clicked) {
        // no-op
    }
}
