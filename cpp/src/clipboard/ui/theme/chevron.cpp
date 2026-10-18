#include "clipboard/ui/theme/chevron.h"
#include "core/constants.h"
#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QPen>
#include <QPointF>

namespace jietuba::clipboard {
namespace {
bool render(const QString &path, const QString &color, int width, int height, int ratio) {
    QImage image(width * ratio, height * ratio, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(color), 1.5 * ratio, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const double margin = 1.5 * ratio;
    const QPointF points[] = {{margin, margin}, {width * ratio / 2.0, height * ratio - margin}, {width * ratio - margin, margin}};
    painter.drawPolyline(points, 3);
    painter.end();
    return image.save(path);
}
}

QString chevron_image(const QString &color, int width, int height) {
    // The border-triangle trick draws a solid rectangle on sub-controls, so it cannot be used.
    QString name = color;
    if (name.startsWith(u'#')) name.remove(0, 1);
    const QString stem = QString("chevron_%1_%2x%3").arg(name.toLower()).arg(width).arg(height);
    const QDir folder(QDir(get_app_data_dir()).filePath("Cache/ui"));
    const QString path = folder.filePath(stem + ".png");
    if (!QFile::exists(path)) {
        // The @2x copy is picked by Qt on high-DPI screens.
        if (!QDir().mkpath(folder.path()) || !render(folder.filePath(stem + "@2x.png"), color, width, height, 2) || !render(path, color, width, height, 1)) return {};
    }
    return path;
}
}
