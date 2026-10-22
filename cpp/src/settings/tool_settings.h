#pragma once
#include <algorithm>
#include <optional>
#include <QPoint>
#include <vector>
#include <QObject>
#include <QSet>
#include <QSettings>
#include <QMap>
#include <QVariant>
#include <QColor>
#include <QJsonObject>

namespace jietuba {
class ToolSettings {
public:
    ToolSettings() = default;
    ToolSettings(QString id, QVariantMap defaults) : tool_id(std::move(id)), defaults(std::move(defaults)), current_(this->defaults) {}
    QString tool_id;
    QVariantMap defaults;
    QVariant get(const QString &key, const QVariant &fallback = {}) const;
    void set(const QString &key, const QVariant &value) { current_[key] = value; }
    void update(const QVariantMap &values);
    void reset_to_defaults() { current_ = defaults; }
    QVariantMap to_dict() const { return current_; }
    void from_dict(const QVariantMap &values);
private:
    QVariantMap current_;
};

// The QSettings a manager reads and writes.
class SettingsStore {
public:
    explicit SettingsStore(QSettings &settings) : settings_(&settings) {}
    QVariant value(const QString &key, const QVariant &fallback = {}) const { return settings_->value(key, fallback); }
    bool contains(const QString &key) const { return settings_->contains(key); }
    void setValue(const QString &key, const QVariant &value) const { settings_->setValue(key, stored_int(value)); }
    void sync() const { settings_->sync(); }
    // Python ints reach QSettings as int when they fit 32 bits, which the registry stores as REG_DWORD; integers
    // held as qint64 here would become REG_QWORD in the settings both versions share.
    static QVariant stored_int(const QVariant &value);
    QSettings &settings() const { return *settings_; }
private:
    QSettings *settings_;
};

// Whether settings export carries a setting; see app_settings.inc.
enum class SettingScope { Export, Local, Internal };
struct AppSetting {
    const char *name;
    const char *path;
    SettingScope scope;
};
// The rows of app_settings.inc, in table order.
const std::vector<AppSetting> &APP_SETTINGS();

namespace setting_detail {
template <typename T> T cast(const QVariant &value);
template <> inline QString cast<QString>(const QVariant &value) { return value.toString(); }
template <> inline bool cast<bool>(const QVariant &value) { return value.toBool(); }
template <> inline int cast<int>(const QVariant &value) { return value.toInt(); }
template <> inline double cast<double>(const QVariant &value) { return value.toDouble(); }
template <> inline QStringList cast<QStringList>(const QVariant &value) { return value.toStringList(); }
template <> inline QVariantList cast<QVariantList>(const QVariant &value) { return value.toList(); }
}

// Explicit QSettings injection avoids reading/writing Python's live registry.
class ToolSettingsManager : public QObject {
    Q_OBJECT
public:
    explicit ToolSettingsManager(QSettings &settings, QObject *parent = nullptr);
    SettingsStore qsettings;
    const QVariantMap &app_defaults() const { return app_defaults_; }
    const QVariantMap &tool_defaults() const { return tool_defaults_; }
    ToolSettings *get_tool_settings(const QString &tool);
    QVariant get_setting(const QString &tool, const QString &key, const QVariant &fallback = {}) const;
    void set_setting(const QString &tool, const QString &key, const QVariant &value, bool save_immediately = true);
    void update_settings(const QString &tool, const QVariantMap &values, bool save_immediately = true);
    void save_all();
    void reset_tool(const QString &tool, bool save_immediately = true);
    void reset_all();
    // QSettings key of an app setting: its path in app_settings.inc. A name outside the table
    // takes pin/ for a pin_ prefix, else app/.
    static QString app_setting_path(const QString &key);
    // By name, for settings picked at run time (a toolbar profile, a mouse action); fixed
    // settings use their typed accessors.
    QVariant get_app_setting(const QString &key, QVariant fallback = {}) const;
    void set_app_setting(const QString &key, const QVariant &value);
    // get_<name>() / set_<name>(value) of the table, looked up by accessor name for the fields
    // translation providers declare by name; nullopt / false when there is none. A generated
    // setter stores the value as given.
    std::optional<QVariant> accessor_value(const QString &name) const;
    bool set_accessor_value(const QString &name, const QVariant &value);
    QString get_inapp_shortcut(const QString &key) const;
    void set_inapp_shortcut(const QString &key, const QString &value);
    // Every slot of a global hotkey function (see GLOBAL_HOTKEY_FUNCTIONS); empty means unset.
    QStringList get_global_hotkeys(const QString &function) const;
    // Saves every slot of a function; throws std::invalid_argument unless there are GLOBAL_HOTKEY_SLOTS values.
    void set_global_hotkeys(const QString &function, const QStringList &values);
    QColor get_color(const QString &tool) const { return QColor(get_setting(tool, "color", "#FF0000").toString()); }
    void set_color(const QString &tool, QColor color, bool save = true) { set_setting(tool, "color", color.name(), save); }
    // Stored widths may be fractional once a tool clamps them, so no int conversion.
    double get_stroke_width(const QString &tool) const { return get_setting(tool, "stroke_width", 3).toDouble(); }
    void set_stroke_width(const QString &tool, int width, bool save = true) { set_setting(tool, "stroke_width", width, save); }
    double get_opacity(const QString &tool) const { return get_setting(tool, "opacity", 1.0).toDouble(); }
    void set_opacity(const QString &tool, double value, bool save = true) { set_setting(tool, "opacity", value, save); }
    int get_font_size(const QString &tool) const { return get_setting(tool, "font_size", 14).toInt(); }
    void set_font_size(const QString &tool, int value, bool save = true) { set_setting(tool, "font_size", value, save); }
    QVariantMap export_settings() const;
    void import_settings(const QVariantMap &values);
    bool is_first_run() const { return !get_has_run_before(); }
    void mark_as_run();
    static bool is_absolute_save_path(const QString &value);
    QSettings &settings() { return qsettings.settings(); }

