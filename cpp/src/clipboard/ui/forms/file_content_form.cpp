#include "clipboard/ui/forms/file_content_form.h"
#include "clipboard/ui/dialogs/manage_dialog.h"
#include "clipboard/ui/forms/form_widgets.h"
#include "clipboard/ui/layout_scale.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/theme.h"
#include <QDragEnterEvent>
#include <QHBoxLayout>
#include <QMimeData>
#include <QVBoxLayout>

namespace jietuba::clipboard {
FileDropZone::FileDropZone(std::function<void(const QString &)> on_path_dropped, const QString &prompt_text, QWidget *parent)
    : QFrame(parent), on_path_dropped_(std::move(on_path_dropped)) {
    setObjectName("FileDropZone");
    setAcceptDrops(true);
    setMinimumHeight(scale_ui(150));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(scale_x(12), scale_y(10), scale_x(12), scale_y(10));
    prompt_label = new QLabel(prompt_text, this);
    prompt_label->setAlignment(Qt::AlignCenter);
    prompt_label->setWordWrap(true);
    layout->addWidget(prompt_label);
    _apply_theme();
}

void FileDropZone::_apply_theme() {
    const auto &tokens = ui_tokens(this);
    // QLabel is a QFrame too; a bare QFrame selector would give the hint text a dashed frame of its own.
    default_style_ = QString("QFrame#FileDropZone {border: 2px dashed %1;border-radius: %2px;background: %3;}"
                             "QLabel {color: %4;font-size: %5px;padding: %6px;}")
                         .arg(tokens.border_hover)
                         .arg(scale_ui(10))
                         .arg(tokens.surface_subtle, tokens.text_muted)
                         .arg(scale_ui(13))
                         .arg(scale_ui(8));
    hover_style_ = QString("QFrame#FileDropZone {border: 2px dashed %1;border-radius: %2px;background: %3;}"
                           "QLabel {color: %1;font-size: %4px;padding: %5px;font-weight: 500;}")
                       .arg(tokens.accent)
                       .arg(scale_ui(10))
                       .arg(tokens.accent_soft)
                       .arg(scale_ui(13))
                       .arg(scale_ui(8));
    setStyleSheet(drag_active_ ? hover_style_ : default_style_);
}

QString FileDropZone::extract_first_local_path(const QList<QUrl> &urls) {
    for (const auto &url : urls) {
        const QString local_path = url.toLocalFile();
        if (!local_path.isEmpty()) return local_path;
    }
    return {};
}

bool FileDropZone::apply_dropped_urls(const QList<QUrl> &urls) {
    const QString first_path = extract_first_local_path(urls);
    if (first_path.isEmpty()) return false;
    on_path_dropped_(first_path);
    return true;
}

void FileDropZone::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasUrls() && !extract_first_local_path(event->mimeData()->urls()).isEmpty()) {
        event->acceptProposedAction();
        drag_active_ = true;
        setStyleSheet(hover_style_);
        return;
    }
    event->ignore();
}

void FileDropZone::dragMoveEvent(QDragMoveEvent *event) {
    if (event->mimeData()->hasUrls() && !extract_first_local_path(event->mimeData()->urls()).isEmpty()) {
        event->acceptProposedAction();
        return;
    }
    event->ignore();
}

void FileDropZone::dragLeaveEvent(QDragLeaveEvent *event) {
    drag_active_ = false;
    setStyleSheet(default_style_);
    QFrame::dragLeaveEvent(event);
}

void FileDropZone::dropEvent(QDropEvent *event) {
    drag_active_ = false;
    setStyleSheet(default_style_);
    if (apply_dropped_urls(event->mimeData()->urls())) {
        event->acceptProposedAction();
        return;
    }
    event->ignore();
}

void build_file_content_form(ManageDialog *dialog, bool add_stretch) {
    dialog->selected_file_path.reset();
    dialog->detail_layout->addWidget(field_label(ManageDialog::tr_text("Title (Optional)"), false));

    auto *title_input = new LineEdit;
    dialog->title_input = title_input;
    title_input->setPlaceholderText(ManageDialog::tr_text("Enter title..."));
    dialog->detail_layout->addWidget(title_input);

    dialog->detail_layout->addWidget(field_label(ManageDialog::tr_text("File Path")));
    auto *file_path_input = new LineEdit;
    dialog->file_path_input = file_path_input;
    file_path_input->setPlaceholderText(ManageDialog::tr_text("Select a file..."));
    file_path_input->setReadOnly(true);
    dialog->detail_layout->addWidget(file_path_input);

    auto *btn_row = new QHBoxLayout;
    auto *browse_file_btn = new PushButton(ManageDialog::tr_text("Browse File"));
    QObject::connect(browse_file_btn, &QPushButton::clicked, dialog, [dialog] { dialog->on_browse_file(); });
    btn_row->addWidget(browse_file_btn);
    auto *browse_folder_btn = new PushButton(ManageDialog::tr_text("Browse Folder"));
    QObject::connect(browse_folder_btn, &QPushButton::clicked, dialog, [dialog] { dialog->on_browse_folder(); });
    btn_row->addWidget(browse_folder_btn);
    btn_row->addStretch();
    dialog->detail_layout->addLayout(btn_row);

    auto *drop_zone = new FileDropZone([dialog](const QString &path) { dialog->set_selected_file_path(path); }, ManageDialog::tr_text("Drag a file or folder here"), dialog);
    dialog->file_drop_zone = drop_zone;
    dialog->detail_layout->addWidget(drop_zone);

    if (add_stretch) dialog->detail_layout->addStretch();
}

void build_edit_file_content_form(ManageDialog *dialog, const ClipboardItem &item, const QString &file_path) {
    build_file_content_form(dialog, false);
    dialog->title_input->setText(item.title.value_or(QString()));
    dialog->selected_file_path = file_path.isEmpty() ? std::nullopt : std::optional<QString>(file_path);
    dialog->file_path_input->setText(file_path);
}
}
