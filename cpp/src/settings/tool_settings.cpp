#include "settings/tool_settings.h"
#include <algorithm>
#include "core/constants.h"
#include "core/data_location.h"
#include "core/global_hotkey.h"
#include "core/logger.h"
#include "core/i18n.h"
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
// Integers are held as qint64, as tool defaults are.
template <typename T> QVariant default_value(const T &value) { return QVariant::fromValue(value); }
template <> QVariant default_value<int>(const int &value) { return QVariant::fromValue(qint64(value)); }
template <> QVariant default_value<QVariantList>(const QVariantList &values) {
    QVariantList result;
    for (const auto &value : values) result.append(value.typeId() == QMetaType::Int ? default_value(value.toInt()) : value);
    return result;
}
}

const std::vector<AppSetting> &APP_SETTINGS() {
    static const std::vector<AppSetting> settings = {
#define SETTING_AS(accessor, name, path, Type, initial, scope, getter, setter) {#name, path, SettingScope::scope},
#define SETTING(name, path, Type, initial, scope, getter, setter) SETTING_AS(name, name, path, Type, initial, scope, getter, setter)
#include "settings/app_settings.inc"
#undef SETTING
#undef SETTING_AS
    };
    return settings;
}

const QSet<QString> &LOCAL_SETTING_PATHS() {
    static const QSet<QString> paths = [] {
        QSet<QString> result;
        for (const auto &setting : APP_SETTINGS())
            if (setting.scope == SettingScope::Local) result.insert(setting.path);
        return result;
    }();
    return paths;
}

QSettings &SettingsStore::store(const QString &key) const { return LOCAL_SETTING_PATHS().contains(key) ? *local_ : *settings_; }

void SettingsStore::sync() const {
    settings_->sync();
    if (local_ != settings_) local_->sync();
}

