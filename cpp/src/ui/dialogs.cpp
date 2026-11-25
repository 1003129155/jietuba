#include "ui/dialogs.h"
#include "core/i18n.h"
#include "core/resource_manager.h"
#include "core/qt_utils.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/theme.h"
#include "ui/fluent_lite/titlebar.h"
#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QPlainTextEdit>
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

StandardDialog::StandardDialog(QWidget *parent, const QString &title, const QString &message) : FrostedFramelessDialog(parent) {
    setWindowTitle(title);
    setObjectName("standardDialog");
    // A dialog bound to its parent's modality, never globally on top.
    setWindowModality(Qt::WindowModal);
    setResizeEnabled(false);
    scale_dialog_font(*this);
    if (windowIcon().isNull()) setWindowIcon(ResourceManager::get_icon_by_name(QString::fromUtf8("托盘.svg")));
    // Only the close button: the title is shown large in the content.
    auto *bar = new FluentTitleBar(this);
    setTitleBar(bar);
    for (QWidget *hidden : {static_cast<QWidget *>(bar->minBtn), static_cast<QWidget *>(bar->maxBtn), static_cast<QWidget *>(bar->iconLabel),
                            static_cast<QWidget *>(bar->titleLabel)})
        hidden->hide();
    bar->setDoubleClickEnabled(false);
    configure_dialog_control(*bar);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    content_ = new QWidget(this);
    content_->setObjectName("standard_dialog_content");
    content_layout_ = new QVBoxLayout(content_);
    content_layout_->setContentsMargins(dialog_scaled(24), dialog_scaled(22), dialog_scaled(24), dialog_scaled(24));
    content_layout_->setSpacing(dialog_scaled(10));
    title_label = new QLabel(title, content_);
    title_label->setObjectName("standard_dialog_title");
    title_label->setTextFormat(Qt::PlainText);
    title_label->setWordWrap(true);
    // Clear of the close button in the corner.
    title_label->setContentsMargins(0, 0, std::max(0, bar->closeBtn->width() - dialog_scaled(24)), 0);
    title_label->setVisible(!title.isEmpty());
    connect(this, &QWidget::windowTitleChanged, title_label, [this](const QString &text) {
        title_label->setText(text);
        title_label->setVisible(!text.isEmpty());
    });
    label = new QLabel(message, content_);
    label->setWordWrap(true);
    content_layout_->addWidget(title_label);
    content_layout_->addWidget(label);
    root->addWidget(content_, 1);
    button_box = new DialogButtonRow(this);
    root->addWidget(button_box);
    // The content starts at the top edge; the caption row stays above it so the close
    // button and the drag area receive the mouse.
    bar->raise();
    const auto apply_theme = [this] {
        const auto &t = ui_tokens(this);
        set_own_style(title_label, QString("color: %1; font: 600 %2px %3; background: transparent;").arg(t.fg).arg(dialog_scaled(20)).arg(FONT_FAMILY));
        set_own_style(label, QString("color: %1; font: %2px %3; background: transparent;").arg(t.fg).arg(dialog_scaled(14)).arg(FONT_FAMILY));
        update();
    };
    apply_theme();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, apply_theme);
}

QSize StandardDialog::sizeHint() const {
    const int width = std::max(dialog_scaled(440), layout()->minimumSize().width());
    const int height = layout()->hasHeightForWidth() ? layout()->totalHeightForWidth(width) : layout()->sizeHint().height();
    return {width, height};
}

void StandardDialog::paintEvent(QPaintEvent *) { paint_dialog_frame(this, frosted_corner_radius_, content_->geometry(), ui_tokens(this).card); }

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
        {false, dialog_tr("No"), QDialogButtonBox::NoRole},
    };
    const auto result = show_custom_confirm_dialog(parent, title, message, config);
    return result.metaType().id() == QMetaType::Bool && result.toBool();
}

std::pair<bool, bool> show_confirm_checkbox_dialog(QWidget *parent, const QString &title, const QString &message, const QString &checkbox_text, bool checkbox_checked) {
    StandardDialog dialog(parent, title, message);
    auto *checkbox = new QCheckBox(checkbox_text, &dialog);
    checkbox->setChecked(checkbox_checked);
    dialog.content_layout()->addWidget(checkbox);
    auto *yes_button = dialog.button_box->addButton(dialog_tr("Yes"), QDialogButtonBox::YesRole);
    auto *no_button = dialog.button_box->addButton(dialog_tr("No"), QDialogButtonBox::NoRole);
    QObject::connect(yes_button, &QPushButton::clicked, &dialog, &QDialog::accept);
    QObject::connect(no_button, &QPushButton::clicked, &dialog, &QDialog::reject);
    const bool confirmed = exec_dialog(&dialog) == QDialog::Accepted;
    return {confirmed, checkbox->isChecked()};
}

void show_info_dialog(QWidget *parent, const QString &title, const QString &message) {
    show_custom_confirm_dialog(parent, title, message, {{"ok", dialog_tr("OK"), QDialogButtonBox::AcceptRole}});
}

void show_warning_dialog(QWidget *parent, const QString &title, const QString &message) {
    show_custom_confirm_dialog(parent, title, message, {{"ok", dialog_tr("OK"), QDialogButtonBox::AcceptRole}});
}

