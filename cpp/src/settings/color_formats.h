#pragma once
#include <QColor>
#include <QVector>
#include <QString>
#include <QJsonValue>
namespace jietuba {
class ToolSettingsManager;
namespace color_formats {
struct ColorFormat {
    QString name, template_string;
    bool enabled = false;
    QString render(const QColor &color) const;
};
QVector<ColorFormat> default_formats();
QVector<ColorFormat> normalize(const QVector<ColorFormat> &formats);
QVector<ColorFormat> enabled_formats(const QVector<ColorFormat> &formats);
QVector<ColorFormat> load(const ToolSettingsManager *config);
QVector<ColorFormat> migrate_from_legacy(const ToolSettingsManager &config);
void save(ToolSettingsManager *config, const QVector<ColorFormat> &formats);
}
}
