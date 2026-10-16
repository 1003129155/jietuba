#include "clipboard/ui/forms/group_icon_picker.h"
#include "clipboard/ui/dialogs/manage_dialog.h"
#include "clipboard/ui/forms/form_widgets.h"
#include "clipboard/ui/layout_scale.h"
#include "clipboard/ui/resources/emoji_data.h"
#include "core/py_text.h"
#include "core/ui_theme.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/theme.h"
#include <QGridLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTextBoundaryFinder>
#include <QTimer>

namespace jietuba::clipboard {
void create_group_icon_picker(ManageDialog *dialog, const QString &current_icon) {
    const int form_token = dialog->detail_form_token;
    auto *input_row = new QHBoxLayout;
    input_row->setSpacing(scale_ui(12));

    auto *preview = new IconPreview(current_icon);
    dialog->icon_preview = preview;
    input_row->addWidget(preview);

    auto *icon_input = new LineEdit;
    dialog->icon_input = icon_input;
    icon_input->setPlaceholderText(ManageDialog::tr_text("Enter or paste emoji..."));
    // No length cap: on_icon_input_changed keeps one character, and 👩‍❤️‍💋‍👨 alone is 11 UTF-16 units.
    icon_input->setText(current_icon);
    icon_input->setFixedWidth(scale_ui(180));
    QObject::connect(icon_input, &QLineEdit::textChanged, dialog, [dialog, form_token](const QString &text) { dialog->on_icon_input_changed_checked(text, form_token); });
    input_row->addWidget(icon_input);
    input_row->addWidget(field_hint(ManageDialog::tr_text("or pick one below")));
    input_row->addStretch();
    dialog->detail_layout->addLayout(input_row);

    const auto [group_order, emoji_groups] = get_emoji_groups();

    auto *tab_bar = new QHBoxLayout;
    tab_bar->setSpacing(0);
    tab_bar->setContentsMargins(0, scale_y(4), 0, 0);
    dialog->emoji_tab_buttons.clear();
    dialog->emoji_group_order = group_order;
    dialog->emoji_groups = emoji_groups;

    for (int idx = 0; idx < group_order.size(); ++idx) {
        auto *btn = new QPushButton(get_group_icon(group_order[idx]));
        btn->setFixedSize(scale_x(36), scale_y(30));
        btn->setCursor(Qt::PointingHandCursor);
        btn->setCheckable(true);
        btn->setStyleSheet(emoji_tab_style(false, dialog));
        QObject::connect(btn, &QPushButton::clicked, dialog, [dialog, idx, form_token] { dialog->switch_emoji_group_checked(idx, form_token); });
        tab_bar->addWidget(btn);
        dialog->emoji_tab_buttons.append(btn);
    }
    tab_bar->addStretch();
    dialog->detail_layout->addLayout(tab_bar);

    auto *scroll = new QScrollArea;
    dialog->emoji_scroll = scroll;
    scroll->setWidgetResizable(true);
    scroll->setMinimumHeight(scale_y(110));
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // The scroll bar style cascades from the manage window's scrollbar_qss and follows the theme.
    scroll->setStyleSheet("QScrollArea { border: none; background: transparent; }");
    dialog->detail_layout->addWidget(scroll, 1);

    dialog->emoji_current_idx = 0;
    QTimer::singleShot(0, dialog, [dialog, form_token] { dialog->switch_emoji_group_checked(0, form_token); });
}

IconPreview::IconPreview(const QString &icon) : QLabel(icon) {
    setFixedSize(scale_ui(48), scale_ui(48));
    setAlignment(Qt::AlignCenter);
    _apply_theme();
}

void IconPreview::_apply_theme() {
    const auto &tokens = ui_tokens(this);
    setStyleSheet(QString("QLabel { font-size: %1px; background: %2; border: none; border-radius: %3px; }").arg(scale_ui(26)).arg(tokens.accent_soft).arg(scale_ui(12)));
}

QString emoji_tab_style(bool active, const QWidget *dialog) {
    const auto &tokens = ui_tokens(dialog);
    if (active)
        return QString(R"(
            QPushButton {
                color: %1;
                background: %2; border: none;
                border-bottom: 2px solid %3;
                border-radius: 0px; font-size: %4px;
                padding: %5px 0px;
            }
        )")
            .arg(tokens.text, tokens.accent_soft, tokens.accent)
            .arg(scale_ui(18))
            .arg(scale_y(2));
    return QString(R"(
        QPushButton {
            color: %1;
            background: transparent; border: none;
            border-bottom: 2px solid transparent;
            border-radius: 0px; font-size: %2px;
            padding: %3px 0px;
        }
        QPushButton:hover { background: %4; }
    )")
        .arg(tokens.text)
        .arg(scale_ui(18))
        .arg(scale_y(2))
        .arg(tokens.surface_hover);
}

QString emoji_btn_style(const QWidget *dialog) {
    const auto &tokens = ui_tokens(dialog);
    return QString(R"(
        QPushButton {
            background: transparent; border: none;
            border-radius: %1px; font-size: %2px; padding: 0px;
        }
        QPushButton:hover {
            background: %3;
        }
        QPushButton:pressed {
            background: %4;
        }
    )")
        .arg(scale_ui(4))
        .arg(scale_ui(22))
        .arg(tokens.accent_soft, tokens.surface_hover);
}

void switch_emoji_group(ManageDialog *dialog, int group_idx) {
    dialog->emoji_current_idx = group_idx;
    for (int i = 0; i < dialog->emoji_tab_buttons.size(); ++i) {
        dialog->emoji_tab_buttons[i]->setChecked(i == group_idx);
        dialog->emoji_tab_buttons[i]->setStyleSheet(emoji_tab_style(i == group_idx, dialog));
    }
    const QStringList emojis = dialog->emoji_groups.value(dialog->emoji_group_order.value(group_idx));

    const int btn_size = scale_ui(36);
    const int spacing = scale_ui(2);
    int avail_w = dialog->emoji_scroll->viewport()->width() - scale_x(4);
    if (avail_w < btn_size * 2) avail_w = scale_x(400);
    const int cols = std::max(1, avail_w / (btn_size + spacing));

    auto *container = new QWidget;
    set_own_style(container, "background: transparent;");
    container->setMaximumWidth(avail_w + scale_x(4));
    auto *grid = new QGridLayout(container);
    grid->setSpacing(spacing);
    grid->setContentsMargins(scale_ui(2), scale_ui(2), scale_ui(2), scale_ui(2));

    const QString btn_style = emoji_btn_style(dialog);
    const int form_token = dialog->detail_form_token;
    for (int i = 0; i < emojis.size(); ++i) {
        auto *btn = new QPushButton(emojis[i]);
        btn->setFixedSize(btn_size, btn_size);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(btn_style);
        QObject::connect(btn, &QPushButton::clicked, dialog, [dialog, icon = emojis[i], form_token] { dialog->on_preset_icon_clicked_checked(icon, form_token); });
        grid->addWidget(btn, i / cols, i % cols);
    }
    grid->setColumnStretch(cols, 1);
    grid->setRowStretch(static_cast<int>(emojis.size()) / cols + 1, 1);
    dialog->emoji_scroll->setWidget(container);
}

QString first_icon_cluster(const QString &text) {
    const QString rest = py_lstrip(text);
    if (rest.isEmpty()) return {};
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, rest);
    const qsizetype end = finder.toNextBoundary();
    return end > 0 ? rest.left(end) : rest;
}

void on_icon_input_changed(ManageDialog *dialog, const QString &text) {
    if (py_strip(text).isEmpty()) {
        dialog->icon_preview->setText(QString::fromUtf8("📁"));
        return;
    }
    const QString first_char = first_icon_cluster(text);
    if (first_char.isEmpty()) {
        dialog->icon_preview->setText(QString::fromUtf8("📁"));
        return;
    }
    if (py_len(py_strip(text)) > py_len(first_char)) {
        dialog->icon_input->blockSignals(true);
        dialog->icon_input->setText(first_char);
        dialog->icon_input->blockSignals(false);
    }
    dialog->icon_preview->setText(first_char);
}

void on_preset_icon_clicked(ManageDialog *dialog, const QString &icon) {
    dialog->icon_input->setText(icon);
    dialog->icon_preview->setText(icon);
}
}
