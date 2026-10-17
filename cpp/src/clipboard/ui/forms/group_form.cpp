#include "clipboard/ui/forms/group_form.h"
#include "clipboard/core/manager.h"
#include "clipboard/services/group_service.h"
#include "clipboard/ui/dialogs/manage_dialog.h"
#include "clipboard/ui/forms/form_widgets.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/segmented.h"
#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>

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

// The content types a filter group can show, in the order of the type filter menu.
const QStringList FILTER_KINDS = {"text", "image", "file"};

// Which content type a filter group shows; only visible while "Filter" is chosen.
void build_filter_kind_section(ManageDialog *dialog, const QString &kind, int form_token) {
    auto *box = new QWidget;
    auto *layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(dialog_scaled(6));
    layout->addWidget(field_label(ManageDialog::tr_text("Content Type")));

    auto *switch_widget = new SegmentedWidget;
    const QHash<QString, QString> labels = {{"text", ManageDialog::tr_text("Text")}, {"image", ManageDialog::tr_text("Image")}, {"file", ManageDialog::tr_text("File")}};
    for (const QString &key : FILTER_KINDS) switch_widget->addItem(key, labels.value(key));
    switch_widget->setCurrentItem(kind);
    QObject::connect(switch_widget, &SegmentedWidget::currentItemChanged, dialog, [dialog, form_token](const QString &key) { dialog->on_filter_kind_changed(key, form_token); });
    auto *row = new QHBoxLayout;
    row->addWidget(switch_widget);
    row->addStretch();
    layout->addLayout(row);
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
    auto *radio_normal = new OptionCard(ManageDialog::tr_text("General"), ManageDialog::tr_text("Paste text or files"));
    auto *radio_file = new OptionCard(ManageDialog::tr_text("Quick Launch"), ManageDialog::tr_text("Open files or folders"));
    auto *radio_hidden = new OptionCard(ManageDialog::tr_text("Hidden"), ManageDialog::tr_text("Not shown on the panel"));
    auto *radio_filter = new OptionCard(ManageDialog::tr_text("Filter"), ManageDialog::tr_text("Show one type"));
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
    radio_filter->setEnabled(can_be_filter);

    const auto kind = filter_content_type(group_type);
    if (kind) radio_filter->setChecked(true);
    else if (group_type == GroupType::HIDDEN) radio_hidden->setChecked(true);
    else if (group_type == GroupType::FILE) radio_file->setChecked(true);
    else radio_normal->setChecked(true);

    // Two rows of two, so the descriptions fit in a narrow window.
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(dialog_scaled(10));
    grid->setVerticalSpacing(dialog_scaled(10));
    for (int position = 0; position < cards.size(); ++position) grid->addWidget(cards[position], position / 2, position % 2);
    dialog->detail_layout->addLayout(grid);

    if (!can_be_filter) dialog->detail_layout->addWidget(field_hint(ManageDialog::tr_text("This group has content, so it can't become a filter group")));

    build_filter_kind_section(dialog, kind.value_or(FILTER_KINDS.first()), form_token);
}
}

void build_new_group_form(ManageDialog *dialog) {
    build_name_section(dialog, "");
    build_group_type_section(dialog, static_cast<int>(GroupType::NORMAL), true);
    dialog->detail_layout->addWidget(field_label(ManageDialog::tr_text("Icon")));
    dialog->create_emoji_picker();
}

void build_edit_group_form(ManageDialog *dialog, const Group &group) {
    build_name_section(dialog, group.name);
    // A filter group stores no content, so a group that still has content cannot become one (the Rust core refuses too).
    const bool has_items = !dialog->manager->get_by_group(group.id, 0, 1).isEmpty();
    build_group_type_section(dialog, group.group_type, group.is_filter() || !has_items);
    dialog->detail_layout->addWidget(field_label(ManageDialog::tr_text("Icon")));
    dialog->create_emoji_picker(get_group_display_icon(group.icon, group.group_type));
}
}
