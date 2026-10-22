#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "settings/tool_settings.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/settings_ui/components.h"
#include "ui/settings_ui/dialog.h"
#include "ui/settings_ui/pages.h"

// The screenshot page's "Quick Actions" group: how capturing and annotating behave, and switches
// that skip a confirmation or result window.
// The result windows' check boxes write the same keys: once checked there the window stops
// appearing and only this page turns it back, so each description says what happens while it is on.
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
    dialog->behavior_controls.emplace_back("capture_fullscreen_crosshair",
                                           add(FluentIcon::Layout, "Fullscreen Crosshair", "Show horizontal and vertical guide lines across the screen while capturing.",
                                               config->get_capture_fullscreen_crosshair()));
    dialog->behavior_controls.emplace_back("capture_include_cursor",
                                           add(FluentIcon::Camera, "Include Mouse Pointer", "Draw the mouse pointer into the screenshot where it was when capturing started.",
                                               config->get_capture_include_cursor()));
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
    dialog->set_attr("ocr_copy_directly_toggle", add(FluentIcon::Document, "Copy Recognized Text Directly",
                                                     "Copy the recognized text straight to the clipboard instead of opening the result window.",
                                                     config->get_ocr_copy_directly_enabled()));
    dialog->set_attr("barcode_copy_single_toggle", add(FluentIcon::Search, "Copy a Single Code Directly",
                                                       "When only one QR code or barcode is found, copy its content instead of opening the result window. Multiple codes still open it.",
                                                       config->get_barcode_copy_single_enabled()));
    layout->addWidget(grp_capture);
}
}
