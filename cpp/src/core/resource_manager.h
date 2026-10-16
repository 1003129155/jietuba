#pragma once
#include <QHash>
#include <QIcon>
#include <QString>
#include <tuple>

namespace jietuba {
class ResourceManager {
public:
    // Bundled files (svg/...) resolve to the embedded resource; anything else,
    // such as OCR models, to the directory of the executable.
    static QString get_resource_path(const QString &relative_path);
    static QString get_icon_path(const QString &icon_name);
    // Icons are rasterised once and cached, so they never depend on the source
    // file again. size > 0 renders a size x size square; otherwise a set of
    // common sizes keeps the aspect ratio, plus one extra box of side fit.
    static QIcon get_icon(const QString &svg_path, int size = 0, int fit = 0);
    static QIcon get_icon_by_name(const QString &icon_name, int size = 0);
private:
    static QIcon rasterize(const QString &svg_path, int size, int fit);
    static QHash<QString, QIcon> &icon_cache();
    static QHash<QString, QByteArray> &svg_sources();
};
}
