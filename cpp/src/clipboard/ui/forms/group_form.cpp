#include "clipboard/ui/forms/group_form.h"
#include "clipboard/core/manager.h"
#include "clipboard/services/group_service.h"
#include "clipboard/ui/dialogs/manage_dialog.h"
#include "clipboard/ui/forms/form_widgets.h"
#include "clipboard/ui/forms/group_icon_picker.h"
#include "clipboard/ui/group_kind_style.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/segmented.h"
#include <QHBoxLayout>

namespace jietuba::clipboard {
namespace {
// The name row: the current icon's preview tile on the left, the name input on the right.
void build_name_section(ManageDialog *dialog, const QString &name, const QString &icon, int group_type) {
    dialog->detail_layout->addWidget(field_label(ManageDialog::tr_text("Group Name"), false));
    auto *row = new QHBoxLayout;
    row->setSpacing(dialog_scaled(12));
    auto *preview = new IconPreview(icon, group_kind(group_type));
    dialog->icon_preview = preview;
    row->addWidget(preview);
    auto *input = new LineEdit;
    dialog->group_name_input = input;
    input->setPlaceholderText(ManageDialog::tr_text("Enter group name..."));
    input->setText(name);
    row->addWidget(input, 1, Qt::AlignVCenter);
    dialog->detail_layout->addLayout(row);
}

// The content types a filter group can show, in the order of the type filter menu.
const QStringList FILTER_KINDS = {"text", "image", "file"};

// Which content type a filter group shows; only visible while "Filter" is chosen.
void build_filter_kind_section(ManageDialog *dialog, const QString &kind, int form_token) {
    auto *box = new QWidget;
    auto *layout = new QHBoxLayout(box);
    layout->setContentsMargins(0, dialog_scaled(4), 0, 0);
    layout->setSpacing(dialog_scaled(12));
    layout->addWidget(field_label(ManageDialog::tr_text("Content Type"), false));

    auto *switch_widget = new SegmentedWidget;
    const QHash<QString, QString> labels = {{"text", ManageDialog::tr_text("Text")}, {"image", ManageDialog::tr_text("Image")}, {"file", ManageDialog::tr_text("File")}};
    for (const QString &key : FILTER_KINDS) switch_widget->addItem(key, labels.value(key));
    switch_widget->setCurrentItem(kind);
    QObject::connect(switch_widget, &SegmentedWidget::currentItemChanged, dialog, [dialog, form_token](const QString &key) { dialog->on_filter_kind_changed(key, form_token); });
    layout->addWidget(switch_widget);
    layout->addStretch();
    dialog->filter_kind_box = box;
    dialog->filter_kind_switch = switch_widget;
    dialog->detail_layout->addWidget(box);
    // Show only after it has a parent: a parentless widget made visible flashes as a window of its own.
    box->setVisible(dialog->radio_filter->isChecked());
}

void build_group_type_section(ManageDialog *dialog, int group_type, bool can_be_filter) {
    dialog->detail_layout->addWidget(field_label(ManageDialog::tr_text("Group Type")));

    auto *button_group = new QButtonGroup(dialog);
    dialog->group_type_btn_group = button_group;
    auto *radio_normal = new OptionCard(ManageDialog::tr_text("General"), ManageDialog::tr_text("Paste text or files"), KIND_NORMAL());
    auto *radio_file = new OptionCard(ManageDialog::tr_text("Quick Launch"), ManageDialog::tr_text("Open files or folders"), KIND_FILE());
    auto *radio_hidden = new OptionCard(ManageDialog::tr_text("Hidden"), ManageDialog::tr_text("Not shown on the panel"), KIND_HIDDEN());
    auto *radio_filter = new OptionCard(ManageDialog::tr_text("Filter"), ManageDialog::tr_text("Show one type"), KIND_FILTER());
    dialog->radio_normal = radio_normal;
    dialog->radio_file = radio_file;
    dialog->radio_hidden = radio_hidden;
    dialog->radio_filter = radio_filter;
    const int form_token = dialog->detail_form_token;
    const QList<OptionCard *> cards = {radio_normal, radio_file, radio_hidden, radio_filter};
    int index = 0;
    for (auto *card : cards) {
        button_group->addButton(card, index++);
        QObject::connect(card, &QAbstractButton::toggled, dialog, [dialog, form_token](bool checked) { dialog->on_group_type_toggled(checked, form_token); });
    }
    // A group that still has content cannot become a filter group: the option is not offered at all.
    radio_filter->setEnabled(can_be_filter);
    if (!can_be_filter) radio_filter->hide();

    const auto kind = filter_content_type(group_type);
    if (kind) radio_filter->setChecked(true);
    else if (group_type == GroupType::HIDDEN) radio_hidden->setChecked(true);
    else if (group_type == GroupType::FILE) radio_file->setChecked(true);
    else radio_normal->setChecked(true);

    // One row when the width allows, two per row otherwise, so the descriptions stay whole.
    dialog->detail_layout->addWidget(new CardGrid(cards));

    build_filter_kind_section(dialog, kind.value_or(FILTER_KINDS.first()), form_token);
}
}

void build_new_group_form(ManageDialog *dialog) {
    const QString icon = get_group_display_icon(std::nullopt, static_cast<int>(GroupType::NORMAL));
    build_name_section(dialog, "", icon, static_cast<int>(GroupType::NORMAL));
    build_group_type_section(dialog, static_cast<int>(GroupType::NORMAL), true);
    dialog->detail_layout->addWidget(field_label(ManageDialog::tr_text("Icon")));
    dialog->create_emoji_picker(icon);
}

void build_edit_group_form(ManageDialog *dialog, const Group &group) {
    const QString current_icon = get_group_display_icon(group.icon, group.group_type);
    build_name_section(dialog, group.name, current_icon, group.group_type);
    // A filter group stores no content, so a group that still has content cannot become one (the Rust core refuses too).
    const bool has_items = !dialog->manager->get_by_group(group.id, 0, 1).isEmpty();
    build_group_type_section(dialog, group.group_type, group.is_filter() || !has_items);
    dialog->detail_layout->addWidget(field_label(ManageDialog::tr_text("Icon")));
    dialog->create_emoji_picker(current_icon);
}
}
