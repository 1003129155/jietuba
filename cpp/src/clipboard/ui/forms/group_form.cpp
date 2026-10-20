#include "clipboard/ui/forms/group_form.h"
#include "clipboard/core/manager.h"
#include "clipboard/services/group_service.h"
#include "clipboard/ui/dialogs/manage_dialog.h"
#include "clipboard/ui/forms/form_widgets.h"
#include "clipboard/ui/forms/group_icon_picker.h"
#include "clipboard/ui/group_kind_style.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/inputs.h"
#include <QGridLayout>
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

// A source app's name in the list: the exe name without ".exe", this program's builds as one entry.
QString source_app_label(const QString &app) {
    if (app.compare(OWN_SOURCE_APP, Qt::CaseInsensitive) == 0) return ManageDialog::tr_text("Jietuba (this app)");
    return app.endsWith(".exe", Qt::CaseInsensitive) ? app.chopped(4) : app;
}

// What a filter group shows: a content type and a source app, either left at "any";
// only visible while "Filter" is chosen. Each combo item's data is the rule value, empty for any.
void build_filter_rule_section(ManageDialog *dialog, const FilterRule &rule, int form_token) {
    auto *box = new QWidget;
    auto *layout = new QGridLayout(box);
    layout->setContentsMargins(0, dialog_scaled(4), 0, 0);
    layout->setHorizontalSpacing(dialog_scaled(12));
    layout->setVerticalSpacing(dialog_scaled(8));

    auto *kind_combo = new ComboBox;
    const std::pair<QString, const char *> kinds[] = {{QString(), "All Types"},     {"text", "Text"},  {SUB_KIND_URL, "Link"}, {SUB_KIND_COLOR, "Color"},
                                                      {SUB_KIND_PATH, "Path"}, {"image", "Image"}, {"file", "File"}};
    for (const auto &[kind, label] : kinds) kind_combo->addItem(ManageDialog::tr_text(label), kind);
    kind_combo->setCurrentIndex(std::max(0, kind_combo->findData(rule.kind.value_or(QString()))));

    auto *app_combo = new ComboBox;
    app_combo->addItem(ManageDialog::tr_text("Any App"), QString());
    bool listed = !rule.app;
    for (const auto &[app, count] : dialog->manager->source_apps()) {
        app_combo->addItem(QString("%1 (%2)").arg(source_app_label(app)).arg(count), app);
        listed = listed || app.compare(*rule.app, Qt::CaseInsensitive) == 0;
    }
    // A rule's app with nothing left in the history still shows, rather than turning into "any".
    if (!listed) app_combo->addItem(source_app_label(*rule.app), *rule.app);
    if (rule.app)
        for (int index = 1; index < app_combo->count(); ++index)
            if (app_combo->itemData(index).toString().compare(*rule.app, Qt::CaseInsensitive) == 0) app_combo->setCurrentIndex(index);

    layout->addWidget(field_label(ManageDialog::tr_text("Content Type"), false), 0, 0);
    layout->addWidget(kind_combo, 0, 1);
    layout->addWidget(field_label(ManageDialog::tr_text("Source App"), false), 1, 0);
    layout->addWidget(app_combo, 1, 1);
    layout->setColumnStretch(2, 1);
    for (auto *combo : {kind_combo, app_combo})
        QObject::connect(combo, &QComboBox::currentIndexChanged, dialog, [dialog, form_token] { dialog->on_filter_kind_changed({}, form_token); });
    dialog->filter_kind_box = box;
    dialog->filter_kind_combo = kind_combo;
    dialog->filter_app_combo = app_combo;
    dialog->detail_layout->addWidget(box);
    // Show only after it has a parent: a parentless widget made visible flashes as a window of its own.
    box->setVisible(dialog->radio_filter->isChecked());
}

void build_group_type_section(ManageDialog *dialog, int group_type, bool can_be_filter, const std::optional<FilterRule> &filter_rule) {
    dialog->detail_layout->addWidget(field_label(ManageDialog::tr_text("Group Type")));

    auto *button_group = new QButtonGroup(dialog);
    dialog->group_type_btn_group = button_group;
    auto *radio_normal = new OptionCard(ManageDialog::tr_text("General"), ManageDialog::tr_text("Paste text or files"), KIND_NORMAL());
    auto *radio_file = new OptionCard(ManageDialog::tr_text("Quick Launch"), ManageDialog::tr_text("Open files, folders or links"), KIND_FILE());
    auto *radio_hidden = new OptionCard(ManageDialog::tr_text("Hidden"), ManageDialog::tr_text("Not shown on the panel"), KIND_HIDDEN());
    auto *radio_filter = new OptionCard(ManageDialog::tr_text("Filter"), ManageDialog::tr_text("By type or source"), KIND_FILTER());
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

    if (group_type == GroupType::FILTER) radio_filter->setChecked(true);
    else if (group_type == GroupType::HIDDEN) radio_hidden->setChecked(true);
    else if (group_type == GroupType::FILE) radio_file->setChecked(true);
    else radio_normal->setChecked(true);

    // One row when the width allows, two per row otherwise, so the descriptions stay whole.
    dialog->detail_layout->addWidget(new CardGrid(cards));

    // A new filter group starts on all text.
    build_filter_rule_section(dialog, filter_rule.value_or(FilterRule::of_kind("text")), form_token);
}
}

void build_new_group_form(ManageDialog *dialog) {
    const QString icon = get_group_display_icon(std::nullopt, static_cast<int>(GroupType::NORMAL));
    build_name_section(dialog, "", icon, static_cast<int>(GroupType::NORMAL));
    build_group_type_section(dialog, static_cast<int>(GroupType::NORMAL), true, std::nullopt);
    dialog->detail_layout->addWidget(field_label(ManageDialog::tr_text("Icon")));
    dialog->create_emoji_picker(icon);
}

void build_edit_group_form(ManageDialog *dialog, const Group &group) {
    const QString current_icon = get_group_display_icon(group.icon, group.group_type, group.filter_rule);
    build_name_section(dialog, group.name, current_icon, group.group_type);
    // A filter group stores no content, so a group that still has content cannot become one (the Rust core refuses too).
    const bool has_items = !dialog->manager->get_by_group(group.id, 0, 1).isEmpty();
    build_group_type_section(dialog, group.group_type, group.is_filter() || !has_items, group.filter_rule);
    dialog->detail_layout->addWidget(field_label(ManageDialog::tr_text("Icon")));
    dialog->create_emoji_picker(current_icon);
}
}
