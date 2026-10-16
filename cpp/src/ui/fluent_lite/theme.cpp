#include "ui/fluent_lite/theme.h"
#include "core/resource_manager.h"
#include <QHash>
#include <QLinearGradient>
#include <QPainter>
#include <QPixmap>
#include <QWidget>

namespace jietuba {
const QStringList &FONT_FAMILIES() {
    static const QStringList families = [] {
        QStringList result;
        for (auto name : FONT_FAMILY.split(',')) result.append(name.trimmed().remove('"'));
        return result;
    }();
    return families;
}

const UIThemeTokens &ui_tokens(const QWidget *) { return get_ui_theme()->tokens(); }

QString scrollbar_qss(const QWidget *widget) {
    const auto &t = ui_tokens(widget);
    return QString(R"(
    QScrollBar:vertical { width: 7px; margin: 3px 0; background: transparent; }
    QScrollBar:horizontal { height: 7px; margin: 0 3px; background: transparent; }
    QScrollBar::handle:vertical { min-height: 30px; background: %1; border-radius: 3px; }
    QScrollBar::handle:horizontal { min-width: 30px; background: %1; border-radius: 3px; }
    QScrollBar::handle:hover { background: %2; }
    QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
    QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
    )").arg(t.border_hover, t.accent);
}

namespace {
QIcon tinted_from_path(const QString &path, const QString &color) {
    // Bounded like the reference's LRU cache of 128 entries.
    static QHash<QString, QIcon> cache;
    const auto key = path + '|' + color;
    if (const auto found = cache.constFind(key); found != cache.constEnd()) return *found;
    const auto source = ResourceManager::get_icon(path);
    const auto pixmap = source.pixmap(64, 64);
    if (pixmap.isNull()) return source;
    QPixmap tinted(pixmap.size());
    tinted.fill(Qt::transparent);
    QPainter painter(&tinted);
    painter.drawPixmap(0, 0, pixmap);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(tinted.rect(), QColor::fromString(color));
    painter.end();
    QIcon result(tinted);
    if (cache.size() >= 128) cache.clear();
    cache.insert(key, result);
    return result;
}

QString icon_path(const IconSource &icon) {
    if (const auto *fluent = std::get_if<FluentIcon>(&icon)) return fluent_icon_path(*fluent);
    if (const auto *path = std::get_if<QString>(&icon)) return *path;
    return {};
}
}

QIcon to_qicon(const IconSource &icon, const QWidget *widget) {
    if (const auto *qicon = std::get_if<QIcon>(&icon)) return *qicon;
    if (std::holds_alternative<std::monostate>(icon)) return QIcon();
    QString icon_id;
    QIcon source;
    if (const auto *fluent = std::get_if<FluentIcon>(&icon)) {
        icon_id = fluent_icon_value(*fluent);
        source = fluent_icon(*fluent);
    } else {
        icon_id = std::get<QString>(icon);
        source = ResourceManager::get_icon(icon_id);
    }
    const bool preserve_color = icon_id.toUpper().endsWith("WHITE");
    if (preserve_color || source.isNull()) return source;
    return tinted_from_path(icon_path(icon), ui_tokens(widget).text);
}

QColor css_color(const QString &value) {
    const auto text = value.trimmed();
    if (text.startsWith("rgba(") || text.startsWith("rgb(")) {
        const auto inner = text.mid(text.indexOf('(') + 1).chopped(1);
        const auto channels = inner.split(',');
        QColor color(channels.value(0).trimmed().toInt(), channels.value(1).trimmed().toInt(), channels.value(2).trimmed().toInt());
        if (channels.size() == 4) color.setAlphaF(channels[3].trimmed().toDouble());
        return color;
    }
    return QColor::fromString(text);
}

QIcon tinted_icon(const IconSource &icon, const QString &color) { return tinted_from_path(icon_path(icon), color); }

void paint_solid_background(QWidget *widget, int radius) {
    QPainter painter(widget);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(widget->rect(), Qt::transparent);
    const auto rect = QRectF(widget->rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QLinearGradient gradient(0, 0, 0, widget->height());
    const auto &tokens = ui_tokens(widget);
    gradient.setColorAt(0.0, QColor::fromString(tokens.window_top));
    gradient.setColorAt(1.0, QColor::fromString(tokens.window_bottom));
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setBrush(gradient);
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(rect, radius, radius);
    QPen pen = painter.pen();
    pen.setStyle(Qt::SolidLine);
    pen.setColor(QColor::fromString(tokens.window_border));
    pen.setWidthF(1.0);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(rect, radius, radius);
}
}
