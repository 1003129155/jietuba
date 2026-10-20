#include "settings/tool_settings.h"
#include <algorithm>
#include "core/constants.h"
#include "core/global_hotkey.h"
#include "core/logger.h"
#include "core/i18n.h"
#include "core/py_text.h"
#include <QDate>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <cmath>
#include <stdexcept>
#include <limits>

static void initializeDefaultsResource() {
    static const bool initialized = [] { Q_INIT_RESOURCE(settings); return true; }();
    Q_UNUSED(initialized);
}

namespace jietuba {
namespace {
bool python_truth(const QVariant &value) {
    if (!value.isValid() || value.isNull()) return false;
    const auto type = value.metaType().id();
    if (type == QMetaType::QString) return !value.toString().isEmpty();
    if (type == QMetaType::QVariantList || type == QMetaType::QStringList) return !value.toList().isEmpty();
    if (type == QMetaType::QVariantMap) return !value.toMap().isEmpty();
    return value.toBool();
}
QString python_string(const QVariant &value) {
    if (!value.isValid() || value.isNull()) return "None";
    if (value.metaType().id() == QMetaType::Bool) return value.toBool() ? "True" : "False";
    return value.toString();
}
QVariant coerce_tool_value(QVariant raw, const QVariant &fallback) {
    const auto type = fallback.metaType().id();
    if (type == QMetaType::Bool) {
        if (raw.metaType().id() == QMetaType::QString) {
            return QStringList{"1", "true", "yes", "on"}.contains(raw.toString().toLower());
        }
        return python_truth(raw);
    }
    const bool list = raw.metaType().id() == QMetaType::QVariantList || raw.metaType().id() == QMetaType::QStringList;
    if (type == QMetaType::QString) {
        if (list) return raw.toList().isEmpty() ? fallback : raw.toList().first();
        return python_string(raw);
    }
    if (list) {
        if (raw.toList().isEmpty()) return fallback;
        raw = raw.toList().first();
    }
    if (raw.metaType().id() == QMetaType::QVariantMap) return fallback;
    bool ok = false;
    const double value = raw.toDouble(&ok);
    if (!ok) return fallback;
    if (type == QMetaType::Int || type == QMetaType::LongLong) {
        if (!std::isfinite(value) || value < double(std::numeric_limits<qint64>::min()) || value >= double(std::numeric_limits<qint64>::max())) return fallback;
        return QVariant::fromValue(qint64(value));
    }
    if (type == QMetaType::Double) return value;
    return raw;
}
QVariant restore_type(const QJsonValue &value, const QString &type) {
    if (type == "int") return QVariant::fromValue(qint64(value.toDouble()));
    if (type == "float") return value.toDouble();
    return value.toVariant();
}
}

QVariant ToolSettings::get(const QString &key, const QVariant &fallback) const {
    return current_.value(key, fallback.isValid() && !fallback.isNull() ? fallback : defaults.value(key));
}
void ToolSettings::update(const QVariantMap &values) {
    for (auto it = values.begin(); it != values.end(); ++it) current_[it.key()] = it.value();
}
void ToolSettings::from_dict(const QVariantMap &values) {
    update(values);
}

QVariant SettingsStore::stored_int(const QVariant &value) {
    const int type = value.metaType().id();
    if (type != QMetaType::LongLong && type != QMetaType::ULongLong && type != QMetaType::UInt) return value;
    bool ok = false;
    const qint64 number = value.toLongLong(&ok);
    if (!ok || number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max()) return value;
    return int(number);
}

ToolSettingsManager::ToolSettingsManager(QSettings &settings, QObject *parent) : QObject(parent), qsettings(settings) {
    initializeDefaultsResource();
    QFile file(":/settings/defaults.json");
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Missing verified Python default registry");
    const auto data = QJsonDocument::fromJson(file.readAll()).object();
    for (const auto &tool : data["tool_order"].toArray()) tool_order_.append(tool.toString());
    const auto apps = data["app"].toObject(), app_types = data["app_types"].toObject();
    for (auto it = apps.begin(); it != apps.end(); ++it) app_defaults_[it.key()] = restore_type(it.value(), app_types[it.key()].toString());
    app_defaults_["screenshot_save_path"] = QDir::toNativeSeparators(QDir::homePath() + "/Pictures/jietuba_photos");
    // The registry holds the generating machine's expanded path; it is rebuilt for this user.
    app_defaults_["log_dir"] = QDir::toNativeSeparators(::jietuba::get_log_dir());
    const auto tools = data["tools"].toObject(), tool_types = data["tool_types"].toObject();
    for (auto it = tools.begin(); it != tools.end(); ++it) {
        QVariantMap defaults;
        const auto values = it.value().toObject(), types = tool_types[it.key()].toObject();
        for (auto v = values.begin(); v != values.end(); ++v) defaults[v.key()] = restore_type(v.value(), types[v.key()].toString());
        tool_defaults_[it.key()] = defaults;
        ToolSettings tool(it.key(), defaults);
        _load_tool_settings(tool);
        tools_.insert(it.key(), tool);
    }
}
void ToolSettingsManager::_load_tool_settings(ToolSettings &tool) {
    for (auto it = tool.defaults.begin(); it != tool.defaults.end(); ++it) {
        const auto raw = qsettings.value("tools/" + tool.tool_id + "/" + it.key(), it.value());
        tool.set(it.key(), coerce_tool_value(raw, it.value()));
    }
}
void ToolSettingsManager::_save_tool_settings(const ToolSettings &tool) {
    const auto values = tool.to_dict();
    for (auto it = values.begin(); it != values.end(); ++it) qsettings.setValue("tools/" + tool.tool_id + "/" + it.key(), it.value());
}
ToolSettings *ToolSettingsManager::get_tool_settings(const QString &tool) {
    auto it = tools_.find(tool);
    return it == tools_.end() ? nullptr : &it.value();
}
QVariant ToolSettingsManager::get_setting(const QString &tool, const QString &key, const QVariant &fallback) const {
    const auto it = tools_.find(tool);
    return it == tools_.end() ? fallback : it->get(key, fallback);
}
void ToolSettingsManager::set_setting(const QString &tool, const QString &key, const QVariant &value, bool save) {
    if (auto *settings = get_tool_settings(tool)) {
        settings->set(key, value);
        if (save) _save_tool_settings(*settings);
        emit settings_changed(tool, settings->to_dict());
    }
}
void ToolSettingsManager::update_settings(const QString &tool, const QVariantMap &values, bool save) {
    if (auto *settings = get_tool_settings(tool)) {
        settings->update(values);
        if (save) _save_tool_settings(*settings);
        emit settings_changed(tool, settings->to_dict());
    }
}
void ToolSettingsManager::save_all() { for (const auto &id : tool_order_) _save_tool_settings(tools_[id]); }
void ToolSettingsManager::reset_tool(const QString &tool, bool save) {
    if (auto *settings = get_tool_settings(tool)) {
        settings->reset_to_defaults();
        if (save) _save_tool_settings(*settings);
        emit settings_changed(tool, settings->to_dict());
    }
}
void ToolSettingsManager::reset_all() { for (const auto &id : tool_order_) reset_tool(id); }
void ToolSettingsManager::reset_app_settings() {
    for (auto it = app_defaults_.begin(); it != app_defaults_.end(); ++it) {
        const auto &key = it.key();
        const auto path = key.startsWith("inapp_") ? "inapp/" + key : key.startsWith("pin_") ? "pin/" + key.mid(4) : "app/" + key;
        qsettings.setValue(path, it.value());
    }
}
QVariant ToolSettingsManager::typed_value(const QString &path, const QVariant &fallback) const {
    const auto raw = qsettings.value(path, fallback);
    if (!fallback.isValid() || fallback.isNull()) return raw;
    const auto type = fallback.metaType().id();
    if (type == QMetaType::Bool) return raw.toBool();
    if (type == QMetaType::Int || type == QMetaType::LongLong) return raw.toLongLong();
    if (type == QMetaType::Double) return raw.toDouble();
    return raw.toString();
}
QString ToolSettingsManager::app_setting_path(const QString &key) {
    return key.startsWith("pin_") ? "pin/" + key.mid(4) : "app/" + key;
}
QVariant ToolSettingsManager::get_app_setting(const QString &key, QVariant fallback) const {
    if (!fallback.isValid() || fallback.isNull()) fallback = app_defaults_.value(key);
    return typed_value(app_setting_path(key), fallback);
}
void ToolSettingsManager::set_app_setting(const QString &key, const QVariant &value) {
    qsettings.setValue(app_setting_path(key), value);
}
QString ToolSettingsManager::get_inapp_shortcut(const QString &key) const {
    return qsettings.value("inapp/" + key, app_defaults_.value(key, "")).toString();
}
void ToolSettingsManager::set_inapp_shortcut(const QString &key, const QString &value) { qsettings.setValue("inapp/" + key, value); }
QString ToolSettingsManager::get_hotkey() const { return qsettings.value("app/hotkey", app_defaults_["hotkey"]).toString(); }
QString ToolSettingsManager::get_clipboard_hotkey() const { return qsettings.value("clipboard/hotkey", app_defaults_["clipboard_hotkey"]).toString(); }

const std::vector<GlobalHotkeyFunction> &GLOBAL_HOTKEY_FUNCTIONS() {
    static const std::vector<GlobalHotkeyFunction> functions = {
        {"screenshot", "hotkey", "app/hotkey"},
        {"clipboard", "clipboard_hotkey", "clipboard/hotkey"},
        {"pin_clipboard", "pin_clipboard_hotkey", "clipboard/pin_hotkey"},
        {"translation", "translation_hotkey", "app/translation_hotkey"},
        {"screen_annotation", "screen_annotation_hotkey", "app/screen_annotation_hotkey"},
    };
    return functions;
}

QString global_hotkey_slot_suffix(int slot) { return slot ? QString("_%1").arg(slot + 1) : QString(); }

namespace {
const GlobalHotkeyFunction &global_hotkey_function(const QString &function) {
    for (const auto &entry : GLOBAL_HOTKEY_FUNCTIONS())
        if (function == QLatin1String(entry.function)) return entry;
    throw std::invalid_argument(("unknown global hotkey function: " + function).toStdString());
}

bool is_win_v(const QString &text) {
    try {
        return global_hotkey::parse_hotkey(text) == std::pair<int, int>(global_hotkey::MOD_WIN, 'V');
    } catch (const std::invalid_argument &) {
        return false;
    }
}
}

QStringList ToolSettingsManager::get_global_hotkeys(const QString &function) const {
    const auto &entry = global_hotkey_function(function);
    QStringList values;
    for (int slot = 0; slot < GLOBAL_HOTKEY_SLOTS; ++slot) {
        const QString suffix = global_hotkey_slot_suffix(slot);
        values.append(qsettings.value(entry.path + suffix, app_defaults_.value(entry.default_name + suffix)).toString());
    }
    if (function == "clipboard") migrate_win_v_slot(values);
    return values;
}

void ToolSettingsManager::migrate_win_v_slot(QStringList &values) const {
    const QString path = QString("clipboard/hotkey") + global_hotkey_slot_suffix(WIN_V_CLIPBOARD_SLOT);
    if (qsettings.contains(path)) return;
    values[WIN_V_CLIPBOARD_SLOT] = "";
    if (!get_app_setting("clipboard_take_over_win_v").toBool()) return;
    // Not again when another slot already holds Win+V (the kind old versions could not register).
    for (const auto &entry : GLOBAL_HOTKEY_FUNCTIONS()) {
        for (int slot = 0; slot < GLOBAL_HOTKEY_SLOTS; ++slot) {
            if (QLatin1String(entry.function) == QLatin1String("clipboard") && slot == WIN_V_CLIPBOARD_SLOT) continue;
            const QString suffix = global_hotkey_slot_suffix(slot);
            if (is_win_v(qsettings.value(entry.path + suffix, app_defaults_.value(entry.default_name + suffix)).toString())) return;
        }
    }
    values[WIN_V_CLIPBOARD_SLOT] = "win+v";
}

void ToolSettingsManager::set_global_hotkeys(const QString &function, const QStringList &values) {
    if (values.size() != GLOBAL_HOTKEY_SLOTS)
        throw std::invalid_argument(QString("expected %1 hotkeys, got %2").arg(GLOBAL_HOTKEY_SLOTS).arg(values.size()).toStdString());
    const auto &entry = global_hotkey_function(function);
    for (int slot = 0; slot < GLOBAL_HOTKEY_SLOTS; ++slot) qsettings.setValue(entry.path + global_hotkey_slot_suffix(slot), values[slot]);
    // Versions that know only two slots take over Win+V through the old switch.
    if (function == "clipboard") set_app_setting("clipboard_take_over_win_v", std::any_of(values.begin(), values.end(), is_win_v));
}
QVariantMap ToolSettingsManager::export_settings() const {
    QVariantMap result;
    for (auto it = tools_.begin(); it != tools_.end(); ++it) result[it.key()] = it->to_dict();
    return result;
}
void ToolSettingsManager::import_settings(const QVariantMap &values) {
    for (auto it = values.begin(); it != values.end(); ++it) if (auto *tool = get_tool_settings(it.key())) {
        tool->from_dict(it->toMap());
        _save_tool_settings(*tool);
    }
}
bool ToolSettingsManager::is_first_run() const { return !qsettings.value("app/has_run_before", false).toBool(); }
void ToolSettingsManager::mark_as_run() { qsettings.setValue("app/has_run_before", true); qsettings.sync(); }
bool ToolSettingsManager::is_absolute_save_path(const QString &value) {
#ifdef Q_OS_WIN
    // ntpath.isabs in Python 3.11 accepts rooted paths as well.
    return value.startsWith('/') || value.startsWith('\\') ||
        (value.size() >= 3 && value[1] == ':' && (value[2] == '/' || value[2] == '\\'));
#else
    return QDir::isAbsolutePath(value);
#endif
}
QString ToolSettingsManager::get_screenshot_save_path() const {
    const auto fallback = app_defaults_.value("screenshot_save_path").toString();
    auto path = qsettings.value("app/screenshot_save_path", fallback).toString();
    if (path.isEmpty() || !is_absolute_save_path(path)) {
        path = fallback;
        qsettings.setValue("app/screenshot_save_path", path);
    }
    return path;
}
void ToolSettingsManager::set_screenshot_save_path(const QString &value) {
    if (!value.isEmpty() && is_absolute_save_path(value)) qsettings.setValue("app/screenshot_save_path", value);
}

ToolSettingsManager *get_tool_settings_manager(QSettings *qsettings) {
    static ToolSettingsManager *manager = nullptr;
    if (!manager) {
        if (!qsettings) {
            const auto sandbox = test_sandbox_dir();
            qsettings = sandbox.isEmpty() ? new QSettings(TOOL_SETTINGS_ORGANIZATION, TOOL_SETTINGS_APPLICATION)
                                          : new QSettings(QDir(sandbox).filePath("settings.ini"), QSettings::IniFormat);
        }
        manager = new ToolSettingsManager(*qsettings);
    }
    return manager;
}
QString ToolSettingsManager::get_screenshot_watermark_time_format() const {
    const auto fallback = app_defaults_.value("screenshot_watermark_time_format").toString();
    const auto value = qsettings.value("app/screenshot_watermark_time_format", fallback).toString();
    return value.isEmpty() ? fallback : value;
}


QString ToolSettingsManager::get_capture_engine() const {
    const auto fallback = app_defaults_.value("capture_engine").toString();
    const auto engine = qsettings.value("app/capture_engine", fallback).toString().toLower();
    return CAPTURE_ENGINE_CHOICES.contains(engine) ? engine : fallback;
}

void ToolSettingsManager::set_capture_engine(const QString &value) {
    auto engine = value.toLower();
    if (!CAPTURE_ENGINE_CHOICES.contains(engine)) engine = app_defaults_.value("capture_engine").toString();
    qsettings.setValue("app/capture_engine", engine);
}

const std::vector<MouseAction> &CAPTURE_MOUSE_ACTIONS() {
    static const std::vector<MouseAction> actions = {
        {"copy", "Copy to Clipboard", "doubleleft", "capture"},
        {"pin", "Pin to Screen", "", "capture"},
        {"save", "Save to File", "", "capture"},
        {"quick_save", "Quick Save", "", "capture"},
        {"close", "Close Screenshot", "right", "capture"},
    };
    return actions;
}

const std::vector<MouseAction> &PIN_MOUSE_ACTIONS() {
    static const std::vector<MouseAction> actions = {
        {"zoom", "Zoom Pinned Image", "wheel", "wheel"},
        {"fine_zoom", "Fine Zoom Pinned Image", "alt+wheel", "wheel"},
        {"opacity", "Adjust Opacity", "ctrl+wheel", "wheel"},
        {"close", "Close Pinned Image", "", "click"},
        {"reset", "Reset Pinned Image Size", "", "click"},
        {"thumbnail", "Toggle Pin Thumbnail", "shift+doubleleft", "click"},
        {"region", "Select Thumbnail Region", "dragright", "drag"},
        {"copy_text", "Copy Selected Text", "", "click"},
    };
    return actions;
}

const std::vector<MouseAction> &CLIPBOARD_MOUSE_ACTIONS() {
    static const std::vector<MouseAction> actions = {
        {"paste", "Paste", "left", "click"},
        {"pin", "Pin Image", "middle", "click"},
        {"quick_edit", "Quick Edit Text", "middle", "click"},
        {"menu", "Open Context Menu", "right", "click"},
    };
    return actions;
}

const std::vector<MouseAction> &QUICK_CAPTURE_ACTIONS() {
    static const std::vector<MouseAction> actions = {
        {"copy", "Screenshot", "", "global"},
        {"pin", "Pin to Screen", "win+dragleft", "global"},
        {"copy_pin", "Copy and Pin", "", "global"},
        {"ocr", "Region Text Recognition", "", "global"},
        {"translate", "Region Translation", "", "global"},
        {"edit", "Capture and Edit", "", "global"},
    };
    return actions;
}

namespace {
QString default_binding(const std::vector<MouseAction> &actions, const QString &action) {
    for (const auto &entry : actions)
        if (action == entry.key) return entry.binding;
    throw std::out_of_range(("unknown mouse action: " + action).toStdString());
}
}

QString get_quick_capture_binding(const ToolSettingsManager *config, const QString &action) {
    return config->get_app_setting("quick_capture_" + action, default_binding(QUICK_CAPTURE_ACTIONS(), action)).toString();
}

std::vector<std::pair<std::pair<QStringList, QString>, QString>> get_quick_capture_bindings(const ToolSettingsManager *config) {
    std::vector<std::pair<std::pair<QStringList, QString>, QString>> bindings;
    for (const auto &entry : QUICK_CAPTURE_ACTIONS()) {
        auto parts = get_quick_capture_binding(config, entry.key).toLower().split('+');
        const auto gesture = parts.takeLast();
        // A set: duplicates collapse and order does not matter.
        QStringList modifiers;
        for (const auto &modifier : parts)
            if (!modifiers.contains(modifier)) modifiers.append(modifier);
        modifiers.sort();
        const auto button = gesture.startsWith("drag") ? gesture.mid(4) : gesture;
        bool known = true;
        for (const auto &modifier : modifiers) known = known && QUICK_CAPTURE_MODIFIERS.contains(modifier);
        if (!(gesture.startsWith("drag") && QUICK_CAPTURE_BUTTONS.contains(button) && modifiers.size() >= 1 && modifiers.size() <= 2 && known)) continue;
        const auto key = std::make_pair(modifiers, button);
        bool seen = false;
        for (const auto &existing : bindings) seen = seen || existing.first == key;
        if (!seen) bindings.emplace_back(key, entry.key);
    }
    return bindings;
}

QString get_clipboard_mouse_binding(const ToolSettingsManager *config, const QString &action) {
    return config->get_app_setting("mouse_clipboard_" + action, default_binding(CLIPBOARD_MOUSE_ACTIONS(), action)).toString();
}

QString get_pin_mouse_binding(const ToolSettingsManager *config, const QString &action) {
    auto fallback = default_binding(PIN_MOUSE_ACTIONS(), action);
    if (!config) return fallback;
    if (action == "reset") {
        const auto legacy = config->get_inapp_shortcut("inapp_pin_reset_size");
        if (legacy.contains("mousemiddle")) fallback = QString(legacy).replace("mousemiddle", "middle");
    }
    return config->get_app_setting("mouse_pin_" + action, fallback).toString();
}

QString get_capture_mouse_binding(const ToolSettingsManager *config, const QString &action) {
    auto fallback = default_binding(CAPTURE_MOUSE_ACTIONS(), action);
    if (!config) return fallback;
    if (action == "copy" && !config->get_double_click_copy_close_enabled()) fallback = "";
    return config->get_app_setting("mouse_capture_" + action, fallback).toString();
}

const std::vector<ToolShortcut> &ANNOTATION_TOOL_SHORTCUTS() {
    static const std::vector<ToolShortcut> shortcuts = {
        {"inapp_tool_cursor", "cursor", "Select / Cursor", "s"},
        {"inapp_tool_pen", "pen", "Pen", "p"},
        {"inapp_tool_highlighter", "highlighter", "Highlighter", "m"},
        {"inapp_tool_mosaic", "mosaic", "Mosaic", "x"},
        {"inapp_tool_arrow", "arrow", "Arrow", "a"},
        {"inapp_tool_number", "number", "Number", "n"},
        {"inapp_tool_rect", "rect", "Rectangle", "r"},
        {"inapp_tool_ellipse", "ellipse", "Ellipse", "o"},
        {"inapp_tool_text", "text", "Text", "t"},
        {"inapp_tool_eraser", "eraser", "Eraser", "e"},
    };
    return shortcuts;
}

QString clipboard_pick_keys(const QString &mode) {
    static const QMap<QString, QString> keys = {
        {"both", "123456789abcdefghijklmnopqrstuvwxyz"},
        {"letters", "abcdefghijklmnopqrstuvwxyz"},
        {"digits", "123456789"},
    };
    return keys.value(mode, keys.value("both"));
}

namespace {
// A string setting restricted to choices; anything else reads as the default.
QString choice_value(const SettingsStore &qsettings, const QString &key, const QString &fallback, const QStringList &choices) {
    const auto value = qsettings.value(key, fallback).toString().toLower();
    return choices.contains(value) ? value : fallback;
}
}

void ToolSettingsManager::set_trimmed(const QString &key, const QString &value) { qsettings.setValue(key, value.trimmed()); }

QString ToolSettingsManager::get_smart_selection_mode() const {
    if (!get_smart_selection()) return "off";
    return choice_value(qsettings, "app/smart_selection_mode", app_defaults_.value("smart_selection_mode").toString(), STORED_SMART_SELECTION_MODES);
}

void ToolSettingsManager::set_smart_selection_mode(const QString &value) {
    auto mode = (value.isEmpty() ? QString("off") : value).toLower();
    if (!SMART_SELECTION_MODES.contains(mode)) mode = app_defaults_.value("smart_selection_mode").toString();
    if (mode != "off") qsettings.setValue("app/smart_selection_mode", mode);
    qsettings.setValue("app/smart_selection", mode != "off");
}

QString ToolSettingsManager::get_ocr_engine() const { return choice_value(qsettings, "app/ocr_engine", app_defaults_.value("ocr_engine").toString(), OCR_ENGINES); }

void ToolSettingsManager::set_ocr_engine(const QString &value) {
    auto engine = value.toLower();
    if (!OCR_ENGINES.contains(engine)) engine = app_defaults_.value("ocr_engine").toString();
    qsettings.setValue("app/ocr_engine", engine);
}

int ToolSettingsManager::get_shift_line_angle() const {
    const int fallback = app_defaults_.value("shift_line_angle").toInt();
    bool ok = false;
    const int angle = qsettings.value("app/shift_line_angle", fallback).toString().toInt(&ok);
    return ok && SHIFT_LINE_ANGLES.contains(angle) ? angle : fallback;
}

void ToolSettingsManager::set_shift_line_angle(int value) {
    qsettings.setValue("app/shift_line_angle", SHIFT_LINE_ANGLES.contains(value) ? value : app_defaults_.value("shift_line_angle").toInt());
}

QString ToolSettingsManager::get_ocr_result_position() const {
    return choice_value(qsettings, "app/ocr_result_position", app_defaults_.value("ocr_result_position").toString(), OCR_RESULT_POSITIONS);
}

void ToolSettingsManager::set_ocr_result_position(const QString &value) {
    auto position = value.toLower();
    if (!OCR_RESULT_POSITIONS.contains(position)) position = app_defaults_.value("ocr_result_position").toString();
    qsettings.setValue("app/ocr_result_position", position);
}

std::optional<QPoint> ToolSettingsManager::get_ocr_result_last_center() const {
    const auto parts = qsettings.value("app/ocr_result_last_center", "").toString().split(',');
    if (parts.size() != 2) return std::nullopt;
    bool ok_x = false, ok_y = false;
    const int x = parts[0].trimmed().toInt(&ok_x), y = parts[1].trimmed().toInt(&ok_y);
    if (!ok_x || !ok_y) return std::nullopt;
    return QPoint(x, y);
}

QString ToolSettingsManager::get_ocr_layout_mode() const {
    return choice_value(qsettings, "app/ocr_layout_mode", app_defaults_.value("ocr_layout_mode").toString(), OCR_LAYOUT_MODES);
}

void ToolSettingsManager::set_ocr_layout_mode(const QString &value) {
    auto mode = value.toLower();
    if (!OCR_LAYOUT_MODES.contains(mode)) mode = app_defaults_.value("ocr_layout_mode").toString();
    qsettings.setValue("app/ocr_layout_mode", mode);
}

QString ToolSettingsManager::get_ocr_punctuation() const {
    return choice_value(qsettings, "app/ocr_punctuation", app_defaults_.value("ocr_punctuation").toString(), OCR_PUNCTUATION_MODES);
}

void ToolSettingsManager::set_ocr_punctuation(const QString &value) {
    auto mode = value.toLower();
    if (!OCR_PUNCTUATION_MODES.contains(mode)) mode = app_defaults_.value("ocr_punctuation").toString();
    qsettings.setValue("app/ocr_punctuation", mode);
}

void ToolSettingsManager::set_translation_provider(const QString &provider_id) {
    qsettings.setValue("translation/active_provider", (provider_id.isEmpty() ? QString("deepl") : provider_id).trimmed().toLower());
}

QVariantMap ToolSettingsManager::get_translation_provider_config(const QString &requested) const {
    const auto provider_id = requested.trimmed().toLower();
    if (provider_id == "deepl") return {{"api_key", get_deepl_api_key()}, {"use_pro", get_deepl_use_pro()}};
    if (provider_id == "amazon")
        return {{"region", get_amazon_translate_region()}, {"access_key_id", get_amazon_translate_access_key_id()},
                {"secret_access_key", get_amazon_translate_secret_access_key()}, {"session_token", get_amazon_translate_session_token()}};
    if (provider_id == "google") return {{"api_key", get_google_translate_api_key()}};
    if (provider_id == "azure")
        return {{"api_key", get_azure_translate_api_key()}, {"region", get_azure_translate_region()}, {"endpoint", get_azure_translate_endpoint()}};
    if (provider_id == "baidu") return {{"appid", get_baidu_translate_appid()}, {"secret_key", get_baidu_translate_secret_key()}};
    if (provider_id == "volcengine")
        return {{"access_key_id", get_volcengine_translate_access_key()}, {"secret_access_key", get_volcengine_translate_secret_key()}};
    if (provider_id == "tencent") return {{"secret_id", get_tencent_translate_secret_id()}, {"secret_key", get_tencent_translate_secret_key()}};
    if (provider_id == "youdao") return {{"app_key", get_youdao_translate_app_key()}, {"app_secret", get_youdao_translate_app_secret()}};
    if (provider_id == "deepseek") return {{"api_key", get_deepseek_api_key()}, {"model", get_deepseek_model()}, {"base_url", get_deepseek_base_url()}};
    if (CUSTOM_LLM_PROVIDER_IDS.contains(provider_id)) {
        QVariantMap config;
        for (const auto &key : CUSTOM_LLM_TEXT_KEYS) config[key] = get_custom_llm_text(provider_id, key);
        config["json_mode"] = get_custom_llm_json_mode(provider_id);
        return config;
    }
    return {};
}

void ToolSettingsManager::set_amazon_translate_region(const QString &value) {
    qsettings.setValue("translation/providers/amazon/region", (value.isEmpty() ? QString("us-west-2") : value).trimmed());
}

QString ToolSettingsManager::get_volcengine_translate_quota_limit() const {
    return qsettings.value("translation/providers/volcengine/quota_limit", app_defaults_.value("volcengine_translate_quota_limit")).toString().trimmed();
}

QString ToolSettingsManager::get_tencent_translate_quota_limit() const {
    return qsettings.value("translation/providers/tencent/quota_limit", app_defaults_.value("tencent_translate_quota_limit")).toString().trimmed();
}

bool ToolSettingsManager::get_translation_quota_enabled(const QString &requested) const {
    const auto provider_id = requested.trimmed().toLower();
    if (provider_id == "volcengine") return get_volcengine_translate_quota_enabled();
    if (provider_id == "tencent") return get_tencent_translate_quota_enabled();
    return true;
}

qint64 ToolSettingsManager::get_translation_quota_limit(const QString &requested) const {
    const auto provider_id = requested.trimmed().toLower();
    QString limit;
    if (provider_id == "volcengine") limit = get_volcengine_translate_quota_limit();
    else if (provider_id == "tencent") limit = get_tencent_translate_quota_limit();
    if (limit.isEmpty()) return 0;
    return py_int(limit).value_or(0);
}

namespace {
// The month bucket of the usage counters, local time.
QString current_month() { return QDate::currentDate().toString("yyyyMM"); }
}

qint64 ToolSettingsManager::get_translation_usage_chars(const QString &provider_id) const {
    return qsettings.value("translation/usage/" + current_month() + "/" + provider_id, 0).toLongLong();
}

void ToolSettingsManager::add_translation_usage_chars(const QString &provider_id, qint64 chars) {
    if (chars <= 0) return;
    const auto key = "translation/usage/" + current_month() + "/" + provider_id;
    qsettings.setValue(key, qsettings.value(key, 0).toLongLong() + chars);
}

namespace {
// (slot, key) of a custom service field such as "custom_llm_2_api_key"; longer slots first,
// since "custom_llm" is a prefix of the others.
std::optional<std::pair<QString, QString>> custom_llm_field(const QString &config_key) {
    QStringList slot_ids = CUSTOM_LLM_PROVIDER_IDS;
    std::sort(slot_ids.begin(), slot_ids.end(), [](const QString &a, const QString &b) { return a.size() > b.size(); });
    for (const auto &slot_id : slot_ids) {
        if (!config_key.startsWith(slot_id + "_")) continue;
        const QString key = config_key.mid(slot_id.size() + 1);
        if (key == "json_mode" || CUSTOM_LLM_TEXT_KEYS.contains(key)) return std::make_pair(slot_id, key);
        return std::nullopt;
    }
    return std::nullopt;
}
}

std::optional<QVariant> ToolSettingsManager::get_provider_field(const QString &config_key) const {
    if (const auto field = custom_llm_field(config_key))
        return field->second == "json_mode" ? QVariant(get_custom_llm_json_mode(field->first)) : QVariant(get_custom_llm_text(field->first, field->second));
    static const QMap<QString, QString (ToolSettingsManager::*)() const> text_getters{
        {"volcengine_translate_quota_limit", &ToolSettingsManager::get_volcengine_translate_quota_limit},
        {"tencent_translate_quota_limit", &ToolSettingsManager::get_tencent_translate_quota_limit},
    };
    if (const auto getter = text_getters.value(config_key)) return (this->*getter)();
    return generated_accessor_value("get_" + config_key);
}

bool ToolSettingsManager::set_provider_field(const QString &config_key, const QVariant &value) {
    if (const auto field = custom_llm_field(config_key)) {
        if (field->second == "json_mode") set_custom_llm_json_mode(field->first, value.toBool());
        else set_custom_llm_text(field->first, field->second, value.toString());
        return true;
    }
    if (apply_generated_setter("set_" + config_key, value)) return true;
    static const QMap<QString, void (ToolSettingsManager::*)(const QString &)> text_setters{
        {"amazon_translate_region", &ToolSettingsManager::set_amazon_translate_region},
        {"amazon_translate_access_key_id", &ToolSettingsManager::set_amazon_translate_access_key_id},
        {"amazon_translate_secret_access_key", &ToolSettingsManager::set_amazon_translate_secret_access_key},
        {"amazon_translate_session_token", &ToolSettingsManager::set_amazon_translate_session_token},
        {"google_translate_api_key", &ToolSettingsManager::set_google_translate_api_key},
        {"azure_translate_api_key", &ToolSettingsManager::set_azure_translate_api_key},
        {"azure_translate_region", &ToolSettingsManager::set_azure_translate_region},
        {"azure_translate_endpoint", &ToolSettingsManager::set_azure_translate_endpoint},
        {"baidu_translate_appid", &ToolSettingsManager::set_baidu_translate_appid},
        {"baidu_translate_secret_key", &ToolSettingsManager::set_baidu_translate_secret_key},
        {"volcengine_translate_access_key", &ToolSettingsManager::set_volcengine_translate_access_key},
        {"volcengine_translate_secret_key", &ToolSettingsManager::set_volcengine_translate_secret_key},
        {"volcengine_translate_quota_limit", &ToolSettingsManager::set_volcengine_translate_quota_limit},
        {"tencent_translate_secret_id", &ToolSettingsManager::set_tencent_translate_secret_id},
        {"tencent_translate_secret_key", &ToolSettingsManager::set_tencent_translate_secret_key},
        {"tencent_translate_quota_limit", &ToolSettingsManager::set_tencent_translate_quota_limit},
        {"youdao_translate_app_key", &ToolSettingsManager::set_youdao_translate_app_key},
        {"youdao_translate_app_secret", &ToolSettingsManager::set_youdao_translate_app_secret},
        {"deepseek_api_key", &ToolSettingsManager::set_deepseek_api_key},
        {"deepseek_model", &ToolSettingsManager::set_deepseek_model},
        {"deepseek_base_url", &ToolSettingsManager::set_deepseek_base_url},
    };
    static const QMap<QString, void (ToolSettingsManager::*)(bool)> bool_setters{
        {"volcengine_translate_quota_enabled", &ToolSettingsManager::set_volcengine_translate_quota_enabled},
        {"tencent_translate_quota_enabled", &ToolSettingsManager::set_tencent_translate_quota_enabled},
    };
    if (const auto setter = bool_setters.value(config_key)) {
        (this->*setter)(value.toBool());
        return true;
    }
    const auto setter = text_setters.value(config_key);
    if (!setter) return false;
    (this->*setter)(value.toString());
    return true;
}

QString ToolSettingsManager::get_custom_llm_text(const QString &slot, const QString &key) const {
    return qsettings.value("translation/providers/" + slot + "/" + key, app_defaults_.value(slot + "_" + key)).toString();
}

bool ToolSettingsManager::get_custom_llm_json_mode(const QString &slot) const {
    return qsettings.value("translation/providers/" + slot + "/json_mode", app_defaults_.value(slot + "_json_mode")).toBool();
}

QString ToolSettingsManager::get_translation_target_lang() const {
    const auto saved = qsettings.value("app/translation_target_lang", "").toString();
    if (!saved.isEmpty()) return saved;
    static const QMap<QString, QString> lang_map = {{"zh", "ZH"}, {"ja", "JA"}, {"en", "EN"}};
    return lang_map.value(I18nManager::get_system_language(), "EN");
}

QVariantMap ToolSettingsManager::get_translation_params() const {
    const auto saved_target_lang = get_app_setting("translation_target_lang", "").toString();
    return {
        {"api_key", get_deepl_api_key()},
        {"target_lang", !saved_target_lang.isEmpty() ? saved_target_lang : get_translation_target_lang()},
        {"use_pro", get_deepl_use_pro()},
        {"split_sentences", get_translation_split_sentences() ? "nonewlines" : "0"},
        {"preserve_formatting", get_translation_preserve_formatting()},
    };
}

QVariantMap ToolSettingsManager::get_translation_request_params() const {
    const auto saved_target_lang = get_app_setting("translation_target_lang", "").toString();
    return {
        {"target_lang", !saved_target_lang.isEmpty() ? saved_target_lang : get_translation_target_lang()},
        {"split_sentences", get_translation_split_sentences() ? "nonewlines" : "0"},
        {"preserve_formatting", get_translation_preserve_formatting()},
    };
}

QString ToolSettingsManager::get_clipboard_image_size() const {
    const auto fallback = app_defaults_.value("clipboard_image_size").toString();
    const auto value = qsettings.value("clipboard/image_size", fallback).toString();
    return CLIPBOARD_IMAGE_SIZES.contains(value) ? value : fallback;
}

int ToolSettingsManager::get_clipboard_display_lines() const {
    return std::clamp(qsettings.value("clipboard/display_lines", app_defaults_.value("clipboard_display_lines")).toInt(), 1, 5);
}

void ToolSettingsManager::set_clipboard_image_size(const QString &value) {
    if (CLIPBOARD_IMAGE_SIZES.contains(value)) qsettings.setValue("clipboard/image_size", value);
}

QString ToolSettingsManager::get_clipboard_multi_paste_separator() const {
    return qsettings.value("clipboard/multi_paste_separator", app_defaults_.value("clipboard_multi_paste_separator")).toString();
}

QString ToolSettingsManager::get_clipboard_multi_paste_order() const {
    const auto fallback = app_defaults_.value("clipboard_multi_paste_order").toString();
    const auto value = qsettings.value("clipboard/multi_paste_order", fallback).toString();
    return CLIPBOARD_MULTI_PASTE_ORDERS.contains(value) ? value : fallback;
}

void ToolSettingsManager::set_clipboard_multi_paste_order(const QString &value) {
    if (CLIPBOARD_MULTI_PASTE_ORDERS.contains(value)) qsettings.setValue("clipboard/multi_paste_order", value);
}

QString ToolSettingsManager::get_clipboard_group_bar_position() const {
    static const QStringList positions = {"right", "left", "top"};
    const auto value = qsettings.value("clipboard/group_bar_position", app_defaults_.value("clipboard_group_bar_position")).toString();
    return positions.contains(value) ? value : "right";
}

void ToolSettingsManager::set_clipboard_group_bar_position(const QString &value) {
    static const QStringList positions = {"right", "left", "top"};
    if (positions.contains(value)) qsettings.setValue("clipboard/group_bar_position", value);
}

bool ToolSettingsManager::should_show_main_window_on_start() {
    if (is_first_run()) {
        log_debug(T("检测到首次运行，将自动打开设置窗口"), "Startup");
        return true;
    }
    const bool show = get_show_main_window();
    if (show) log_debug(T("根据用户设置：显示设置窗口"), "Startup");
    else log_debug(T("根据用户设置：不自动打开设置窗口"), "Startup");
    return show;
}
}
