#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "settings/tool_settings.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/labels.h"
#include "ui/settings_ui/components.h"
#include "ui/settings_ui/dialog.h"
#include "ui/settings_ui/pages.h"

namespace jietuba {
QWidget *create_developer_page(SettingsDialog *dialog) {
    auto *config = dialog->config_manager;
    auto *scroll = page_scroll_area(dialog);
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet("QScrollArea { border: none; background: transparent; }");
    auto *view = new QWidget;
    set_own_style(view, "background: transparent;");
    auto *layout = new QVBoxLayout(view);
    layout->setContentsMargins(0, 0, dialog_scaled(10), 0);
    layout->setSpacing(dialog_scaled(14));

    // ════ Long screenshot ════
    auto *grp_stitch = new SettingsCardGroup(dialog->tr_text("Long Screenshot"), view);
    auto *engine_card = new SettingCard(FluentIcon::Setting, dialog->tr_text("Stitching Engine"), {}, grp_stitch);
    auto *engine_combo = new ComboBox(engine_card);
    engine_combo->addItem("Rust Hash", "hash_rust");
    engine_combo->setCurrentIndex(0);
    dialog->set_attr("engine_combo", engine_combo);
    engine_card->addControl(engine_combo);
    grp_stitch->addSettingCard(engine_card);
    // Top pixels skipped while matching later captures.
    auto *ignore_top_card = new SettingCard(FluentIcon::Filter, dialog->tr_text("Ignore Top Pixels"),
                                            dialog->tr_text("Skip this many top pixels on subsequent captures during matching"), grp_stitch);
    auto *ignore_spin = new SpinBox(ignore_top_card);
    ignore_spin->setRange(0, 300);
    ignore_spin->setSingleStep(1);
    ignore_spin->setValue(static_cast<int>(config->get_long_stitch_ignore_top_pixels()));
    dialog->set_attr("ignore_top_pixels_spinbox", ignore_spin);
    ignore_top_card->addControl(ignore_spin);
    grp_stitch->addSettingCard(ignore_top_card);
    layout->addWidget(grp_stitch);

    // ════ Tools ════
    auto *grp_tools = new SettingsCardGroup(dialog->tr_text("Tools"), view);
    auto *wizard_card = new SettingCard(FluentIcon::Setting, dialog->tr_text("Setup Wizard"), dialog->tr_text("Re-run the initial setup guide."), grp_tools);
    auto *btn_wizard = new PrimaryPushButton(dialog->tr_text("Open Wizard"), wizard_card);
    QObject::connect(btn_wizard, &QPushButton::clicked, dialog, &SettingsDialog::open_welcome_wizard);
    wizard_card->addControl(btn_wizard);
    grp_tools->addSettingCard(wizard_card);
    layout->addWidget(grp_tools);

    // ════ Startup preload ════
    auto *grp_preload = new SettingsCardGroup(dialog->tr_text("Startup Preload"), view);
    auto *preload_desc = new CaptionLabel(
        dialog->tr_text("Control which modules are preloaded at startup. Disable to speed up launch. Changes take effect after restart."), view);
    preload_desc->setWordWrap(true);
    preload_desc->setStyleSheet(QString("padding: 0 0 %1px 0;").arg(dialog_scaled(4)));
    layout->addWidget(preload_desc);
    struct PreloadItem {
        const char *attr;
        FluentIcon icon;
        const char *title, *desc, *key;
    };
    const PreloadItem preload_items[] = {
        {"preload_screenshot_toggle", FluentIcon::Camera, "Preload Screenshot Modules", "Pre-load the capture engine, canvas and tools", "preload_screenshot"},
        {"preload_toolbar_toggle", FluentIcon::Layout, "Preload Toolbar", "Pre-create toolbar widgets to avoid first-capture lag", "preload_toolbar"},
        {"preload_ocr_toggle", FluentIcon::Search, "Preload OCR Engine", "Pre-load OCR model in background thread", "preload_ocr"},
        {"preload_settings_toggle", FluentIcon::Setting, "Preload Settings Window", "Pre-create the settings dialog on startup", "preload_settings"},
        {"preload_clipboard_toggle", FluentIcon::Paste, "Preload Clipboard Manager", "Initialize clipboard monitoring on startup", "preload_clipboard"},
    };
    for (const auto &item : preload_items) {
        auto *card = new SwitchSettingCard(item.icon, dialog->tr_text(item.title), dialog->tr_text(item.desc), grp_preload);
        card->setChecked(config->get_app_setting(item.key).toBool());
        dialog->set_attr(item.attr, card);
        grp_preload->addSettingCard(card);
    }
    layout->addWidget(grp_preload);

    // ════ Selection info panel ════
    auto *grp_panel = new SettingsCardGroup(dialog->tr_text("Selection Info Panel"), view);
    auto *hide_card = new SwitchSettingCard(FluentIcon::Hide, dialog->tr_text("Hide Panel on Drag"), dialog->tr_text("Hide the info panel while dragging the selection area"), grp_panel);
    hide_card->setChecked(config->get_app_setting("screenshot_info_hide_on_drag").toBool());
    dialog->set_attr("info_hide_on_drag_toggle", hide_card);
    grp_panel->addSettingCard(hide_card);
    layout->addWidget(grp_panel);

    layout->addStretch();
    scroll->setWidget(view);
    return scroll;
}
}
