#include "ui/dialogs.h"
#include "core/i18n.h"
#include "core/resource_manager.h"
#include "core/ui_theme.h"
#include "ui/fluent_lite/inputs.h"
#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

namespace jietuba {
namespace {
QString dialog_tr(const char *text) { return i18n_tr(text, "StandardDialog"); }

std::pair<QString, QString> run_file_dialog(QFileDialog *raw_dialog) {
    // The parent may close while the dialog is open and take the dialog with it.
    QPointer<QFileDialog> dialog(raw_dialog);
    const int accepted = exec_dialog(dialog);
    if (!accepted || !dialog) {
        delete dialog.data();
        return {};
    }
    const auto files = dialog->selectedFiles();
    std::pair<QString, QString> result{files.isEmpty() ? QString() : files.front(), dialog->selectedNameFilter()};
    delete dialog.data();
    return result;
}
}

int exec_dialog(QDialog *dialog, QWidget *owner) {
    if (dialog->windowModality() == Qt::NonModal) dialog->setWindowModality(Qt::WindowModal);
    if (owner && !dialog->parentWidget()) {
        if (auto *owner_handle = owner->window()->windowHandle()) {
            dialog->winId();
            dialog->windowHandle()->setTransientParent(owner_handle);
        }
    }
    return dialog->exec();
}

std::pair<QString, QString> get_open_file_name(QWidget *parent, const QString &caption, const QString &directory, const QString &name_filter) {
    auto *dialog = new QFileDialog(parent, caption, directory, name_filter);
    dialog->setFileMode(QFileDialog::ExistingFile);
    return run_file_dialog(dialog);
}

std::pair<QString, QString> get_save_file_name(QWidget *parent, const QString &caption, const QString &directory, const QString &name_filter) {
    auto *dialog = new QFileDialog(parent, caption, directory, name_filter);
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    return run_file_dialog(dialog);
}

QString get_existing_directory(QWidget *parent, const QString &caption, const QString &directory) {
    auto *dialog = new QFileDialog(parent, caption, directory);
    dialog->setFileMode(QFileDialog::Directory);
    dialog->setOption(QFileDialog::ShowDirsOnly);
    return run_file_dialog(dialog).first;
}

StandardDialog::StandardDialog(QWidget *parent, const QString &title, const QString &message) : QDialog(parent) {
    setWindowTitle(title);
    setObjectName("standardDialog");
    // A dialog bound to its parent's modality, never globally on top.
    setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
    setWindowModality(Qt::WindowModal);
    if (windowIcon().isNull()) setWindowIcon(ResourceManager::get_icon_by_name(QString::fromUtf8("托盘.svg")));
    setStyleSheet(R"(
            QDialog#standardDialog {
                background: #FFFFFF;
                color: #202124;
            }
            .QLabel {
                background: transparent;
                color: #202124;
                font-size: 13px;
            }
            QCheckBox {
                background: transparent;
                color: #202124;
                font-size: 13px;
            }
            QPushButton {
                min-width: 84px;
                min-height: 30px;
                padding: 4px 14px;
                background: #FFFFFF;
                color: #202124;
                border: 1px solid #D0D7DE;
                border-radius: 6px;
            }
            QPushButton:hover {
                background: #F5F7FA;
            }
            QPushButton:pressed {
                background: #EBEEF2;
            }
        )");
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(12);
    layout->setContentsMargins(20, 16, 20, 16);
    label = new QLabel(message, this);
    label->setWordWrap(true);
    layout->addWidget(label);
    button_box = new QDialogButtonBox(this);
    layout->addWidget(button_box);
}

QDialog *track_modeless_dialog(QDialog *dialog) {
    // Qt ownership (parent or WA_DeleteOnClose) already keeps the dialog alive.
    return dialog;
}

