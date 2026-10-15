#include "clipboard/ui/forms/text_content_form.h"
#include "clipboard/ui/dialogs/manage_dialog.h"
#include "clipboard/ui/forms/form_widgets.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/inputs.h"

namespace jietuba::clipboard {
namespace {
void build_form(ManageDialog *dialog, const QString &title_placeholder, const QString &title, const QString &content) {
    dialog->detail_layout->addWidget(field_label(ManageDialog::tr_text("Title (Optional)"), false));
    auto *title_input = new LineEdit;
    dialog->title_input = title_input;
    title_input->setPlaceholderText(title_placeholder);
    title_input->setText(title);
    dialog->detail_layout->addWidget(title_input);

    dialog->detail_layout->addWidget(field_label(ManageDialog::tr_text("Content")));
    auto *content_edit = new TextEdit;
    dialog->content_edit = content_edit;
    content_edit->setPlainText(content);
    content_edit->setLineWrapMode(QTextEdit::NoWrap);
    content_edit->setMinimumHeight(dialog_scaled(220));
    dialog->detail_layout->addWidget(content_edit, 1);
}
}

void build_text_content_form(ManageDialog *dialog) {
    build_form(dialog, ManageDialog::tr_text("Enter title (e.g., Restart Command)..."), "", "");
    dialog->content_edit->setPlaceholderText(ManageDialog::tr_text("Enter text content to save..."));
}

void build_edit_text_content_form(ManageDialog *dialog, const ClipboardItem &item) {
    build_form(dialog, ManageDialog::tr_text("Enter title..."), item.title.value_or(QString()), item.content);
}
}
