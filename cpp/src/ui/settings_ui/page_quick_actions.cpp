#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "settings/tool_settings.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/settings_ui/components.h"
#include "ui/settings_ui/dialog.h"
#include "ui/settings_ui/pages.h"

// The screenshot page's "Quick Actions" group: how annotating behaves.
namespace jietuba {
void add_quick_actions_group(SettingsDialog *dialog, QWidget *view, QVBoxLayout *layout) {
    auto *config = dialog->config_manager;
    auto *grp_capture = new SettingsCardGroup(dialog->tr_text("Quick Actions"), view, "capture");
    const auto add = [&](FluentIcon icon, const char *title, const char *content, bool checked) {
        auto *card = new SwitchSettingCard(icon, dialog->tr_text(title), dialog->tr_text(content), grp_capture);
        card->setChecked(checked);
        grp_capture->addSettingCard(card);
        return card;
    };
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