std::unique_ptr<QSettings> open_local_settings() {
    if (!test_sandbox_dir().isEmpty()) return std::make_unique<QSettings>(data_location().settings_file(), QSettings::IniFormat);
    return std::make_unique<QSettings>(LOCAL_SETTINGS_KEY, QSettings::NativeFormat);
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

ToolSettingsManager::ToolSettingsManager(QSettings &settings, QSettings *local_settings, QObject *parent)
    : QObject(parent), qsettings(settings, local_settings) {
    initializeDefaultsResource();
    QFile file(":/settings/defaults.json");
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Missing verified Python default registry");
    const auto data = QJsonDocument::fromJson(file.readAll()).object();
    for (const auto &tool : data["tool_order"].toArray()) tool_order_.append(tool.toString());
#define SETTING_AS(accessor, name, path, Type, initial, scope, getter, setter) app_defaults_.insert(QStringLiteral(#name), default_value<Type>(initial));
#define SETTING(name, path, Type, initial, scope, getter, setter) SETTING_AS(name, name, path, Type, initial, scope, getter, setter)
#include "settings/app_settings.inc"
#undef SETTING
#undef SETTING_AS
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
    static const QHash<QString, QString> paths = [] {
        QHash<QString, QString> result;
        for (const auto &setting : APP_SETTINGS()) result.insert(setting.name, setting.path);
        return result;
    }();
    if (const auto it = paths.constFind(key); it != paths.constEnd()) return *it;
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

std::optional<QVariant> ToolSettingsManager::accessor_value(const QString &name) const {
#define SETTING_AS(accessor, key, path, Type, initial, scope, getter, setter) \
    if (name == QLatin1String(#accessor)) return QVariant(get_##accessor());
#define SETTING(key, path, Type, initial, scope, getter, setter) SETTING_AS(key, key, path, Type, initial, scope, getter, setter)
#include "settings/app_settings.inc"
#undef SETTING
#undef SETTING_AS
    return std::nullopt;
}

bool ToolSettingsManager::set_accessor_value(const QString &name, const QVariant &value) {
#define JIETUBA_SET_BY_NAME_Gen(accessor, path, Type) qsettings.setValue(QStringLiteral(path), value); return true;
#define JIETUBA_SET_BY_NAME_Trim(accessor, path, Type) set_##accessor(value.toString()); return true;
#define JIETUBA_SET_BY_NAME_Own(accessor, path, Type) set_##accessor(setting_detail::cast<Type>(value)); return true;
#define JIETUBA_SET_BY_NAME_None(accessor, path, Type) return false;
#define SETTING_AS(accessor, key, path, Type, initial, scope, getter, setter) \
    if (name == QLatin1String(#accessor)) { JIETUBA_SET_BY_NAME_##setter(accessor, path, Type) }
#define SETTING(key, path, Type, initial, scope, getter, setter) SETTING_AS(key, key, path, Type, initial, scope, getter, setter)
#include "settings/app_settings.inc"
#undef SETTING
#undef SETTING_AS
#undef JIETUBA_SET_BY_NAME_Gen
#undef JIETUBA_SET_BY_NAME_Trim
#undef JIETUBA_SET_BY_NAME_Own
#undef JIETUBA_SET_BY_NAME_None
    return false;
}

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
    if (!get_clipboard_take_over_win_v()) return;
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
    if (function == "clipboard") set_clipboard_take_over_win_v(std::any_of(values.begin(), values.end(), is_win_v));
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
void ToolSettingsManager::mark_as_run() {
    set_has_run_before(true);
    qsettings.sync();
}
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
    auto path = stored_screenshot_save_path().toString();
    if (path.isEmpty() || !is_absolute_save_path(path)) {
        path = default_screenshot_save_path().toString();
        store_screenshot_save_path(path);
    }
    return path;
}
void ToolSettingsManager::set_screenshot_save_path(const QString &value) {
    if (!value.isEmpty() && is_absolute_save_path(value)) store_screenshot_save_path(value);
}

ToolSettingsManager *get_tool_settings_manager(QSettings *qsettings) {
    static ToolSettingsManager *manager = nullptr;
    if (!manager) {
        QSettings *local = nullptr;
        if (!qsettings) {
            qsettings = new QSettings(data_location().settings_file(), QSettings::IniFormat);
            if (test_sandbox_dir().isEmpty()) local = open_local_settings().release();
        }
        manager = new ToolSettingsManager(*qsettings, local);
    }
    return manager;
}
QString ToolSettingsManager::get_screenshot_watermark_time_format() const {
    const auto value = stored_screenshot_watermark_time_format().toString();
    return value.isEmpty() ? default_screenshot_watermark_time_format().toString() : value;
}

namespace {
// A text setting restricted to choices, compared in lower case; anything else reads as fallback.
QString choice_value(const QVariant &stored, const QVariant &fallback, const QStringList &choices) {
    const auto value = stored.toString().toLower();
    return choices.contains(value) ? value : fallback.toString();
}
// What a choice setter stores: the choice in lower case, anything else the default.
QString choice_or_default(const QString &value, const QVariant &fallback, const QStringList &choices) {
    const auto choice = value.toLower();
    return choices.contains(choice) ? choice : fallback.toString();
}
}

QString ToolSettingsManager::get_capture_engine() const {
    return choice_value(stored_capture_engine(), default_capture_engine(), CAPTURE_ENGINE_CHOICES);
}

void ToolSettingsManager::set_capture_engine(const QString &value) {
    store_capture_engine(choice_or_default(value, default_capture_engine(), CAPTURE_ENGINE_CHOICES));
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
        {"scan_code", "Scan QR code / barcode", "", "global"},
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

QString ToolSettingsManager::get_smart_selection_mode() const {
    if (!get_smart_selection()) return "off";
    return choice_value(stored_smart_selection_mode(), default_smart_selection_mode(), STORED_SMART_SELECTION_MODES);
}

void ToolSettingsManager::set_smart_selection_mode(const QString &value) {
    const auto mode = choice_or_default(value.isEmpty() ? QString("off") : value, default_smart_selection_mode(), SMART_SELECTION_MODES);
    if (mode != "off") store_smart_selection_mode(mode);
    set_smart_selection(mode != "off");
}

QString ToolSettingsManager::get_ocr_engine() const { return choice_value(stored_ocr_engine(), default_ocr_engine(), OCR_ENGINES); }

void ToolSettingsManager::set_ocr_engine(const QString &value) { store_ocr_engine(choice_or_default(value, default_ocr_engine(), OCR_ENGINES)); }

int ToolSettingsManager::get_shift_line_angle() const {
    const int fallback = default_shift_line_angle().toInt();
    bool ok = false;
    const int angle = stored_shift_line_angle().toString().toInt(&ok);
    return ok && SHIFT_LINE_ANGLES.contains(angle) ? angle : fallback;
}

void ToolSettingsManager::set_shift_line_angle(int value) {
    store_shift_line_angle(SHIFT_LINE_ANGLES.contains(value) ? value : default_shift_line_angle().toInt());
}

QString ToolSettingsManager::get_ocr_result_position() const {
    return choice_value(stored_ocr_result_position(), default_ocr_result_position(), OCR_RESULT_POSITIONS);
}

void ToolSettingsManager::set_ocr_result_position(const QString &value) {
    store_ocr_result_position(choice_or_default(value, default_ocr_result_position(), OCR_RESULT_POSITIONS));
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
    return choice_value(stored_ocr_layout_mode(), default_ocr_layout_mode(), OCR_LAYOUT_MODES);
}

void ToolSettingsManager::set_ocr_layout_mode(const QString &value) {
    store_ocr_layout_mode(choice_or_default(value, default_ocr_layout_mode(), OCR_LAYOUT_MODES));
}

QString ToolSettingsManager::get_ocr_punctuation() const {
    return choice_value(stored_ocr_punctuation(), default_ocr_punctuation(), OCR_PUNCTUATION_MODES);
}

void ToolSettingsManager::set_ocr_punctuation(const QString &value) {
    store_ocr_punctuation(choice_or_default(value, default_ocr_punctuation(), OCR_PUNCTUATION_MODES));
}

void ToolSettingsManager::set_translation_provider(const QString &provider_id) {
    store_translation_provider((provider_id.isEmpty() ? QString("deepl") : provider_id).trimmed().toLower());
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
        // Each slot's settings are named <slot>_<key>.
        QVariantMap config;
        for (const auto &key : CUSTOM_LLM_TEXT_KEYS) config[key] = get_app_setting(provider_id + "_" + key).toString();
        config["json_mode"] = get_app_setting(provider_id + "_json_mode").toBool();
        return config;
    }
    return {};
}

void ToolSettingsManager::set_amazon_translate_region(const QString &value) {
    store_amazon_translate_region((value.isEmpty() ? QString("us-west-2") : value).trimmed());
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
    // NFKC turns full-width digits and signs typed through an IME into ASCII ones.
    return limit.normalized(QString::NormalizationForm_KC).toLongLong();
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

QString ToolSettingsManager::get_translation_target_lang() const {
    const auto saved = stored_translation_target_lang().toString();
    if (!saved.isEmpty()) return saved;
    static const QMap<QString, QString> lang_map = {{"zh", "ZH"}, {"zh_TW", "ZH-HANT"}, {"ja", "JA"}, {"en", "EN"}};
    return lang_map.value(I18nManager::get_system_language(), "EN");
}

QVariantMap ToolSettingsManager::get_translation_params() const {
    return {
        {"api_key", get_deepl_api_key()},
        {"target_lang", get_translation_target_lang()},
        {"use_pro", get_deepl_use_pro()},
        {"split_sentences", get_translation_split_sentences() ? "nonewlines" : "0"},
        {"preserve_formatting", get_translation_preserve_formatting()},
    };
}

QVariantMap ToolSettingsManager::get_translation_request_params() const {
    return {
        {"target_lang", get_translation_target_lang()},
        {"split_sentences", get_translation_split_sentences() ? "nonewlines" : "0"},
        {"preserve_formatting", get_translation_preserve_formatting()},
    };
}

QString ToolSettingsManager::get_clipboard_image_size() const {
    const auto value = stored_clipboard_image_size().toString();
    return CLIPBOARD_IMAGE_SIZES.contains(value) ? value : default_clipboard_image_size().toString();
}

void ToolSettingsManager::set_clipboard_image_size(const QString &value) {
    if (CLIPBOARD_IMAGE_SIZES.contains(value)) store_clipboard_image_size(value);
}

QString ToolSettingsManager::get_clipboard_multi_paste_order() const {
    const auto value = stored_clipboard_multi_paste_order().toString();
    return CLIPBOARD_MULTI_PASTE_ORDERS.contains(value) ? value : default_clipboard_multi_paste_order().toString();
}

void ToolSettingsManager::set_clipboard_multi_paste_order(const QString &value) {
    if (CLIPBOARD_MULTI_PASTE_ORDERS.contains(value)) store_clipboard_multi_paste_order(value);
}

QString ToolSettingsManager::get_clipboard_group_bar_position() const {
    static const QStringList positions = {"right", "left", "top"};
    const auto value = stored_clipboard_group_bar_position().toString();
    return positions.contains(value) ? value : "right";
}

void ToolSettingsManager::set_clipboard_group_bar_position(const QString &value) {
    static const QStringList positions = {"right", "left", "top"};
    if (positions.contains(value)) store_clipboard_group_bar_position(value);
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
