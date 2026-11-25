#include "clipboard/ui/group_kind_style.h"
#include "clipboard/core/models.h"
#include <QByteArray>
#include <QCoreApplication>
#include <QHash>
#include <QPainter>
#include <QSvgRenderer>

namespace jietuba::clipboard {
const QString &KIND_NORMAL() {
    static const QString kind = "normal";
    return kind;
}

const QString &KIND_FILE() {
    static const QString kind = "file";
    return kind;
}

const QString &KIND_HIDDEN() {
    static const QString kind = "hidden";
    return kind;
}

const QString &KIND_FILTER() {
    static const QString kind = "filter";
    return kind;
}

namespace {
// Line drawings on a 24x24 canvas.
const char *icon_of(const QString &kind) {
    if (kind == KIND_FILE()) return R"(<path d="M13 2 4 14h7l-1 8 9-12h-7z"/>)";
    if (kind == KIND_HIDDEN())
        return R"(<path d="M3 3l18 18"/><path d="M10.6 6.1A10 10 0 0 1 12 6c5 0 9 6 9 6a15 15 0 0 1-3.2 3.7M6.6 6.7C4 8.5 3 12 3 12s4 6 9 6c1.4 0 2.7-.3 3.9-.8"/><path d="M9.9 9.9a3 3 0 0 0 4.2 4.2"/>)";
    if (kind == KIND_FILTER()) return R"(<path d="M3 5h18l-7 8v6l-4 2v-8z"/>)";
    return R"(<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>)";
}

// Owned by the application, so they go with it and not at static destruction after it.
QSvgRenderer &renderer_for(const QString &kind, const QString &color_name) {
    static QHash<QString, QSvgRenderer *> cache;
    auto &entry = cache[kind + color_name];
    if (!entry) {
        const QString svg = QString(R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="%1" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">%2</svg>)")
                                .arg(color_name, QString::fromUtf8(icon_of(kind)));
        entry = new QSvgRenderer(svg.toUtf8(), QCoreApplication::instance());
    }
    return *entry;
}
}

QString group_kind(int group_type) {
    if (group_type == GroupType::FILE) return KIND_FILE();
    if (group_type == GroupType::HIDDEN) return KIND_HIDDEN();
    if (group_type == GroupType::FILTER) return KIND_FILTER();
    return KIND_NORMAL();
}

void paint_kind_icon(QPainter *painter, const QRectF &rect, const QString &kind, const QColor &color) {
    renderer_for(kind, color.name()).render(painter, rect);
}
}
