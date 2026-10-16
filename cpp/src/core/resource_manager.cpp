#include "core/resource_manager.h"
#include "core/logger.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QPainter>
#include <QPixmap>
#include <QSet>
#include <QSvgRenderer>
#include <algorithm>
#include <memory>
#include <set>

namespace jietuba {
namespace {
// Box sizes rasterised up front; see get_icon.
constexpr int RASTER_BOX_SIZES[] = {16, 24, 32, 48, 64, 128};

std::unique_ptr<QSvgRenderer> svg_renderer(const QString &svg_path, QHash<QString, QByteArray> &sources) {
    auto found = sources.constFind(svg_path);
    QByteArray data;
    if (found == sources.constEnd()) {
        QFile file(svg_path);
        if (!file.open(QIODevice::ReadOnly)) return nullptr;
        data = file.readAll();
        sources.insert(svg_path, data);
    } else {
        data = *found;
    }
    auto renderer = std::make_unique<QSvgRenderer>(data);
    return renderer->isValid() ? std::move(renderer) : nullptr;
}
}

QHash<QString, QIcon> &ResourceManager::icon_cache() {
    static QHash<QString, QIcon> cache;
    return cache;
}

QHash<QString, QByteArray> &ResourceManager::svg_sources() {
    static QHash<QString, QByteArray> sources;
    return sources;
}

QString ResourceManager::get_resource_path(const QString &relative_path) {
    const auto normalized = QDir::fromNativeSeparators(relative_path);
    const auto embedded = ":/" + normalized;
    if (QFile::exists(embedded)) return embedded;
    return QDir(QCoreApplication::applicationDirPath()).filePath(normalized);
}

QString ResourceManager::get_icon_path(const QString &icon_name) { return get_resource_path("svg/" + icon_name); }

QIcon ResourceManager::rasterize(const QString &svg_path, int size, int fit) {
    const auto renderer = svg_renderer(svg_path, svg_sources());
    if (!renderer) return {};
    auto render_to = [&](const QSize &target) {
        QPixmap pixmap(target);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        renderer->render(&painter);
        painter.end();
        return pixmap;
    };
    QIcon icon;
    if (size > 0) {
        icon.addPixmap(render_to(QSize(size, size)));
        return icon;
    }
    auto default_size = renderer->defaultSize();
    if (default_size.isEmpty()) default_size = QSize(64, 64);
    std::set<int> boxes(std::begin(RASTER_BOX_SIZES), std::end(RASTER_BOX_SIZES));
    if (fit > 0) boxes.insert(fit);
    QSet<QPair<int, int>> seen;
    for (const int box : boxes) {
        // Keep the aspect ratio: some icons are 128x32 or 10x6 and would be distorted on a square canvas.
        const auto target = default_size.scaled(box, box, Qt::KeepAspectRatio);
        if (target.isEmpty()) continue;
        const QPair<int, int> key(target.width(), target.height());
        if (seen.contains(key)) continue;
        seen.insert(key);
        icon.addPixmap(render_to(target));
    }
    return icon;
}

QIcon ResourceManager::get_icon(const QString &svg_path, int size, int fit) {
    if (size > 0 || std::find(std::begin(RASTER_BOX_SIZES), std::end(RASTER_BOX_SIZES), fit) != std::end(RASTER_BOX_SIZES)) fit = 0;
    const auto cache_key = QString("%1|%2|%3").arg(svg_path).arg(size).arg(fit);
    if (const auto found = icon_cache().constFind(cache_key); found != icon_cache().constEnd()) return *found;
    const auto icon = rasterize(svg_path, size, fit);
    if (icon.isNull()) {
        // Not cached, so a transient failure is not made permanent.
        log_warning(T("图标渲染失败，本次返回空图标: {svg_path}", {{"svg_path", svg_path}}), "Resource");
        return QIcon();
    }
    icon_cache().insert(cache_key, icon);
    return icon;
}

QIcon ResourceManager::get_icon_by_name(const QString &icon_name, int size) { return get_icon(get_icon_path(icon_name), size); }
}
