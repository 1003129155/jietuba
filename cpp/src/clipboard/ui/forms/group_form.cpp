#include "clipboard/ui/forms/group_form.h"
#include "clipboard/services/group_service.h"
#include "clipboard/ui/dialogs/manage_dialog.h"
#include "clipboard/ui/forms/form_widgets.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/inputs.h"
#include <QHBoxLayout>

namespace jietuba::clipboard {
namespace {
void build_name_section(ManageDialog *dialog, const QString &name) {
    dialog->detail_layout->addWidget(field_label(ManageDialog::tr_text("Group Name"), false));
    auto *input = new LineEdit;
    dialog->group_name_input = input;
    input->setPlaceholderText(ManageDialog::tr_text("Enter group name..."));
    input->setText(name);
    dialog->detail_layout->addWidget(input);
}

void build_group_type_section(ManageDialog *dialog, bool is_file_group, bool is_hidden = false) {
    dialog->detail_layout->addWidget(field_label(ManageDialog::tr_text("Group Type")));

    auto *button_group = new QButtonGroup(dialog);
    dialog->group_type_btn_group = button_group;
    auto *radio_normal = new OptionCard(ManageDialog::tr_text("General"), ManageDialog::tr_text("Paste text or files"));
    auto *radio_file = new OptionCard(ManageDialog::tr_text("Quick Launch"), ManageDialog::tr_text("Open files or folders"));
    auto *radio_hidden = new OptionCard(ManageDialog::tr_text("Hidden"), ManageDialog::tr_text("Not shown on the panel"));
    dialog->radio_normal = radio_normal;
    dialog->radio_file = radio_file;
    dialog->radio_hidden = radio_hidden;
    const int form_token = dialog->detail_form_token;
    int index = 0;
    for (auto *card : {radio_normal, radio_file, radio_hidden}) {
        button_group->addButton(card, index++);
        QObject::connect(card, &QAbstractButton::toggled, dialog, [dialog, form_token](bool checked) { dialog->on_group_type_toggled(checked, form_token); });
    }

    if (is_hidden) radio_hidden->setChecked(true);
    else if (is_file_group) radio_file->setChecked(true);
    else radio_normal->setChecked(true);

    auto *row = new QHBoxLayout;
    row->setSpacing(dialog_scaled(10));
    row->addWidget(radio_normal);
    row->addWidget(radio_file);
    row->addWidget(radio_hidden);
    dialog->detail_layout->addLayout(row);
}
}

void build_new_group_form(ManageDialog *dialog) {
    build_name_section(dialog, "");
    build_group_type_section(dialog, false, false);
    dialog->detail_layout->addWidget(field_label(ManageDialog::tr_text("Icon")));
    dialog->create_emoji_picker();
}

void build_edit_group_form(ManageDialog *dialog, const Group &group) {
    build_name_section(dialog, group.name);
    const bool is_file_group = group.group_type == 1;
    const bool is_hidden = group.group_type == 2;
    build_group_type_section(dialog, is_file_group, is_hidden);
    dialog->detail_layout->addWidget(field_label(ManageDialog::tr_text("Icon")));
    dialog->create_emoji_picker(get_group_display_icon(group.icon, is_file_group, is_hidden));
}
}