QVariant show_custom_confirm_dialog(QWidget *parent, const QString &title, const QString &message, const std::vector<DialogButton> &buttons_config) {
    StandardDialog dialog(parent, title, message);
    for (const auto &cfg : buttons_config)
        if (cfg.role == QDialogButtonBox::RejectRole || cfg.role == QDialogButtonBox::NoRole) {
            dialog.result_action = cfg.id;
            break;
        }
    for (const auto &cfg : buttons_config) {
        auto *button = dialog.button_box->addButton(cfg.text, cfg.role);
        if (cfg.is_default) button->setDefault(true);
        const auto id = cfg.id;
        QObject::connect(button, &QPushButton::clicked, &dialog, [&dialog, id] { dialog.result_action = id; });
        if (cfg.role == QDialogButtonBox::RejectRole || cfg.role == QDialogButtonBox::NoRole)
            QObject::connect(button, &QPushButton::clicked, &dialog, &QDialog::reject);
        else
            // Accept, yes, destructive and action roles all close the dialog.
            QObject::connect(button, &QPushButton::clicked, &dialog, &QDialog::accept);
    }
    exec_dialog(&dialog);
    return dialog.result_action;
}

bool show_confirm_dialog(QWidget *parent, const QString &title, const QString &message) {
    const std::vector<DialogButton> config = {
        {true, dialog_tr("Yes"), QDialogButtonBox::YesRole},
        {false, dialog_tr("No"), QDialogButtonBox::NoRole, true},
    };
    const auto result = show_custom_confirm_dialog(parent, title, message, config);
    return result.metaType().id() == QMetaType::Bool && result.toBool();
}

std::pair<bool, bool> show_confirm_checkbox_dialog(QWidget *parent, const QString &title, const QString &message, const QString &checkbox_text, bool checkbox_checked) {
    StandardDialog dialog(parent, title, message);
    auto *checkbox = new QCheckBox(checkbox_text, &dialog);
    checkbox->setChecked(checkbox_checked);
    auto *layout = static_cast<QVBoxLayout *>(dialog.layout());
    layout->insertWidget(layout->count() - 1, checkbox);
    auto *yes_button = dialog.button_box->addButton(dialog_tr("Yes"), QDialogButtonBox::YesRole);
    auto *no_button = dialog.button_box->addButton(dialog_tr("No"), QDialogButtonBox::NoRole);
    no_button->setDefault(true);
    QObject::connect(yes_button, &QPushButton::clicked, &dialog, &QDialog::accept);
    QObject::connect(no_button, &QPushButton::clicked, &dialog, &QDialog::reject);
    const bool confirmed = exec_dialog(&dialog) == QDialog::Accepted;
    return {confirmed, checkbox->isChecked()};
}

void show_info_dialog(QWidget *parent, const QString &title, const QString &message) {
    show_custom_confirm_dialog(parent, title, message, {{"ok", dialog_tr("OK"), QDialogButtonBox::AcceptRole, true}});
}

void show_warning_dialog(QWidget *parent, const QString &title, const QString &message) {
    show_custom_confirm_dialog(parent, title, message, {{"ok", dialog_tr("OK"), QDialogButtonBox::AcceptRole, true}});
}

namespace {
QScrollArea *make_scroll(QDialog *dialog) {
    auto *scroll = new QScrollArea(dialog);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QScrollArea::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet("QScrollArea { border: none; background: transparent; } QScrollArea > QWidget > QWidget { background: transparent; }");
    return scroll;
}
}

void show_text_dialog(QWidget *parent, const QString &title, const QString &content) {
    StandardDialog dialog(parent, title, "");
    dialog.resize(620, 430);
    dialog.label->hide();
    dialog.layout()->setContentsMargins(18, 14, 18, 14);
    dialog.layout()->setSpacing(10);
    auto *scroll = make_scroll(&dialog);
    auto *content_widget = new QWidget(scroll);
    auto *content_layout = new QVBoxLayout(content_widget);
    content_layout->setContentsMargins(0, 0, 0, 0);
    auto *content_label = new QLabel(content, content_widget);
    content_label->setWordWrap(true);
    content_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    set_own_style(content_label, "font-size: 13px; line-height: 1.45; background: transparent;");
    content_layout->addWidget(content_label);
    content_layout->addStretch(1);
    scroll->setWidget(content_widget);
    auto *layout = static_cast<QVBoxLayout *>(dialog.layout());
    layout->insertWidget(layout->count() - 1, scroll, 1);
    auto *ok_button = dialog.button_box->addButton(dialog_tr("OK"), QDialogButtonBox::AcceptRole);
    ok_button->setDefault(true);
    QObject::connect(ok_button, &QPushButton::clicked, &dialog, &QDialog::accept);
    exec_dialog(&dialog);
}

