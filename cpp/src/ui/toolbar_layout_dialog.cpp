#include "ui/toolbar_layout_dialog.h"
#include "core/i18n.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/labels.h"
#include "ui/fluent_lite/theme.h"
#include "ui/toolbar_layout.h"
#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QScreen>

namespace jietuba {
namespace {
QString tld_tr(const char *text) { return i18n_tr(text, "ToolbarLayoutDialog"); }

// Toolbar tooltips carry shortcut hints and are too long for a list, so rows use short names.
const char *button_name(const QString &key) {
    static const QMap<QString, const char *> names = {
        {"long_screenshot", "Long screenshot"}, {"save", "Save"}, {"copy", "Copy"}, {"screenshot_translate", "Screenshot translate"},
        {"text_recognize", "Recognize text"}, {"scan_code", "Scan code"}, {"gif", "Screen recording"},
        {"pen", "Pen"}, {"highlighter", "Highlighter"}, {"mosaic", "Mosaic"}, {"spotlight", "Spotlight"},
        {"watermark", "Watermark"}, {"arrow", "Arrow"}, {"number", "Number"}, {"rect", "Rectangle"},
        {"ellipse", "Ellipse"}, {"text", "Text"}, {"eraser", "Eraser"}, {"undo", "Undo"}, {"redo", "Redo"},
        {"cancel", "Cancel screenshot"}, {"pin", "Pin image"}, {"confirm", "Confirm"},
        {"annotation_pointer", "Pass through"}, {"annotation_clear", "Clear all"}, {"annotation_hide", "Hide drawings"},
        {"annotation_exit", "Exit annotation"}, {"annotation_board", "Board"}, {"annotation_cursor", "Cursor highlight"},
    };
    return names.value(key, "");
}

const std::pair<QString, const char *> MODE_NAMES[] = {
    {toolbar_layout::SHOW, "Always show"}, {toolbar_layout::MORE, "In more menu"}, {toolbar_layout::HIDE, "Always hide"}};
}

ToolbarLayoutRow::ToolbarLayoutRow(const QString &key, const QIcon &icon, QWidget *parent) : DraggableRow(parent), key(key) {
    // White backing: toolbar icons are dark glyphs drawn for a white toolbar and vanish on dark themes.
    auto *icon_label = new QLabel(this);
    icon_label->setFixedSize(dialog_scaled(28), dialog_scaled(28));
    icon_label->setAlignment(Qt::AlignCenter);
    icon_label->setPixmap(icon.pixmap(dialog_scaled(20), dialog_scaled(20)));
    set_own_style(icon_label, QString("background: #FFFFFF; border-radius: %1px;").arg(dialog_scaled(6)));
    combo = new ComboBox(this);
    configure_dialog_control(*combo);
    for (const auto &[mode, text] : MODE_NAMES) combo->addItem(tld_tr(text), mode);
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(dialog_scaled(8), dialog_scaled(3), dialog_scaled(8), dialog_scaled(3));
    layout->setSpacing(dialog_scaled(10));
    layout->addWidget(grip);
    layout->addWidget(icon_label);
    auto *name_label = new BodyLabel(tld_tr(button_name(key)), this);
    configure_dialog_control(*name_label);
    layout->addWidget(name_label, 1);
    layout->addWidget(combo);
}

QString ToolbarLayoutRow::mode() const { return combo->currentData().toString(); }

void ToolbarLayoutRow::set_mode(const QString &mode) { combo->setCurrentIndex(combo->findData(mode)); }

ToolbarLayoutDialog::ToolbarLayoutDialog(const QJsonArray &layout, const QMap<QString, QIcon> &icons, QWidget *parent,
                                         const toolbar_layout::LayoutProfile &profile)
    : QDialog(parent), profile_(profile) {
    scale_dialog_font(*this);
    setWindowTitle(tld_tr("Customize Toolbar"));
    // Opened from the full-screen topmost capture window, so it stays on top too.
    setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::WindowStaysOnTopHint);
    setStyleSheet(QString("QDialog { background: %1; }").arg(ui_tokens(this).window));
    list_ = new ReorderableRowList(this);
    // Locked buttons get no disabled row; only adjustable buttons have one.
    for (const auto &key : profile.order)
        if (!profile.locked.contains(key)) rows_[key] = new ToolbarLayoutRow(key, icons.value(key));
    auto *hint = new CaptionLabel(tld_tr("Drag the handle to reorder. Use the drop-down to change visibility."), this);
    configure_dialog_control(*hint);
    auto *reset_btn = new TransparentPushButton(tld_tr("Restore defaults"), this);
    configure_dialog_control(*reset_btn);
    connect(reset_btn, &QPushButton::clicked, this, [this] { fill(toolbar_layout::default_layout(profile_)); });
    auto *ok_btn = new PrimaryPushButton(tld_tr("OK"), this);
    configure_dialog_control(*ok_btn);
    connect(ok_btn, &QPushButton::clicked, this, &QDialog::accept);
    auto *cancel_btn = new PushButton(tld_tr("Cancel"), this);
    configure_dialog_control(*cancel_btn);
    connect(cancel_btn, &QPushButton::clicked, this, &QDialog::reject);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(reset_btn);
    buttons->addStretch(1);
    buttons->addWidget(ok_btn);
    buttons->addWidget(cancel_btn);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(dialog_scaled(16), dialog_scaled(16), dialog_scaled(16), dialog_scaled(14));
    root->setSpacing(dialog_scaled(10));
    root->addWidget(list_, 1);
    root->addWidget(hint);
    root->addLayout(buttons);
    fill(layout);
    fit_height_to_rows();
}

QJsonArray ToolbarLayoutDialog::entries() const {
    QJsonArray entries;
    for (auto *row : list_->rows()) {
        auto *layout_row = static_cast<ToolbarLayoutRow *>(row);
        entries.append(QJsonArray{layout_row->key, layout_row->mode()});
    }
    for (const auto &[index, key] : locked_slots_) entries.insert(std::min(index, static_cast<int>(entries.size())), QJsonArray{key, toolbar_layout::SHOW});
    return toolbar_layout::normalize_layout(entries, profile_);
}

void ToolbarLayoutDialog::fill(const QJsonArray &layout) {
    const auto normalized = toolbar_layout::normalize_layout(layout, profile_);
    locked_slots_.clear();
    int editable_index = 0;
    for (int index = 0; index < normalized.size(); ++index) {
        const auto entry = normalized[index].toArray();
        const auto key = entry[0].toString();
        if (profile_.locked.contains(key)) {
            locked_slots_.emplace_back(index, key);
            continue;
        }
        auto *row = rows_[key];
        list_->add_row(row, editable_index++);
        row->set_mode(entry[1].toString());
    }
}

void ToolbarLayoutDialog::fit_height_to_rows() {
    int content_height = list_->content_height();
    list_->setFixedHeight(content_height);
    int desired_height = sizeHint().height();
    auto *screen = parentWidget() ? parentWidget()->screen() : QApplication::primaryScreen();
    if (screen) {
        const int max_height = std::max(dialog_scaled(360), screen->availableGeometry().height() - dialog_scaled(40));
        if (desired_height > max_height) {
            const int non_list_height = desired_height - content_height;
            content_height = std::max(dialog_scaled(180), max_height - non_list_height);
            list_->setFixedHeight(content_height);
            desired_height = non_list_height + content_height;
        }
    }
    resize(dialog_scaled(480), desired_height);
}
}
