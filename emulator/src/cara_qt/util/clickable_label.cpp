// Modified for Cara on 2026-11-12; see NOTICE for attribution.
// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "cara_qt/util/clickable_label.h"

ClickableLabel::ClickableLabel(QWidget* parent, Qt::WindowFlags f) : QLabel(parent) {}

void ClickableLabel::mouseReleaseEvent(QMouseEvent* event) {
    emit clicked();
}