void show_update_dialog(QWidget *parent, const QString &title, const QString &content, const QString &download_caption,
                        const QString &download_url, const QString &download_button_text) {
    StandardDialog dialog(parent, title, "");
    dialog.resize(660, 480);
    dialog.label->hide();
    dialog.layout()->setContentsMargins(18, 14, 18, 14);
    dialog.layout()->setSpacing(10);
    auto *scroll = make_scroll(&dialog);
    auto *content_widget = new QWidget(scroll);
    auto *content_layout = new QVBoxLayout(content_widget);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(8);
    auto *content_label = new QLabel(content, content_widget);
    content_label->setTextFormat(Qt::PlainText);
    content_label->setWordWrap(true);
    content_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    set_own_style(content_label, "font-size: 13px; line-height: 1.45; background: transparent;");
    content_layout->addWidget(content_label);
    auto *download_label = new QLabel(download_caption, content_widget);
    set_own_style(download_label, "font-size: 13px; font-weight: 600; background: transparent;");
    content_layout->addWidget(download_label);
    const auto escaped_url = download_url.toHtmlEscaped().replace('\'', "&#x27;");
    auto *link_label = new QLabel(QString("<a href=\"%1\">%1</a>").arg(escaped_url), content_widget);
    link_label->setTextInteractionFlags(Qt::TextBrowserInteraction);
    link_label->setOpenExternalLinks(true);
    link_label->setWordWrap(true);
    content_layout->addWidget(link_label);
    content_layout->addStretch(1);
    scroll->setWidget(content_widget);
    auto *layout = static_cast<QVBoxLayout *>(dialog.layout());
    layout->insertWidget(layout->count() - 1, scroll, 1);
    auto *download_button = dialog.button_box->addButton(download_button_text, QDialogButtonBox::AcceptRole);
    QObject::connect(download_button, &QPushButton::clicked, &dialog, [download_url] { QDesktopServices::openUrl(QUrl(download_url)); });
    QObject::connect(download_button, &QPushButton::clicked, &dialog, &QDialog::accept);
    auto *ok_button = dialog.button_box->addButton(dialog_tr("OK"), QDialogButtonBox::RejectRole);
    QObject::connect(ok_button, &QPushButton::clicked, &dialog, &QDialog::accept);
    exec_dialog(&dialog);
}

QDialog *show_modeless_warning_dialog(QWidget *parent, const QString &title, const QString &message, bool single_line) {
    auto *dialog = new StandardDialog(parent, title, message);
    dialog->setWindowModality(Qt::NonModal);
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    dialog->setAttribute(Qt::WA_ShowWithoutActivating, true);
    auto *ok_button = dialog->button_box->addButton(dialog_tr("OK"), QDialogButtonBox::AcceptRole);
    ok_button->setDefault(true);
    QObject::connect(ok_button, &QPushButton::clicked, dialog, &QDialog::accept);
    if (single_line) {
        // Paths stay on one line; beyond the screen width they scroll and can still be copied.
        dialog->label->hide();
        auto *path_field = new LineEdit(dialog, false);
        path_field->setText(message);
        path_field->setReadOnly(true);
        path_field->setFrame(false);
        path_field->setFocusPolicy(Qt::ClickFocus);
        set_own_style(path_field, "background: transparent; color: #202124; font-size: 13px;");
        path_field->setCursorPosition(0);
        static_cast<QVBoxLayout *>(dialog->layout())->insertWidget(0, path_field);
        path_field->ensurePolished();
        int width = std::max(320, path_field->fontMetrics().horizontalAdvance(message) + 48);
        if (auto *screen = dialog->screen()) width = std::min(width, screen->availableGeometry().width() - 40);
        dialog->resize(width, dialog->sizeHint().height());
        ok_button->setFocus();
    }
    track_modeless_dialog(dialog);
    dialog->show();
    return dialog;
}

void show_error_dialog(QWidget *parent, const QString &title, const QString &message) {
    show_custom_confirm_dialog(parent, title, message, {{"ok", dialog_tr("OK"), QDialogButtonBox::AcceptRole, true}});
}
}
