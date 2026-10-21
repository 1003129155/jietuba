#include "clipboard/ui/group_kind_style.h"
#include "clipboard/core/models.h"
#include "core/ui_theme.h"
#include <QByteArray>
#include <QCoreApplication>
#include <QHash>
#include <QPainter>
#include <QSvgRenderer>
#include <algorithm>

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
struct KindStyle {
    const char *color;
    const char *light_text;
    const char *dark_text;
    const char *icon;
};

// Icons are line drawings on a 24x24 canvas.
const KindStyle &style_of(const QString &kind) {
    static const KindStyle normal = {"#C58A1B", "#8A5F0C", "#EBC066", R"(<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>)"};
    static const KindStyle file = {"#E0743A", "#B4551F", "#F0A27A", R"(<path d="M13 2 4 14h7l-1 8 9-12h-7z"/>)"};
    static const KindStyle hidden = {"#8A7CC9", "#6B5BB5", "#B9B0E8",
                                     R"(<path d="M3 3l18 18"/><path d="M10.6 6.1A10 10 0 0 1 12 6c5 0 9 6 9 6a15 15 0 0 1-3.2 3.7M6.6 6.7C4 8.5 3 12 3 12s4 6 9 6c1.4 0 2.7-.3 3.9-.8"/><path d="M9.9 9.9a3 3 0 0 0 4.2 4.2"/>)"};
    static const KindStyle filter = {"#3A9A8A", "#1F7A6C", "#7FD1C3", R"(<path d="M3 5h18l-7 8v6l-4 2v-8z"/>)"};
    if (kind == KIND_FILE()) return file;
    if (kind == KIND_HIDDEN()) return hidden;
    if (kind == KIND_FILTER()) return filter;
    return normal;
}

// Owned by the application, so they go with it and not at static destruction after it.
QSvgRenderer &renderer_for(const QString &kind, const QString &color_name) {
    static QHash<QString, QSvgRenderer *> cache;
    auto &entry = cache[kind + color_name];
    if (!entry) {
        const QString svg = QString(R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="%1" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">%2</svg>)")
                                .arg(color_name, QString::fromUtf8(style_of(kind).icon));
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

QColor kind_color(const QString &kind) { return QColor(QString::fromLatin1(style_of(kind).color)); }

QColor kind_tint(const QString &kind, double alpha, const UIThemeTokens &tokens) {
    QColor color = kind_color(kind);
    color.setAlphaF(std::min(1.0, alpha + (tokens.is_dark() ? 0.08 : 0.0)));
    return color;
}

QColor kind_text_color(const QString &kind, const UIThemeTokens &tokens) {
    const KindStyle &style = style_of(kind);
    return QColor(QString::fromLatin1(tokens.is_dark() ? style.dark_text : style.light_text));
}

void paint_kind_icon(QPainter *painter, const QRectF &rect, const QString &kind, const QColor &color) {
    renderer_for(kind, color.name()).render(painter, rect);
}
}
