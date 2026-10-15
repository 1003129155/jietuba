#include "ui/watermark_input_dialog.h"
#include "canvas/items/watermark_item.h"
#include "core/ui_scale.h"
#include "settings/tool_settings.h"
#include "ui/dialogs.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/labels.h"
#include "ui/fluent_lite/theme.h"
#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QScreen>
#include <QShortcut>
#include <QTextCursor>
#include <QVBoxLayout>

namespace jietuba {
namespace {
QString wid_tr(const char *text) { return i18n_tr(text, "WatermarkInputDialog"); }
}

const std::vector<TimeFormat> &watermark_time_formats() {
    static const std::vector<TimeFormat> formats = {
        {"ISO 24h", "yyyy-MM-dd HH:mm:ss", "Year-month-day, 24-hour (China, Japan, Korea, ISO)"},
        {"Slash 24h", "yyyy/MM/dd HH:mm:ss", "Slash-separated date, 24-hour"},
        {"EU 24h", "dd/MM/yyyy HH:mm:ss", "Day/month/year, 24-hour (most of Europe)"},
        {"US 24h", "MM/dd/yyyy HH:mm:ss", "Month/day/year, 24-hour (United States)"},
        {"US 12h", "MM/dd/yyyy hh:mm:ss AP", "Month/day/year, 12-hour (United States and Canada)"},
        {"Date", "yyyy-MM-dd", "Date only"},
        {"Time", "HH:mm:ss", "Time only, 24-hour"},
        {"Time 12h", "hh:mm:ss AP", "Time only, 12-hour"},
        {"Weekday", "ddd yyyy-MM-dd HH:mm:ss", "Date and time with the weekday"},
        {"Millis", "yyyy-MM-dd HH:mm:ss.zzz", "With milliseconds, for logs"},
        {"Compact", "yyyyMMddHHmmss", "Compact, for file names"},
    };
    return formats;
}

QString default_watermark_time_format() { return watermark_time_formats().front().format; }

QString time_token(const QString &time_format) { return "$" + time_format + "$"; }

QString time_format_short_name(const QString &time_format) {
    for (const auto &entry : watermark_time_formats())
        if (time_format == entry.format) return entry.name;
    return time_format;
}

WatermarkInputDialog::WatermarkInputDialog(const QString &template_text, const QString &time_format, QWidget *parent) : QDialog(parent) {
    scale_dialog_font(*this);
    setWindowTitle(wid_tr("Input Watermark"));
    // Over the full-screen capture window, on top like the capture's colour picker.
    setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::WindowStaysOnTopHint);
    const auto &tokens = ui_tokens(this);
    setStyleSheet(QString("QDialog { background: %1; }").arg(tokens.window));
    time_format_ = default_watermark_time_format();
    for (const auto &entry : watermark_time_formats())
        if (time_format == entry.format) time_format_ = time_format;

    edit = new TextEdit(this);
    edit->setAcceptRichText(false);
    edit->setPlaceholderText(wid_tr("Type the watermark. Use the time button to insert the capture time."));
    edit->setPlainText(template_text);
    edit->setFixedHeight(dialog_scaled(BASE_EDIT_HEIGHT));
    edit->setStyleSheet(QString("QTextEdit { color: %1; background: %2; border: 1px solid %3; border-radius: %4px;"
                                " padding: %5px; font: %6px %7; selection-background-color: %8; }"
                                "QTextEdit:focus { border-color: %8; }")
                            .arg(tokens.text, tokens.input_background, tokens.border)
                            .arg(dialog_scaled(INPUT_RADIUS))
                            .arg(dialog_scaled(4))
                            .arg(dialog_scaled(14))
                            .arg(FONT_FAMILY, ACCENT));
    connect(edit, &QTextEdit::textChanged, this, &WatermarkInputDialog::refresh_preview);

    preview = new CaptionLabel("", this);
    preview->setWordWrap(true);
    preview->setTextFormat(Qt::PlainText);

    auto *time_label = new QLabel(wid_tr("Time:"), this);
    time_button = new QToolButton(this);
    time_button->setPopupMode(QToolButton::MenuButtonPopup);
    time_button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    time_button->setCursor(Qt::PointingHandCursor);
    connect(time_button, &QToolButton::clicked, this, [this] { insert_time(); });
    time_menu = new QMenu(time_button);
    time_menu->setToolTipsVisible(true);
    connect(time_menu, &QMenu::aboutToShow, this, &WatermarkInputDialog::fill_time_menu);
    time_button->setMenu(time_menu);
    time_button->setStyleSheet(QString("QToolButton { color: %1; background: %2; border: 1px solid %3; border-radius: %4px;"
                                       " padding: %5px %6px %5px %7px; min-height: %8px; }"
                                       "QToolButton:hover { border-color: %9; background: %10; }"
                                       "QToolButton::menu-button { width: %11px; border: none; }")
                                   .arg(tokens.text, tokens.input_background, tokens.border)
                                   .arg(dialog_scaled(INPUT_RADIUS))
                                   .arg(dialog_scaled(4))
                                   .arg(dialog_scaled(26))
                                   .arg(dialog_scaled(10))
                                   .arg(dialog_scaled(22))
                                   .arg(tokens.border_hover, tokens.surface_strong)
                                   .arg(dialog_scaled(20)));

