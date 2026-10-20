#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "ocr/ocr_manager.h"
#include "ocr/ppocr_models.h"
#include "settings/color_formats.h"
#include "settings/tool_settings.h"
#include "ui/dialogs.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/labels.h"
#include "ui/settings_ui/color_format_dialog.h"
#include "ui/settings_ui/components.h"
#include "ui/settings_ui/dialog.h"
#include "ui/settings_ui/pages.h"
#include "ui/settings_ui/ppocr_model_dialog.h"
#include <QHash>
#include <QJsonArray>
#include <QStandardItemModel>
#include <algorithm>

namespace jietuba {
namespace {
// Opens the colour format manager; the result stays on the dialog until Apply.
void manage_color_formats(SettingsDialog *dialog) {
    ColorFormatDialog editor(*dialog->magnifier_color_formats, dialog);
    if (exec_dialog(&editor)) dialog->magnifier_color_formats = editor.entries();
}

// The OCR combo's item for a PP-OCR model is this prefix and the model id.
const QString PPOCR_PREFIX = "ppocr_rust:";
// The last item, which opens the model manager.
const QString MANAGE_MODELS = "manage_models";

// Deleting the model in use sets the engine to Auto at once rather than at Apply, so no setting
// is left pointing at a deleted model; a model only picked in the combo puts the combo back on Auto.
void on_model_removed(SettingsDialog *dialog, PpocrModelDialog *window, const QString &model_id) {
    auto *config = dialog->config_manager;
    auto *combo = dialog->attr<QComboBox>("ocr_engine_combo");
    const bool in_use = config->get_ocr_engine() == OCRManager::ENGINE_PP_RUST && config->get_ppocr_model() == model_id;
    if (in_use) {
        config->set_ocr_engine(OCRManager::ENGINE_AUTO);
        set_ocr_engine(OCRManager::ENGINE_AUTO);
        dialog->update_snapshot("ocr_engine_combo", OCRManager::ENGINE_AUTO);
    }
    if (in_use || combo->property("selected").toString() == PPOCR_PREFIX + model_id) {
        combo->setProperty("selected", OCRManager::ENGINE_AUTO);
        window->show_status(dialog->tr_text("The deleted model was selected for OCR, so OCR now uses Auto."));
    }
}

// Opens the model manager; file changes take effect at once, and the OCR list is refilled
// afterwards with the item that was selected before.
void open_model_manager(SettingsDialog *dialog) {
    auto *combo = dialog->attr<QComboBox>("ocr_engine_combo");
    PpocrModelDialog window(dialog);
    QObject::connect(&window, &PpocrModelDialog::model_removed, dialog, [dialog, &window](const QString &model_id) { on_model_removed(dialog, &window, model_id); });
    exec_dialog(&window);
    const auto [engine, model_id] = ocr_choice(combo->property("selected").toString());
    fill_ocr_engines(dialog, engine, model_id.value_or(dialog->config_manager->get_ppocr_model()));
}

// Picking "Download more models..." opens the manager; any other item becomes the selection.
void on_ocr_engine_activated(SettingsDialog *dialog) {
    auto *combo = dialog->attr<QComboBox>("ocr_engine_combo");
    const QString data = combo->currentData().toString();
    if (data == MANAGE_MODELS) open_model_manager(dialog);
    else combo->setProperty("selected", data);
}

SwitchSettingCard *switch_card(SettingsDialog *dialog, SettingsCardGroup *group, FluentIcon icon, const char *title, const char *content, bool checked, const QString &attr) {
    auto *card = new SwitchSettingCard(icon, dialog->tr_text(title), dialog->tr_text(content), group);
    card->setChecked(checked);
    dialog->set_attr(attr, card);
    group->addSettingCard(card);
    return card;
}
}

QWidget *create_capture_page(SettingsDialog *dialog) {
    auto *config = dialog->config_manager;
    auto *scroll = page_scroll_area(dialog);
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet("QScrollArea { border: none; background: transparent; }");
    auto *view = new QWidget;
    set_own_style(view, "background: transparent;");
    auto *layout = new QVBoxLayout(view);
    layout->setContentsMargins(0, 0, dialog_scaled(10), 0);
    layout->setSpacing(dialog_scaled(20));

    // ── Capture engine ──
    auto *grp_engine = new SettingsCardGroup(dialog->tr_text("Screenshot"), view, "capture");
    auto *engine_card = new SettingCard(FluentIcon::Camera, dialog->tr_text("Capture Engine"),
                                        dialog->tr_text("Auto uses the HDR engine when a display has HDR turned on."), grp_engine);
    auto *capture_engine_combo = new ComboBox(engine_card);
    // HDR is the engine's name and is not translated.
    for (const auto &[label, engine] : {std::make_pair(dialog->tr_text("Auto (Recommended)"), "auto"), std::make_pair(dialog->tr_text("Standard"), "mss"),
                                        std::make_pair(QString("HDR"), "hdr")})
        capture_engine_combo->addItem(label, QString(engine));
    capture_engine_combo->setCurrentIndex(CAPTURE_ENGINE_CHOICES.indexOf(config->get_capture_engine()));
    dialog->set_attr("capture_engine_combo", capture_engine_combo);
    engine_card->addControl(capture_engine_combo);
    grp_engine->addSettingCard(engine_card);
    layout->addWidget(grp_engine);

    // ── Smart selection ──
    auto *grp_smart = new SettingsCardGroup(dialog->tr_text("Smart Selection"), view, "capture");
    // Three levels in one combo rather than a master switch plus a drill-down switch: the
    // configuration has only off / window / element, and two switches would add the invalid
    // "master off, drill-down on" combination.
    auto *mode_card = new SettingCard(FluentIcon::Camera, dialog->tr_text("Detection Mode"),
                                      dialog->tr_text("Snap the selection to the window or control under the cursor. "
                                                      "Control detection reads the accessibility tree and takes longer on large windows."),
                                      grp_smart);
    auto *smart_mode_combo = new ComboBox(mode_card);
    for (const auto &[label, mode] : {std::make_pair("Off", "off"), std::make_pair("Window", "window"), std::make_pair("Control", "element")})
        smart_mode_combo->addItem(dialog->tr_text(label), QString(mode));
    smart_mode_combo->setCurrentIndex(SMART_SELECTION_MODES.indexOf(config->get_smart_selection_mode()));
    dialog->set_attr("smart_mode_combo", smart_mode_combo);
    mode_card->addControl(smart_mode_combo);
    grp_smart->addSettingCard(mode_card);
    switch_card(dialog, grp_smart, FluentIcon::Sync, "Selection Switch Animation", "Slide the selection when it moves from one window or control to another.",
                config->get_smart_selection_animation(), "smart_animation_toggle");
    layout->addWidget(grp_smart);

    // ── Saving ──
    auto *grp_save = new SettingsCardGroup(dialog->tr_text("Save Settings"), view, "capture");
    // Auto-save has one switch per moment under this title card; deliver_screenshot decides.
    grp_save->addSettingCard(new SettingCard(FluentIcon::Save, dialog->tr_text("Auto-save"),
                                             dialog->tr_text("Screenshots are saved to the save folder automatically when the actions below finish."), grp_save));
    auto *save_card = new SubSwitchSettingCard(dialog->tr_text("When a Screenshot Is Done"),
                                               dialog->tr_text("Saves when a screenshot is finished, including confirming a screenshot, long screenshots, "
                                                               "global mouse shortcut screenshots, and more."),
                                               grp_save);
    save_card->setChecked(config->get_screenshot_save_enabled());
    dialog->set_attr("save_toggle", save_card);
    grp_save->addSettingCard(save_card);
    auto *pin_save_card = new SubSwitchSettingCard(dialog->tr_text("When a Pinned Image Is Created"),
                                                   dialog->tr_text("Saves when a selected region becomes a pinned image, including regular pins, "
                                                                   "global mouse shortcut pins, long screenshot pins, and more."),
                                                   grp_save);
    pin_save_card->setChecked(config->get_screenshot_pin_save_enabled());
    dialog->set_attr("screenshot_pin_save_toggle", pin_save_card);
    grp_save->addSettingCard(pin_save_card);
    // Writes the file path to the clipboard (CF_HDROP). Only an auto-saved capture has a path,
    // decided in deliver_screenshot; the switches are stored independently, with no UI coupling.
    switch_card(dialog, grp_save, FluentIcon::CommandPrompt, "Write File Path to Clipboard",
                "Lets tools that only recognize a file path (e.g. some terminal apps) paste the screenshot too. Only takes effect when the screenshot is auto-saved.",
                config->get_clipboard_file_reference_enabled(), "clipboard_file_reference_toggle");
    switch_card(dialog, grp_save, FluentIcon::History, "Also Save the Original",
                "When a screenshot with annotations (including mosaic) or a watermark is saved to the save folder, "
                "a copy without them is saved too, with _original at the end of its file name.",
                config->get_screenshot_save_original_enabled(), "screenshot_save_original_toggle");
    // The save folder: the path is the card description, the buttons go in the control column.
    auto *path_card = new SettingCard(FluentIcon::Folder, dialog->tr_text("Save Folder"), config->get_screenshot_save_path(), grp_save);
    dialog->set_attr("save_path_lbl", path_card->contentLabel);
    auto *btn_change = new PushButton(dialog->tr_text("Change"), path_card);
    QObject::connect(btn_change, &QPushButton::clicked, dialog, &SettingsDialog::change_save_dir);
    auto *btn_open = new PushButton(dialog->tr_text("Open"), path_card);
    QObject::connect(btn_open, &QPushButton::clicked, dialog, &SettingsDialog::open_save_dir);
    path_card->addControl(btn_change);
    path_card->addControl(btn_open);
    grp_save->addSettingCard(path_card);
    // Save format.
    auto *fmt_card = new SettingCard(FluentIcon::Document, dialog->tr_text("Save Format"), dialog->tr_text("File format for auto-saved screenshots."), grp_save);
    auto *format_combo = new ComboBox(fmt_card);
    for (const auto &[label, value] : {std::make_pair("PNG", "PNG"), std::make_pair("JPG", "JPG"), std::make_pair("BMP", "BMP"), std::make_pair("WebP", "WEBP"), std::make_pair("PDF", "PDF")})
        format_combo->addItem(label, QString(value));
    format_combo->setCurrentIndex(std::max<int>(0, QStringList{"PNG", "JPG", "BMP", "WEBP", "PDF"}.indexOf(config->get_screenshot_format().toUpper())));
    dialog->set_attr("screenshot_format_combo", format_combo);
    fmt_card->addControl(format_combo);
    grp_save->addSettingCard(fmt_card);
    // The window behind Save in the capture toolbar, the pin menu and the clipboard.
    auto *style_card = new SettingCard(FluentIcon::Save, dialog->tr_text("Save Dialog"),
                                       dialog->tr_text("Used when saving a screenshot, a pin or a clipboard image."), grp_save);
    auto *style_combo = new ComboBox(style_card);
    style_combo->addItem(dialog->tr_text("Modern (with preview)"), QString("modern"));
    style_combo->addItem(dialog->tr_text("Classic (system dialog)"), QString("classic"));
    style_combo->setCurrentIndex(std::max(0, style_combo->findData(config->get_app_setting("save_dialog_style").toString())));
    dialog->set_attr("save_dialog_style_combo", style_combo);
    style_card->addControl(style_combo);
    grp_save->addSettingCard(style_card);
    layout->addWidget(grp_save);

    // ── Watermark ── (text and style are edited in the capture toolbar's watermark panel)
    auto *grp_watermark = new SettingsCardGroup(dialog->tr_text("Watermark"), view, "capture");
    switch_card(dialog, grp_watermark, FluentIcon::Sparkle, "Add the Last Watermark Automatically",
                "If the last screenshot ended with a watermark, the next one gets the same watermark "
                "with the time updated; otherwise none is added. Screenshots taken with mouse shortcuts follow the same rule.",
                config->get_screenshot_watermark_auto(), "watermark_auto_toggle");
    layout->addWidget(grp_watermark);

    // ── Magnifier ──
    auto *grp_magnifier = new SettingsCardGroup(dialog->tr_text("Magnifier"), view, "capture");
    switch_card(dialog, grp_magnifier, FluentIcon::Search, "Show Magnifier", "Follows the cursor while selecting, and picks the color under it.",
                config->get_app_setting("magnifier_enabled").toBool(), "magnifier_enabled_toggle");
    switch_card(dialog, grp_magnifier, FluentIcon::Layout, "Pixel Grid",
                "Outline each pixel in the magnified view. Hidden at low zoom, where the lines would cover the pixels.",
                config->get_app_setting("magnifier_grid").toBool(), "magnifier_grid_toggle");
    switch_card(dialog, grp_magnifier, FluentIcon::Info, "Shortcut Hint",
                "Show the switch-format and copy-color shortcuts under the readings. Off makes the magnifier shorter.",
                config->get_app_setting("magnifier_hint").toBool(), "magnifier_hint_toggle");
    // Formats can be checked several at a time and added, which a combo cannot hold, so they
    // get their own window; the result stays on the dialog until Apply.
    auto *formats_card = new SettingCard(FluentIcon::Palette, dialog->tr_text("Color Formats"),
                                         dialog->tr_text("Which formats Shift cycles through on the magnifier, for C to copy."), grp_magnifier);
    dialog->magnifier_color_formats = color_formats::load(config);
    auto *manage_btn = new PushButton(dialog->tr_text("Manage Color Formats"), formats_card);
    QObject::connect(manage_btn, &QPushButton::clicked, dialog, [dialog] { manage_color_formats(dialog); });
    dialog->set_attr("magnifier_color_formats_btn", manage_btn);
    formats_card->addControl(manage_btn);
    grp_magnifier->addSettingCard(formats_card);
    layout->addWidget(grp_magnifier);

    // ── OCR ──
    QStringList available_engines;
    try {
        available_engines = get_available_engines();
    } catch (const std::exception &) {
        available_engines.clear();
    }
    const bool ocr_available = !available_engines.isEmpty();
    auto *grp_ocr = new SettingsCardGroup(dialog->tr_text("OCR Settings"), view, "capture");
    auto *ocr_engine_card = new SettingCard(FluentIcon::Font, dialog->tr_text("OCR Engine"), dialog->tr_text("Switch OCR engines here."), grp_ocr);
    auto *ocr_engine_combo = new ComboBox(ocr_engine_card);
    ocr_engine_combo->setProperty("has_oneocr", available_engines.contains(OCRManager::ENGINE_ONEOCR));
    dialog->set_attr("ocr_engine_combo", ocr_engine_combo);
    fill_ocr_engines(dialog, config->get_ocr_engine(), config->get_ppocr_model());
    QObject::connect(model_downloads(), &ModelDownloads::models_changed, dialog, [dialog, ocr_engine_combo] {
        const auto [engine, model_id] = ocr_choice(ocr_engine_combo->property("selected").toString());
        fill_ocr_engines(dialog, engine, model_id.value_or(dialog->config_manager->get_ppocr_model()));
    });
    QObject::connect(ocr_engine_combo, &QComboBox::activated, dialog, [dialog] { on_ocr_engine_activated(dialog); });
    ocr_engine_card->addControl(ocr_engine_combo);
    grp_ocr->addSettingCard(ocr_engine_card);

    // The PP-OCR code is always built in; only models/ and onnxruntime.dll may be missing, so the
    // manager card always shows (the reference shows it whenever ppocr_rust is installed).
    // "Download more models..." at the end of the combo opens the same window.
    auto *manage_card = new SettingCard(FluentIcon::Download, dialog->tr_text("Manage PP-OCR Models"),
                                        dialog->tr_text("Download models for other languages or speeds, or add your own."), grp_ocr);
    auto *manage_models_btn = new PushButton(dialog->tr_text("Manage Models"), manage_card);
    QObject::connect(manage_models_btn, &QPushButton::clicked, dialog, [dialog] { open_model_manager(dialog); });
    manage_card->addControl(manage_models_btn);
    grp_ocr->addSettingCard(manage_card);

    auto *ocr_position_card = new SettingCard(FluentIcon::Layout, dialog->tr_text("OCR Result Window Position"), dialog->tr_text("Where the text recognition result window appears."), grp_ocr);
    auto *position_combo = new ComboBox(ocr_position_card);
    // The labels follow OCR_RESULT_POSITIONS.
    const char *position_labels[] = {"Capture display center", "Primary display center", "Beside capture area", "Near cursor", "Last position"};
    for (int i = 0; i < OCR_RESULT_POSITIONS.size(); ++i) position_combo->addItem(dialog->tr_text(position_labels[i]), OCR_RESULT_POSITIONS[i]);
    position_combo->setCurrentIndex(OCR_RESULT_POSITIONS.indexOf(config->get_ocr_result_position()));
    dialog->set_attr("ocr_result_position_combo", position_combo);
    ocr_position_card->addControl(position_combo);
    grp_ocr->addSettingCard(ocr_position_card);
    if (!ocr_available)
        grp_ocr->addSettingCard(new SettingCard(FluentIcon::Info,
                                                dialog->tr_text("OCR unavailable: install the Snipping Tool from the Microsoft Store, or download the full version"),
                                                {}, grp_ocr));
    layout->addWidget(grp_ocr);

    auto *hint = new CaptionLabel(dialog->tr_text("💡 Hint: Even with auto-save off, it will be copied to clipboard."), view);
    hint->setStyleSheet("padding: 5px;");
    layout->addWidget(hint);
    layout->addStretch();
    scroll->setWidget(view);
    return scroll;
}

std::pair<QString, std::optional<QString>> ocr_choice(const QString &data) {
    if (data.startsWith(PPOCR_PREFIX)) return {OCRManager::ENGINE_PP_RUST, data.mid(PPOCR_PREFIX.size())};
    return {data, std::nullopt};
}

void fill_ocr_engines(SettingsDialog *dialog, const QString &engine, const QString &model_id) {
    auto *combo = dialog->attr<QComboBox>("ocr_engine_combo");
    if (!combo) return;
    struct Item {
        QString label, data;
        bool enabled;
    };
    std::vector<Item> items;
    if (combo->property("has_oneocr").toBool()) items.push_back({dialog->tr_text("Snipping Tool OCR"), OCRManager::ENGINE_ONEOCR, true});
    // Models also need models/onnxruntime.dll; without it they are listed but cannot be picked.
    const bool runtime_ready = ppocr_runtime_supported(ppocr_models::runtime_path());
    QHash<QString, int> order;
    const auto catalog = ppocr_models::catalog().value("models").toArray();
    for (int i = 0; i < catalog.size(); ++i) order.insert(catalog[i].toObject().value("id").toString(), i);
    std::vector<QJsonObject> models;
    for (const auto &value : ppocr_models::installed()) models.push_back(value.toObject());
    std::stable_sort(models.begin(), models.end(), [&](const QJsonObject &a, const QJsonObject &b) {
        return order.value(a.value("catalog_id").toString(), catalog.size()) < order.value(b.value("catalog_id").toString(), catalog.size());
    });
    for (const auto &model : models) items.push_back({model.value("name").toString(), PPOCR_PREFIX + model.value("id").toString(), runtime_ready});
    const bool any = std::any_of(items.begin(), items.end(), [](const Item &item) { return item.enabled; });
    items.insert(items.begin(), {dialog->tr_text("Auto (Recommended)"), OCRManager::ENGINE_AUTO, any});
    const QString selected = engine == OCRManager::ENGINE_PP_RUST ? PPOCR_PREFIX + model_id : engine;
    if (std::none_of(items.begin(), items.end(), [&](const Item &item) { return item.data == selected; })) {
        const QString name = engine == OCRManager::ENGINE_PP_RUST ? (model_id.isEmpty() ? QString("PP-OCRv6 small") : model_id) : dialog->tr_text("Snipping Tool OCR");
        items.push_back({dialog->tr_text("%1 (not installed)").replace("%1", name), selected, false});
    }
    const QSignalBlocker blocker(combo);
    combo->clear();
    auto *model = qobject_cast<QStandardItemModel *>(combo->model());
    for (const auto &item : items) {
        combo->addItem(item.label, item.data);
        if (model) model->item(combo->count() - 1)->setEnabled(item.enabled);
    }
    combo->insertSeparator(combo->count());
    combo->addItem(dialog->tr_text("Download more models..."), MANAGE_MODELS);
    combo->setCurrentIndex(combo->findData(selected));
    combo->setProperty("selected", selected);
}
}
