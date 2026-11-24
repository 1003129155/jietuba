#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "settings/tool_settings.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/settings_ui/components.h"
#include "ui/settings_ui/dialog.h"
#include "ui/settings_ui/pages.h"
#include "ui/settings_ui/search.h"
#include <QCoreApplication>

// The screenshot page's "Annotation" group: how annotating behaves.
namespace jietuba {
namespace {
// One switch row per tool under a folded card, so the group stays one row tall; the header names
// what is off, since the rows are out of sight.
void add_auto_select_card(SettingsDialog *dialog, SettingsCardGroup *group) {
    auto *header = new SettingCard(FluentIcon::Edit, dialog->tr_text("Select After Drawing"),
                                   dialog->tr_text("Select a new annotation right away, so the wheel or its handles adjust it. "
                                                   "For tools turned off, the wheel sizes the next annotation instead."),
                                   group);
    auto *summary = new QLabel(header);
    apply_theme_text_style(summary, 14, false, {}, true);
    dialog->set_attr("auto_select_summary", summary);
    const int tool_count = static_cast<int>(std::size(AUTO_SELECT_TOOLS));
    const auto off_count = [](int count) { return QCoreApplication::translate("SettingsDialog", "%n Off", nullptr, count); };
    // As wide as the longest summary, so the control column is sized for it before it shows.
    QStringList summaries{dialog->tr_text("All On"), dialog->tr_text("All Off")};
    for (const auto &tool : AUTO_SELECT_TOOLS) summaries << dialog->tr_text("Off: %1").arg(dialog->tr_text(tool.label));
    for (int count = 2; count < tool_count; ++count) summaries << off_count(count);
    summary->ensurePolished();
    int widest = 0;
    for (const auto &text : summaries) widest = std::max(widest, summary->fontMetrics().horizontalAdvance(text));
    summary->setMinimumWidth(widest + 1);
    summary->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    header->addControl(summary, Qt::AlignRight | Qt::AlignVCenter);
    group->addSettingCard(header);

    QList<QPair<QString, SubSwitchSettingCard *>> rows;
    for (const auto &tool : AUTO_SELECT_TOOLS) {
        const QString key = auto_select_setting(tool.tool_id);
        const QString label = dialog->tr_text(tool.label);
        auto *card = new SubSwitchSettingCard(label, {}, group);
        card->setChecked(dialog->config_manager->get_app_setting(key).toBool());
        dialog->behavior_controls.emplace_back(key, card);
        dialog->set_attr(key + "_toggle", card);
        group->addSettingCard(card);
        rows.append({label, card});
    }
    const auto refresh = [dialog, summary, rows, off_count] {
        QStringList off;
        for (const auto &[label, card] : rows)
            if (!card->isChecked()) off.append(label);
        if (off.isEmpty()) summary->setText(dialog->tr_text("All On"));
        else if (off.size() == rows.size()) summary->setText(dialog->tr_text("All Off"));
        else if (off.size() == 1) summary->setText(dialog->tr_text("Off: %1").arg(off.first()));
        else summary->setText(off_count(static_cast<int>(off.size())));
    };
    for (const auto &[label, card] : rows) QObject::connect(card, &SwitchSettingCard::checkedChanged, summary, refresh);
    refresh();
    if (auto *expander = group->expander(header)) {
        expander->setExpanded(false);
        mark_search_revealable(expander->body(), [expander] { expander->setExpanded(true); });
    }
}
}

void add_quick_actions_group(SettingsDialog *dialog, QWidget *view, QVBoxLayout *layout) {
    auto *config = dialog->config_manager;
    auto *grp_capture = new SettingsCardGroup(dialog->tr_text("Annotation"), view, "capture");
    const auto add = [&](FluentIcon icon, const char *title, const char *content, bool checked) {
        auto *card = new SwitchSettingCard(icon, dialog->tr_text(title), dialog->tr_text(content), grp_capture);
        card->setChecked(checked);
        grp_capture->addSettingCard(card);
        return card;
    };
    add_auto_select_card(dialog, grp_capture);
    dialog->set_attr("cross_tool_selection_toggle", add(FluentIcon::Edit, "Enable Ctrl Cross-Tool Selection",
                                                        "Hold Ctrl and click any editable annotation to adjust it without switching tools.",
                                                        config->get_cross_tool_selection_enabled()));
    auto *shift_line_card = new SettingCard(FluentIcon::Edit, dialog->tr_text("Straight Lines with Shift"),
                                            dialog->tr_text("Hold Shift while drawing with the pen or highlighter to draw a straight line from that point. "
                                                            "Release Shift to keep drawing freely."),
                                            grp_capture);
    auto *shift_line_combo = new ComboBox(shift_line_card);
    // The labels follow SHIFT_LINE_ANGLES.
    const char *angle_labels[] = {"Horizontal or Vertical", "Every 45°", "Every 30°", "Every 15°", "Any Angle"};
    for (int i = 0; i < SHIFT_LINE_ANGLES.size(); ++i) shift_line_combo->addItem(dialog->tr_text(angle_labels[i]), SHIFT_LINE_ANGLES[i]);
    shift_line_combo->setCurrentIndex(SHIFT_LINE_ANGLES.indexOf(config->get_shift_line_angle()));
    dialog->set_attr("shift_line_angle_combo", shift_line_combo);
    shift_line_card->addControl(shift_line_combo);
    grp_capture->addSettingCard(shift_line_card);
    dialog->set_attr("text_always_on_top_toggle", add(FluentIcon::Font, "Keep Text Annotations on Top", "Keep text above other annotations, including ones drawn later.",
                                                      config->get_text_always_on_top_enabled()));
    layout->addWidget(grp_capture);
}
}