    auto *apply_button = new PrimaryPushButton(wid_tr("Apply"), this);
    apply_button->setDefault(false);
    apply_button->setAutoDefault(false);
    connect(apply_button, &QPushButton::clicked, this, &QDialog::accept);
    auto *cancel_button = new PushButton(wid_tr("Cancel"), this);
    cancel_button->setAutoDefault(false);
    connect(cancel_button, &QPushButton::clicked, this, &QDialog::reject);
    for (const auto &sequence : {QKeySequence("Ctrl+Return"), QKeySequence("Ctrl+Enter")}) {
        auto *shortcut = new QShortcut(sequence, this);
        connect(shortcut, &QShortcut::activated, this, &QDialog::accept);
    }

    auto *bottom = new QHBoxLayout;
    bottom->setSpacing(dialog_scaled(8));
    bottom->addWidget(time_label);
    bottom->addWidget(time_button);
    bottom->addStretch(1);
    bottom->addWidget(apply_button);
    bottom->addWidget(cancel_button);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(dialog_scaled(16), dialog_scaled(14), dialog_scaled(16), dialog_scaled(14));
    root->setSpacing(dialog_scaled(8));
    root->addWidget(edit);
    root->addWidget(preview);
    root->addLayout(bottom);

    configure_dialog_controls(*this);
    setFixedWidth(dialog_scaled(BASE_WIDTH));
    sync_time_button();
    refresh_preview();
    edit->moveCursor(QTextCursor::End);
    edit->setFocus();
}

QString WatermarkInputDialog::template_text() const { return edit->toPlainText().trimmed(); }

void WatermarkInputDialog::insert_time(const QString &time_format) {
    if (!time_format.isEmpty()) {
        time_format_ = time_format;
        sync_time_button();
    }
    edit->insertPlainText(time_token(time_format_));
    edit->setFocus();
}

void WatermarkInputDialog::sync_time_button() {
    time_button->setText(time_format_short_name(time_format_));
    for (const auto &entry : watermark_time_formats()) {
        if (time_format_ == entry.format) {
            time_button->setToolTip(wid_tr("Insert the capture time") + "\n" + wid_tr(entry.description));
            break;
        }
    }
}

void WatermarkInputDialog::fill_time_menu() {
    time_menu->clear();
    const auto now = QDateTime::currentDateTime();
    for (const auto &entry : watermark_time_formats()) {
        // After the tab the example aligns like a shortcut column.
        auto *action = new QAction(QString("%1\t%2").arg(entry.name, expand_text(time_token(entry.format), now)), time_menu);
        action->setToolTip(wid_tr(entry.description));
        action->setCheckable(true);
        action->setChecked(time_format_ == entry.format);
        const QString format = entry.format;
        connect(action, &QAction::triggered, this, [this, format] { insert_time(format); });
        time_menu->addAction(action);
    }
}

void WatermarkInputDialog::refresh_preview() {
    const auto text = expand_text(edit->toPlainText());
    preview->setText(!text.isEmpty() ? wid_tr("Preview: %1").replace("%1", text) : QString());
    preview->setVisible(!text.isEmpty());
}

void WatermarkInputDialog::place_near(QWidget *anchor) {
    adjustSize();
    const auto top_left = anchor->mapToGlobal(QPoint(0, 0));
    auto *screen = QApplication::screenAt(top_left);
    if (!screen) screen = QApplication::primaryScreen();
    const auto area = screen->availableGeometry();
    const int gap = dialog_scaled(6);
    const auto frame = frameGeometry().size();
    int y = top_left.y() + anchor->height() + gap;
    if (y + frame.height() > area.bottom()) y = top_left.y() - frame.height() - gap;
    const int x = std::max(area.left(), std::min(top_left.x(), area.right() - frame.width()));
    move(x, std::max(area.top(), y));
}

std::optional<QString> edit_watermark_text(QWidget *anchor, ToolSettingsManager *settings_manager) {
    WatermarkInputDialog dialog(settings_manager->get_screenshot_watermark_text(), settings_manager->get_screenshot_watermark_time_format());
    dialog.place_near(anchor);
    const bool accepted = exec_dialog(&dialog, anchor) == QDialog::Accepted;
    settings_manager->set_screenshot_watermark_time_format(dialog.time_format());
    if (!accepted) return std::nullopt;
    return dialog.template_text();
}
}
