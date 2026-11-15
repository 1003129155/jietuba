#pragma once
#include <QDateTime>
#include <QStringList>

namespace jietuba {
class ToolSettingsManager;
namespace file_names {
enum Kind { Screenshot, Pin, LongScreenshot, Gif, Video, OriginalSuffix };
QStringList defaults();
QStringList load(ToolSettingsManager &config);
QString problem(const QString &pattern, Kind kind);
QString render(const QString &pattern, const QString &prefix = {}, const QDateTime &time = QDateTime::currentDateTime());
QString stem(ToolSettingsManager &config, Kind kind, const QString &prefix = {});
}
}