    // The accessors of app_settings.inc. A generated getter converts the stored value to the
    // row's type, falling back to the default or to the value given. Hand-written (Own) ones
    // get stored_<name>() and default_<name>() for a getter, store_<name>() for a setter.
#define JIETUBA_GETTER_Gen(accessor, name, path, Type) \
    Type get_##accessor() const { return setting_detail::cast<Type>(qsettings.value(QStringLiteral(path), app_defaults_.value(QStringLiteral(name)))); } \
    Type get_##accessor(const Type &fallback) const { return setting_detail::cast<Type>(qsettings.value(QStringLiteral(path), QVariant::fromValue(fallback))); }
#define JIETUBA_GETTER_Own(accessor, name, path, Type) \
private: \
    QVariant stored_##accessor() const { return qsettings.value(QStringLiteral(path), app_defaults_.value(QStringLiteral(name))); } \
    QVariant default_##accessor() const { return app_defaults_.value(QStringLiteral(name)); } \
public:
#define JIETUBA_SETTER_Gen(accessor, path, Type) \
    void set_##accessor(const Type &value) { qsettings.setValue(QStringLiteral(path), QVariant::fromValue(value)); }
#define JIETUBA_SETTER_Trim(accessor, path, Type) \
    void set_##accessor(const QString &value) { qsettings.setValue(QStringLiteral(path), value.trimmed()); }
#define JIETUBA_SETTER_Own(accessor, path, Type) \
private: \
    void store_##accessor(const QVariant &value) const { qsettings.setValue(QStringLiteral(path), value); } \
public:
#define JIETUBA_SETTER_None(accessor, path, Type)
#define SETTING_AS(accessor, name, path, Type, initial, scope, getter, setter) \
    JIETUBA_GETTER_##getter(accessor, #name, path, Type) JIETUBA_SETTER_##setter(accessor, path, Type)
#define SETTING(name, path, Type, initial, scope, getter, setter) SETTING_AS(name, name, path, Type, initial, scope, getter, setter)
#include "settings/app_settings.inc"
#undef SETTING
#undef SETTING_AS
#undef JIETUBA_GETTER_Gen
#undef JIETUBA_GETTER_Own
#undef JIETUBA_SETTER_Gen
#undef JIETUBA_SETTER_Trim
#undef JIETUBA_SETTER_Own
#undef JIETUBA_SETTER_None

