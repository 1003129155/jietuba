#pragma once
#include <QString>
#include <QStringList>

namespace jietuba {
QString get_app_data_dir();
QString get_log_dir();
// The log folder is shared with the Python version (runtime_YYYYMMDD.log, crash.log,
// faulthandler.log); own names keep two running versions out of each other's files.
inline const QString RUNTIME_LOG_PREFIX = "runtime_cpp_";
inline const QString CRASH_LOG_NAME = "crash_cpp.log";
inline const QString FAULT_LOG_NAME = "faulthandler_cpp.log";
// Set only in the test executables (tests/test_sandbox.cpp). Defaults that would reach the
// user's settings, autostart entry or desktop go into this folder instead.
QString test_sandbox_dir();

// font-family values for QSS (most widgets) and for system-level UI (tray and context menus).
inline const QString CSS_FONT_FAMILY = R"("Microsoft YaHei", "SimSun", Arial, sans-serif)";
inline const QString CSS_FONT_FAMILY_UI = R"("Microsoft YaHei UI", "Segoe UI", sans-serif)";
inline const QString DEFAULT_FONT_FAMILY = "Microsoft YaHei";

// Fonts offered by the text tool; kept short so startup never enumerates the font database.
inline const QStringList COMMON_TEXT_FONTS = {
    "Microsoft YaHei UI", "SimSun", "Segoe UI", "Arial", "Yu Gothic UI", "Meiryo", "Microsoft JhengHei UI", "PMingLiU",
};

QString get_default_text_font_for_language(const QString *language_code = nullptr);
QString get_system_default_text_font_family();
QStringList get_available_text_fonts();
QString normalize_text_font_family(const QString &font_family, const QString *language_code = nullptr);

inline const QString PROJECT_GITHUB_URL = "https://github.com/1003129155/jietuba";
inline const QString PROJECT_RELEASES_LATEST_URL = PROJECT_GITHUB_URL + "/releases/latest";
inline const QString PROJECT_LATEST_RELEASE_API_URL = "https://api.github.com/repos/1003129155/jietuba/releases/latest";
}
