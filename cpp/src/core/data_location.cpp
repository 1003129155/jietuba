#include "core/data_location.h"
#include "core/constants.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <iterator>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace jietuba {
namespace {
const QString SETTINGS_FILE_NAME = "settings.ini";

// Settings are read before QApplication exists, when applicationFilePath() is still empty.
QString program_file() {
#ifdef Q_OS_WIN
    wchar_t path[MAX_PATH * 4] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return QDir::fromNativeSeparators(QString::fromWCharArray(path, static_cast<int>(length)));
#else
    return QCoreApplication::applicationFilePath();
#endif
}
}

QString DataLocation::settings_file() const { return QDir(dir).filePath(SETTINGS_FILE_NAME); }

DataLocation resolve_data_location(const QString &program_dir, const QString &roaming_dir) {
    if (!program_dir.isEmpty() && QFileInfo(QDir(program_dir).filePath(SETTINGS_FILE_NAME)).isFile())
        return {QDir::cleanPath(QDir::fromNativeSeparators(program_dir)), true};
    return {QDir(QDir::fromNativeSeparators(roaming_dir)).filePath("Jietuba"), false};
}

const DataLocation &data_location() {
    static const DataLocation location =
        resolve_data_location(test_sandbox_dir().isEmpty() ? QFileInfo(program_file()).absolutePath() : QString(), roaming_app_data_dir());
    return location;
}

QString roaming_app_data_dir() {
    const QString base = qEnvironmentVariable("APPDATA");
    return base.isEmpty() ? QDir(QDir::homePath()).filePath("AppData/Roaming") : QDir::fromNativeSeparators(base);
}
}