    // The Own accessors of the table.
    // Repaired to the default when not an absolute path.
    QString get_screenshot_save_path() const;
    void set_screenshot_save_path(const QString &value);
    QString get_screenshot_watermark_time_format() const;
    // auto / mss / hdr; anything else reads as the default.
    QString get_capture_engine() const;
    void set_capture_engine(const QString &value);
    // "off" when smart selection is switched off, else the stored level.
    QString get_smart_selection_mode() const;
    void set_smart_selection_mode(const QString &mode);
    void set_screenshot_format(const QString &value) { store_screenshot_format(value.toUpper()); }
    void set_screenshot_quality(int value) { store_screenshot_quality(std::max(1, std::min(100, value))); }
    QString get_ocr_engine() const;
    void set_ocr_engine(const QString &value);
    // One of SHIFT_LINE_ANGLES; anything else reads as the default.
    int get_shift_line_angle() const;
    void set_shift_line_angle(int value);
    QString get_ocr_result_position() const;
    void set_ocr_result_position(const QString &value);
    QString get_ocr_layout_mode() const;
    void set_ocr_layout_mode(const QString &value);
    QString get_ocr_punctuation() const;
    void set_ocr_punctuation(const QString &value);
    void set_pin_default_opacity(double opacity) { store_pin_default_opacity(std::max(0.1, std::min(1.0, opacity))); }
    void set_translation_provider(const QString &provider_id);
    void set_amazon_translate_region(const QString &value);
    QString get_volcengine_translate_quota_limit() const { return stored_volcengine_translate_quota_limit().toString().trimmed(); }
    QString get_tencent_translate_quota_limit() const { return stored_tencent_translate_quota_limit().toString().trimmed(); }
    // The saved target language, else the system language's.
    QString get_translation_target_lang() const;
    // The saved target language; empty for the automatic choice.
    QString get_saved_translation_target_lang() const { return stored_translation_target_lang().toString(); }
    void set_clipboard_history_limit(int value) { store_clipboard_history_limit(std::max(0, value)); }
    int get_clipboard_display_lines() const { return std::clamp(stored_clipboard_display_lines().toInt(), 1, 5); }
    void set_clipboard_display_lines(int value) { store_clipboard_display_lines(std::clamp(value, 1, 5)); }
    QString get_clipboard_image_size() const;
    void set_clipboard_image_size(const QString &value);
    void set_clipboard_line_height_padding(int value) { store_clipboard_line_height_padding(std::max(0, value)); }
    QString get_clipboard_multi_paste_order() const;
    void set_clipboard_multi_paste_order(const QString &value);
    QString get_clipboard_group_bar_position() const;
    void set_clipboard_group_bar_position(const QString &value);
    // Choices offered by the settings pages; never stored.
    QVariant get_gif_fps_options() const { return default_gif_fps_options(); }
    QVariant get_clipboard_window_opacity_options() const { return default_clipboard_window_opacity_options(); }
    QVariant get_clipboard_font_size_options() const { return default_clipboard_font_size_options(); }

