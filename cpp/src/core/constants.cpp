#include "core/constants.h"
#include "core/i18n.h"
#include "core/logger.h"
#include <QDir>
#include <QFontDatabase>
#include <QHash>

namespace jietuba {
QString get_app_data_dir() {
    // %LOCALAPPDATA% rather than home/AppData/Local: domain profiles may redirect the home
    // directory. Shared with the Python version, like the settings.
    const QString folder = "Jietuba";
    const auto base = qEnvironmentVariable("LOCALAPPDATA");
    if (!base.isEmpty()) return QDir(base).filePath(folder);
    return QDir(QDir::homePath()).filePath("AppData/Local/" + folder);
}

QString get_log_dir() { return QDir(get_app_data_dir()).filePath("Logs"); }

QString test_sandbox_dir() { return qEnvironmentVariable("JIETUBA_TEST_SANDBOX"); }

QString get_default_text_font_for_language(const QString *language_code) {
    static const QHash<QString, QString> by_language = {
        {"zh", "Microsoft YaHei UI"}, {"zh_CN", "Microsoft YaHei UI"}, {"zh_TW", "Microsoft JhengHei UI"},
        {"en", "Segoe UI"}, {"ja", "Yu Gothic UI"},
    };
    const QString code = language_code ? *language_code : I18nManager::get_current_language();
    const auto found = by_language.constFind(code);
    return found != by_language.constEnd() ? *found : get_system_default_text_font_family();
}

QString get_system_default_text_font_family() {
    static bool logged = false;
    auto family = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
    if (family.isEmpty()) family = COMMON_TEXT_FONTS.front();
    if (!logged) {
        log_debug(T("系统默认文字字体: {family}", {{"family", family}}), "Font");
        logged = true;
    }
    return family;
}

QStringList get_available_text_fonts() {
    auto fonts = COMMON_TEXT_FONTS;
    const auto system_font = get_system_default_text_font_family();
    if (!system_font.isEmpty() && !fonts.contains(system_font)) fonts.prepend(system_font);
    return fonts;
}

QString normalize_text_font_family(const QString &font_family, const QString *language_code) {
    if (get_available_text_fonts().contains(font_family)) return font_family;
    return get_default_text_font_for_language(language_code);
}
}
