#include "clipboard/ui/theme/themes.h"
#include "core/theme.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "settings/tool_settings.h"
#include "ui/color_picker_dialog.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/labels.h"
#include "ui/settings_ui/components.h"
#include "ui/settings_ui/dialog.h"
#include "ui/settings_ui/pages.h"
#include <QMenu>
#include <QWidgetAction>

namespace jietuba {
namespace {
ComboBox *add_combo_card(SettingsDialog *dialog, SettingsCardGroup *grp, FluentIcon icon, const char *title, const char *content, const QString &attr,
                         const std::vector<std::pair<QString, QVariant>> &items, const QVariant &current) {
    auto *card = new SettingCard(icon, dialog->tr_text(title), content ? dialog->tr_text(content) : QString(), grp);
    auto *combo = new ComboBox(card);
    for (const auto &[label, value] : items) combo->addItem(label, value);
    combo->setCurrentIndex(std::max(0, combo->findData(current)));
    dialog->set_attr(attr, combo);
    card->addControl(combo);
    grp->addSettingCard(card);
    return combo;
}

// Application appearance: follow the system, or force light or dark.
void build_application_section(SettingsDialog *dialog, SettingsCardGroup *grp) {
    add_combo_card(dialog, grp, FluentIcon::Application, "Interface Theme", "Choose a display mode.", "_ui_theme_combo",
                   {{dialog->tr_text("System"), "system"}, {dialog->tr_text("Light"), "light"}, {dialog->tr_text("Dark"), "dark"}},
                   ui_theme_mode_value(get_ui_theme()->mode()));
    // Toolbar and panel scale: the operation UI only, never capture pixels or drawn content.
    std::vector<std::pair<QString, QVariant>> percents;
    for (int percent : UIScaleManager::PERCENT_OPTIONS) percents.emplace_back(QString("%1%").arg(percent), percent);
    add_combo_card(dialog, grp, FluentIcon::Layout, "Toolbar & Panel Scale", "Size of toolbars, tool panels, popups and the clipboard window.", "_ui_scale_combo",
                   percents, get_ui_scale().percent());
    // Window scale: font and size of standalone windows.
    add_combo_card(dialog, grp, FluentIcon::FontSize, "Window Scale", "Font and size of application windows.", "_dialog_scale_combo", percents,
                   get_dialog_scale().percent());
    // Read by choose_color, the one entry every colour control opens the picker through.
    add_combo_card(dialog, grp, FluentIcon::Palette, "Color Picker", "Used when choosing a custom color.", "_color_picker_style_combo",
                   {{dialog->tr_text("Modern"), "modern"}, {dialog->tr_text("Classic (system dialog)"), "system"}},
                   dialog->config_manager->get_color_picker_style());
}

// Picks a colour into one of the dialog's appearance colours and its swatch; both are opaque.
void pick_color(SettingsDialog *dialog, std::optional<QColor> SettingsDialog::*color, const QString &button_attr, const char *title) {
    auto *button = dialog->attr_widget(button_attr);
    const QColor current = dialog->*color ? *(dialog->*color) : QColor();
    if (const auto selected = choose_color(current, button, dialog->tr_text(title), ColorPickerDialog::NoOption)) {
        dialog->*color = *selected;
        update_color_btn(button, *selected);
    }
}

// Screenshot appearance: theme colour, mask colour, selection border and handles.
void build_screenshot_section(SettingsDialog *dialog, SettingsCardGroup *grp) {
    auto &theme = get_theme();

    auto *theme_card = new SettingCard(FluentIcon::Palette, dialog->tr_text("Theme Color"), {}, grp);
    auto *theme_btn = new ColorSwatchButton(theme_card);
    dialog->set_attr("_theme_color_btn", theme_btn);
    dialog->appearance_theme_color = theme.theme_color();
    update_color_btn(theme_btn, *dialog->appearance_theme_color);
    QObject::connect(theme_btn, &QPushButton::clicked, dialog,
                     [dialog] { pick_color(dialog, &SettingsDialog::appearance_theme_color, "_theme_color_btn", "Select Theme Color"); });
    theme_card->addControl(theme_btn);
    grp->addSettingCard(theme_card);

    auto *mask_card = new SettingCard(FluentIcon::Brush, dialog->tr_text("Mask Color"), {}, grp);
    auto *mask_btn = new ColorSwatchButton(mask_card);
    dialog->set_attr("_mask_color_btn", mask_btn);
    const QColor mc = theme.mask_color();
    dialog->appearance_mask_color = QColor(mc.red(), mc.green(), mc.blue());
    update_color_btn(mask_btn, *dialog->appearance_mask_color);
    QObject::connect(mask_btn, &QPushButton::clicked, dialog, [dialog] { pick_color(dialog, &SettingsDialog::appearance_mask_color, "_mask_color_btn", "Select Mask Color"); });
    mask_card->addControl(mask_btn);
    grp->addSettingCard(mask_card);

    // Border width steps come from the theme manager rather than a list of their own here.
    std::vector<std::pair<QString, QVariant>> widths;
    for (int width : ThemeManager::BORDER_WIDTH_OPTIONS) widths.emplace_back(QString("%1px").arg(width), width);
    add_combo_card(dialog, grp, FluentIcon::Edit, "Selection Border Width", "Thickness of the selection outline.", "_selection_border_combo", widths,
                   theme.selection_border_width());
    // Hidden handles are only not drawn; all eight directions still drag.
    add_combo_card(dialog, grp, FluentIcon::Alignment, "Selection Handles", "Hidden handles can still be dragged to resize.", "_selection_handle_combo",
                   {{dialog->tr_text("8 handles"), ThemeManager::HANDLES_ALL},
                    {dialog->tr_text("4 corners"), ThemeManager::HANDLES_CORNERS},
                    {dialog->tr_text("None"), ThemeManager::HANDLES_NONE}},
                   theme.selection_handle_style());
    // Diameter and ring width read as one, so only small / medium / large rather than two pixel settings.
    add_combo_card(dialog, grp, FluentIcon::Edit, "Selection Handle Size", "Size of the selection handles.", "_selection_handle_size_combo",
                   {{dialog->tr_text("Small"), ThemeManager::HANDLE_SIZE_SMALL},
                    {dialog->tr_text("Medium"), ThemeManager::HANDLE_SIZE_MEDIUM},
                    {dialog->tr_text("Large"), ThemeManager::HANDLE_SIZE_LARGE}},
                   theme.selection_handle_size());
}

// Clipboard appearance: theme, font size and opacity; saved like the rest.
void build_clipboard_section(SettingsDialog *dialog, SettingsCardGroup *grp) {
    auto *config = dialog->config_manager;

    auto *theme_card = new SettingCard(FluentIcon::Brush, dialog->tr_text("Theme"), {}, grp);
    auto *theme_btn = new ColorSwatchButton(theme_card);
    dialog->set_attr("_clip_theme_btn", theme_btn);
    dialog->clip_theme_name = config->get_clipboard_theme();
    apply_clip_theme_btn_style(theme_btn, *dialog->clip_theme_name);
    QObject::connect(theme_btn, &QPushButton::clicked, dialog, [dialog, theme_btn] {
        auto *menu = new QMenu(dialog);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->setStyleSheet(theme_menu_style() + QString(" QMenu { padding: %1px; }").arg(dialog_scaled(4)));
        auto buttons = std::make_shared<std::vector<std::pair<QString, ColorSwatchButton *>>>();
        auto update_btns = [buttons](const QString &selected) {
            for (const auto &[name, btn] : *buttons) {
                // The check is drawn on the swatch's own background, so its shade follows the swatch, not the UI theme.
                // An SVG rather than ✓, which is not in the UI fonts and falls back differently on each Windows version.
                btn->setIcon(name == selected ? tinted_icon(FluentIcon::Check, name == "dark" ? "#FFFFFF" : "#333333") : QIcon());
                btn->setSelected(name == selected);
            }
        };
        for (const auto &[tname, swatch] : clipboard::PRESET_THEME_SWATCHES()) {
            auto *action = new QWidgetAction(menu);
            auto *btn = new ColorSwatchButton(menu);
            btn->setFixedWidth(theme_btn->width());
            btn->setBaseIconSize(14);
            apply_clip_theme_btn_style(btn, tname);
            QObject::connect(btn, &QPushButton::clicked, menu, [dialog, theme_btn, menu, update_btns, name = tname] {
                dialog->clip_theme_name = name;
                apply_clip_theme_btn_style(theme_btn, name);
                update_btns(name);
                menu->close();
                // The theme is held on the dialog; no control signal schedules the save.
                dialog->schedule_save();
            });
            action->setDefaultWidget(btn);
            menu->addAction(action);
            buttons->emplace_back(tname, btn);
        }
        // Built on click, outside the page build's configure_dialog_controls pass.
        configure_dialog_controls(*menu);
        update_btns(dialog->clip_theme_name.value_or(QString()));
        menu->popup(theme_btn->mapToGlobal(theme_btn->rect().bottomLeft()));
    });
    theme_card->addControl(theme_btn);
    grp->addSettingCard(theme_card);

    std::vector<std::pair<QString, QVariant>> font_sizes;
    for (const QVariant &size : config->get_clipboard_font_size_options().toList()) font_sizes.emplace_back(QString("%1px").arg(size.toLongLong()), size);
    add_combo_card(dialog, grp, FluentIcon::FontSize, "Font Size", nullptr, "_clip_font_combo", font_sizes, config->get_clipboard_font_size());

    std::vector<std::pair<QString, QVariant>> opacities;
    for (const QVariant &percent : config->get_clipboard_window_opacity_options().toList())
        opacities.emplace_back(percent.toLongLong() == 0 ? dialog->tr_text("Opaque") : QString("%1%").arg(percent.toLongLong()), percent);
    add_combo_card(dialog, grp, FluentIcon::Transparent, "Opacity", nullptr, "_clip_opacity_combo", opacities, config->get_clipboard_window_opacity());
}
}

void apply_clip_theme_btn_style(QWidget *btn, const QString &name) {
    auto *swatch_btn = qobject_cast<ColorSwatchButton *>(btn);
    if (!swatch_btn) return;
    for (const auto &[theme_name, swatch] : clipboard::PRESET_THEME_SWATCHES()) {
        if (theme_name != name) continue;
        const auto &[accent, bg] = swatch;
        swatch_btn->setFill(QString("qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 %1, stop:0.5 %1, stop:0.5 %2, stop:1 %2)").arg(bg, accent));
        return;
    }
}

void update_color_btn(QWidget *btn, const QColor &color) {
    if (auto *swatch = qobject_cast<ColorSwatchButton *>(btn)) swatch->setFill(QString("rgb(%1, %2, %3)").arg(color.red()).arg(color.green()).arg(color.blue()));
}

QWidget *create_appearance_page(SettingsDialog *dialog) {
    auto *scroll = page_scroll_area(dialog);
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet("QScrollArea { border: none; background: transparent; }");
    auto *view = new QWidget;
    set_own_style(view, "background: transparent;");
    auto *layout = new QVBoxLayout(view);
    layout->setContentsMargins(0, 0, dialog_scaled(10), 0);
    layout->setSpacing(dialog_scaled(14));

    auto *grp_app = new SettingsCardGroup(dialog->tr_text("Application"), view);
    build_application_section(dialog, grp_app);
    layout->addWidget(grp_app);

    auto *grp_ss = new SettingsCardGroup(dialog->tr_text("Screenshot"), view, "capture");
    build_screenshot_section(dialog, grp_ss);
    layout->addWidget(grp_ss);

    auto *grp_clip = new SettingsCardGroup(dialog->tr_text("Clipboard"), view, "clipboard");
    build_clipboard_section(dialog, grp_clip);
    layout->addWidget(grp_clip);

    auto *hint = new CaptionLabel(dialog->tr_text("💡 Hint: Color changes take effect on the next screenshot."), view);
    hint->setStyleSheet(QString("padding: %1px;").arg(dialog_scaled(5)));
    layout->addWidget(hint);
    layout->addStretch();
    scroll->setWidget(view);
    return scroll;
}
}