namespace {
QScrollArea *make_scroll(QDialog *dialog) {
    auto *scroll = new QScrollArea(dialog);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QScrollArea::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet("QScrollArea { border: none; background: transparent; } QScrollArea > QWidget > QWidget { background: transparent; }" +
                          scrollbar_qss(dialog));
    return scroll;
}

// Long text below the title, readable and selectable.
QLabel *body_text(const QString &text, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    set_own_style(label, QString("color: %1; font: %2px %3; background: transparent;").arg(ui_tokens(parent).fg).arg(dialog_scaled(14)).arg(FONT_FAMILY));
    return label;
}
}

void show_text_dialog(QWidget *parent, const QString &title, const QString &content) {
    StandardDialog dialog(parent, title, "");
    dialog.resize(dialog_scaled(620), dialog_scaled(430));
    dialog.setResizeEnabled(true);
    dialog.label->hide();
    auto *scroll = make_scroll(&dialog);
    auto *content_widget = new QWidget(scroll);
    auto *content_layout = new QVBoxLayout(content_widget);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->addWidget(body_text(content, content_widget));
    content_layout->addStretch(1);
    scroll->setWidget(content_widget);
    dialog.content_layout()->addWidget(scroll, 1);
    auto *ok_button = dialog.button_box->addButton(dialog_tr("OK"), QDialogButtonBox::AcceptRole);
    QObject::connect(ok_button, &QPushButton::clicked, &dialog, &QDialog::accept);
    exec_dialog(&dialog);
}

void show_update_dialog(QWidget *parent, const QString &title, const QString &content, const QString &download_caption,
                        const QString &download_url, const QString &download_button_text) {
    StandardDialog dialog(parent, title, "");
    dialog.resize(dialog_scaled(660), dialog_scaled(480));
    dialog.setResizeEnabled(true);
    dialog.label->hide();
    auto *scroll = make_scroll(&dialog);
    auto *content_widget = new QWidget(scroll);
    auto *content_layout = new QVBoxLayout(content_widget);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(dialog_scaled(8));
    auto *content_label = body_text(content, content_widget);
    content_label->setTextFormat(Qt::PlainText);
    content_layout->addWidget(content_label);
    auto *download_label = new QLabel(download_caption, content_widget);
    set_own_style(download_label, QString("color: %1; font: 600 %2px %3; background: transparent;").arg(ui_tokens(&dialog).fg).arg(dialog_scaled(14)).arg(FONT_FAMILY));
    content_layout->addWidget(download_label);
    const auto escaped_url = download_url.toHtmlEscaped().replace('\'', "&#x27;");
    auto *link_label = new QLabel(QString("<a href=\"%1\">%1</a>").arg(escaped_url), content_widget);
    link_label->setTextInteractionFlags(Qt::TextBrowserInteraction);
    link_label->setOpenExternalLinks(true);
    link_label->setWordWrap(true);
    content_layout->addWidget(link_label);
    content_layout->addStretch(1);
    scroll->setWidget(content_widget);
    dialog.content_layout()->addWidget(scroll, 1);
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
    QObject::connect(ok_button, &QPushButton::clicked, dialog, &QDialog::accept);
    if (single_line) {
        // Paths stay on one line; beyond the screen width they scroll and can still be copied.
        dialog->label->hide();
        auto *path_field = new LineEdit(dialog, false);
        path_field->setText(message);
        path_field->setReadOnly(true);
        path_field->setFrame(false);
        path_field->setFocusPolicy(Qt::ClickFocus);
        set_own_style(path_field, QString("background: transparent; border: none; padding: 0; color: %1; font: %2px %3;")
                                      .arg(ui_tokens(dialog).fg).arg(dialog_scaled(14)).arg(FONT_FAMILY));
        path_field->setCursorPosition(0);
        dialog->content_layout()->addWidget(path_field);
        path_field->ensurePolished();
        int width = std::max(dialog->sizeHint().width(), path_field->fontMetrics().horizontalAdvance(message) + dialog_scaled(56));
        if (auto *screen = dialog->screen()) width = std::min(width, screen->availableGeometry().width() - dialog_scaled(40));
        dialog->resize(width, dialog->sizeHint().height());
        ok_button->setFocus();
    }
    track_modeless_dialog(dialog);
    dialog->show();
    return dialog;
}

QDialog *show_unplaced_overlay_translations(QWidget *parent, const QString &title, const QString &text, const QString &notice) {
    auto message = i18n_tr("Some translations do not fit. The original text is still shown in the image.", "OverlayTranslation");
    if (!notice.isEmpty()) message += QStringLiteral("\n\n") + notice;
    auto *dialog = new StandardDialog(parent, title, message);
    dialog->label->setTextFormat(Qt::PlainText);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setAttribute(Qt::WA_ShowWithoutActivating);
    auto *details = new QPlainTextEdit(dialog);
    details->setPlainText(text);
    details->setReadOnly(true);
    details->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    dialog->content_layout()->addWidget(details, 1);
    auto *ok = dialog->button_box->addButton(dialog_tr("OK"), QDialogButtonBox::AcceptRole);
    QObject::connect(ok, &QPushButton::clicked, dialog, &QDialog::accept);
    dialog->setResizeEnabled(true);
    dialog->resize(dialog_scaled(560), dialog_scaled(360));
    track_modeless_dialog(dialog);
    dialog->show();
    return dialog;
}

void show_error_dialog(QWidget *parent, const QString &title, const QString &message) {
    show_custom_confirm_dialog(parent, title, message, {{"ok", dialog_tr("OK"), QDialogButtonBox::AcceptRole}});
}
}
