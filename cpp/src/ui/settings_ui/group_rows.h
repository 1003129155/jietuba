#pragma once
#include "core/ui_scale.h"
#include "ui/fluent_lite/labels.h"
#include "ui/settings_ui/components.h"
#include <QHBoxLayout>

namespace jietuba {
// The layout of a custom row card (WhiteCard) in a card group: icon, title and controls spaced like a setting card's,
// but only as tall as the controls need, since a settings page lists many of them.
inline QHBoxLayout *card_row_layout(QWidget *row) {
    row->setMinimumHeight(dialog_scaled(52));
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(dialog_scaled(16), dialog_scaled(8), card_right_margin(), dialog_scaled(8));
    layout->setSpacing(dialog_scaled(20));
    return layout;
}
}