    // Centre where the result window was last closed; nullopt without a valid record.
    std::optional<QPoint> get_ocr_result_last_center() const;
    void set_ocr_result_last_center(int x, int y) { qsettings.setValue("app/ocr_result_last_center", QString("%1,%2").arg(x).arg(y)); }
    // Settings of one translation provider; empty for an unknown provider.
    QVariantMap get_translation_provider_config(const QString &provider_id) const;
    // Whether the provider's guard is on; true for a provider without one, which the
    // service never asks.
    bool get_translation_quota_enabled(const QString &provider_id) const;
    // The custom monthly limit; 0 when unset, not a number or the provider has none.
    qint64 get_translation_quota_limit(const QString &provider_id) const;
    // Characters used in the current calendar month; a new month starts again from 0.
    qint64 get_translation_usage_chars(const QString &provider_id) const;
    void add_translation_usage_chars(const QString &provider_id, qint64 chars);
    // All translation parameters in one place.
    QVariantMap get_translation_params() const;
    // Parameters independent of the provider.
    QVariantMap get_translation_request_params() const;
    // First run always shows the settings window; afterwards the user's setting decides.
    bool should_show_main_window_on_start();
    inline static const QStringList CLIPBOARD_IMAGE_SIZES = {"small", "medium", "large"};
    inline static const QStringList CLIPBOARD_MULTI_PASTE_ORDERS = {"oldest_first", "newest_first"};
signals:
    void settings_changed(QString tool, QVariantMap settings);
private:
    void _load_tool_settings(ToolSettings &tool);
    void _save_tool_settings(const ToolSettings &tool);
    QVariant typed_value(const QString &path, const QVariant &fallback) const;
    // Until the third clipboard slot is saved it follows the old switch; nothing is written.
    void migrate_win_v_slot(QStringList &values) const;
    QVariantMap app_defaults_, tool_defaults_;
    QStringList tool_order_;
    QMap<QString, ToolSettings> tools_;
};

// Global hotkeys: function, the default-table name of slot 1 and its QSettings key. Slot n
// (n >= 2) adds _n to both names; slot 1 keeps the old unnumbered names.
constexpr int GLOBAL_HOTKEY_SLOTS = 4;
struct GlobalHotkeyFunction {
    const char *function;
    const char *default_name;
    const char *path;
};
const std::vector<GlobalHotkeyFunction> &GLOBAL_HOTKEY_FUNCTIONS();
// The old "open the clipboard with Win+V" switch became the clipboard's third slot.
constexpr int WIN_V_CLIPBOARD_SLOT = 2;
QString global_hotkey_slot_suffix(int slot);

// Organisation/application of the default QSettings store, shared with the Python version.
inline const char *TOOL_SETTINGS_ORGANIZATION = "Jietuba";
inline const char *TOOL_SETTINGS_APPLICATION = "ToolSettings";
// Global manager; qsettings (for isolated tests) only applies on first creation.
ToolSettingsManager *get_tool_settings_manager(QSettings *qsettings = nullptr);

// The smart selection levels in settings-page order. "off" is the master switch and
// never stored as smart_selection_mode, so the stored values start at the second.
inline const QStringList SMART_SELECTION_MODES = {"off", "window", "element"};
inline const QStringList STORED_SMART_SELECTION_MODES = {"window", "element"};

// (key, label, default binding, kind) of a mouse action.
struct MouseAction {
    const char *key;
    const char *label;
    const char *binding;
    const char *kind;
};
const std::vector<MouseAction> &CAPTURE_MOUSE_ACTIONS();
const std::vector<MouseAction> &PIN_MOUSE_ACTIONS();
const std::vector<MouseAction> &CLIPBOARD_MOUSE_ACTIONS();
// Global mouse shortcuts: drag with modifiers held, written like in-app mouse shortcuts ("win+dragleft").
const std::vector<MouseAction> &QUICK_CAPTURE_ACTIONS();
// The order global hotkey recording joins them: Ctrl, Shift, Alt, Win.
inline const QStringList QUICK_CAPTURE_MODIFIERS = {"ctrl", "shift", "alt", "win"};
inline const QStringList QUICK_CAPTURE_BUTTONS = {"left", "middle", "right", "x1", "x2"};
QString get_quick_capture_binding(const ToolSettingsManager *config, const QString &action);
// ((sorted modifier set, mouse button), action). One or two modifiers; combinations repeating an earlier action are dropped.
std::vector<std::pair<std::pair<QStringList, QString>, QString>> get_quick_capture_bindings(const ToolSettingsManager *config);
QString get_clipboard_mouse_binding(const ToolSettingsManager *config, const QString &action);
QString get_pin_mouse_binding(const ToolSettingsManager *config, const QString &action);
// One mouse gesture for a capture action. The old double-click switch remains a
// migration fallback only; mouse bindings never alter keyboard shortcuts.
QString get_capture_mouse_binding(const ToolSettingsManager *config, const QString &action);

// Capture engines in settings-page order: auto uses HDR while a display has HDR on and
// falls back to mss; mss / hdr use only that one, never falling back.
inline const QStringList CAPTURE_ENGINE_CHOICES = {"auto", "mss", "hdr"};
// OCR engines in settings-page order: auto prefers the system OCR, else PP-OCR; a named one never falls back.
inline const QStringList OCR_ENGINES = {"auto", "oneocr", "ppocr_rust"};
// Where the text recognition window opens, in settings-page order.
inline const QStringList OCR_RESULT_POSITIONS = {"region_screen", "primary_screen", "beside_region", "near_cursor", "last_position"};
// Result layout; values match text_recognition.layout.
inline const QStringList OCR_LAYOUT_MODES = {"auto", "single_line", "multiline"};
inline const QStringList OCR_PUNCTUATION_MODES = {"keep", "halfwidth", "fullwidth"};
// Degrees between the directions a Shift-held pen or highlighter line snaps to, in settings-page
// order; 0 is any angle.
inline const QList<int> SHIFT_LINE_ANGLES = {90, 45, 30, 15, 0};
// The registry default, for tools that run without a settings manager.
inline constexpr int DEFAULT_SHIFT_LINE_ANGLE = 45;

// (config key, tool id, label, default key) of an annotation tool shortcut.
struct ToolShortcut {
    const char *config_key;
    const char *tool_id;
    const char *label;
    const char *default_key;
};
const std::vector<ToolShortcut> &ANNOTATION_TOOL_SHORTCUTS();

// The clipboard window's direct paste keys, in list order.
QString clipboard_pick_keys(const QString &mode);

// Save slots of the custom OpenAI-compatible service; the first keeps the single-slot name.
inline const QStringList CUSTOM_LLM_PROVIDER_IDS = {"custom_llm", "custom_llm_2", "custom_llm_3"};
inline const QStringList CUSTOM_LLM_TEXT_KEYS = {"name", "base_url", "api_key", "model", "temperature", "timeout", "instructions", "extra_body"};
}
