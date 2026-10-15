#include "ui/settings_ui/special_paste_dialog.h"
#include "core/i18n.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/labels.h"
#include "ui/fluent_lite/segmented.h"
#include "ui/fluent_lite/theme.h"
#include "ui/reorderable_rows.h"
#include <QApplication>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QScreen>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace jietuba {
namespace {
QString tr_text(const char *text) { return i18n_tr(text, "SpecialPasteDialog"); }
// Option names are the menu's, translated in the clipboard window's context.
QString menu_tr(const char *text) { return i18n_tr(text, "ClipboardWindow"); }

class PasteOptionRow : public DraggableRow {
public:
    PasteOptionRow(const special_paste::PasteOption &option, bool enabled, QWidget *parent = nullptr) : DraggableRow(parent), key(option.key) {
        check = new QCheckBox(this);
        check->setChecked(enabled);
        check->setAccessibleName(menu_tr(option.label));
        auto *name = new BodyLabel(menu_tr(option.label), this);
        auto *description = new CaptionLabel(tr_text(option.description), this);
        auto *column = new QVBoxLayout;
        column->setSpacing(dialog_scaled(2));
        column->addWidget(name);
        column->addWidget(description);
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(dialog_scaled(8), dialog_scaled(6), dialog_scaled(8), dialog_scaled(6));
        layout->setSpacing(dialog_scaled(10));
        layout->addWidget(grip);
        layout->addWidget(check);
        layout->addLayout(column, 1);
    }
    QString key;
    QCheckBox *check;
};
}

SpecialPasteDialog::SpecialPasteDialog(const QList<special_paste::Entry> &entries, QWidget *parent) : QDialog(parent) {
    scale_dialog_font(*this);
    setWindowTitle(tr_text("Manage Special Paste"));
    setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
    setStyleSheet(QString("QDialog { background: %1; }").arg(ui_tokens(this).window));

    tabs = new SegmentedWidget(this);
    tabs->addItem("text", tr_text("Text"));
    tabs->addItem("file", tr_text("Files"));
    stack_ = new QStackedWidget(this);
    for (const QString kind : {"text", "file"}) {
        auto *row_list = new ReorderableRowList(this);
        row_list->setStyleSheet(row_list->styleSheet() + scrollbar_qss(this));
        stack_->addWidget(row_list);
        lists_.insert(kind, row_list);
    }
    connect(tabs, &SegmentedWidget::currentItemChanged, this, [this](const QString &kind) { stack_->setCurrentWidget(lists_.value(kind)); });
    tabs->setCurrentItem("text");

    auto *hint = new CaptionLabel(
        tr_text("Check the items to show in the menu. Drag the handle to reorder them. Uncheck all items to hide Special Paste for that type."), this);
    reset_button = new TransparentPushButton(tr_text("Restore defaults"), this);
    connect(reset_button, &QPushButton::clicked, this, [this] { fill(special_paste::default_entries()); });
    auto *ok_button = new PrimaryPushButton(tr_text("OK"), this);
    connect(ok_button, &QPushButton::clicked, this, &QDialog::accept);
    auto *cancel_button = new PushButton(tr_text("Cancel"), this);
    connect(cancel_button, &QPushButton::clicked, this, &QDialog::reject);

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(dialog_scaled(8));
    buttons->addWidget(reset_button);
    buttons->addStretch(1);
    buttons->addWidget(ok_button);
    buttons->addWidget(cancel_button);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(dialog_scaled(16), dialog_scaled(16), dialog_scaled(16), dialog_scaled(14));
    root->setSpacing(dialog_scaled(10));
    root->addWidget(tabs);
    root->addWidget(stack_, 1);
    root->addWidget(hint);
    root->addLayout(buttons);

    fill(entries);
    configure_dialog_controls(*this);
    QScreen *screen = parentWidget() ? parentWidget()->screen() : QApplication::primaryScreen();
    int height = dialog_scaled(760);
    if (screen) height = std::min(height, screen->availableGeometry().height() - dialog_scaled(40));
    resize(dialog_scaled(560), std::max(dialog_scaled(360), height));
}

QList<special_paste::Entry> SpecialPasteDialog::entries() const {
    QList<special_paste::Entry> result;
    for (const QString kind : {"text", "file"})
        for (auto *row : lists_.value(kind)->rows()) {
            const auto *option_row = static_cast<const PasteOptionRow *>(row);
            result.append({option_row->key, option_row->check->isChecked()});
        }
    return special_paste::normalize(result);
}

void SpecialPasteDialog::fill(const QList<special_paste::Entry> &entries) {
    for (auto *row_list : lists_) row_list->clear();
    for (const auto &[key, enabled] : special_paste::normalize(entries)) {
        const auto &option = special_paste::OPTIONS_BY_KEY().at(key);
        auto *row = new PasteOptionRow(option, enabled);
        lists_.value(option.content_type)->add_row(row);
        configure_dialog_controls(*row);
    }
}
}
