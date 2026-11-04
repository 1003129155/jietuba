#include "ui/welcome/demo_paint.h"
#include "core/resource_manager.h"
#include "core/theme.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/theme.h"
#include <QFontMetricsF>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>

namespace jietuba::welcome_demo {
namespace {
constexpr double KEYCAP_EDGE = 2;
}

QColor with_alpha(const QString &color, int alpha) { return with_alpha(QColor(color), alpha); }

QColor with_alpha(QColor color, int alpha) {
    color.setAlpha(std::clamp(alpha, 0, 255));
    return color;
}

double phase(int now, int start, int end) { return end <= start ? (now >= end ? 1.0 : 0.0) : std::clamp((now - start) / double(end - start), 0.0, 1.0); }
double ease(QEasingCurve::Type curve, double t) { return QEasingCurve(curve).valueForProgress(t); }
QPointF lerp(const QPointF &from, const QPointF &to, double t) { return from + (to - from) * t; }

double demo_scale() { return get_dialog_scale().factor(); }

QFont pixel_font(int pixel_size, int weight) {
    QFont font;
    font.setPixelSize(std::max(1, pixel_size));
    font.setWeight(static_cast<QFont::Weight>(weight));
    return font;
}

QRectF window_rect(const QRectF &screen) { return screen.adjusted(screen.width() * 0.07, screen.height() * 0.1, -screen.width() * 0.07, -screen.height() * 0.08); }

QRectF chart_rect(const QRectF &screen) {
    const QRectF window = window_rect(screen);
    return {window.x() + window.width() * 0.08, window.y() + window.height() * 0.42, window.width() * 0.56, window.height() * 0.48};
}

QRectF chart_bar(const QRectF &chart, int index) {
    static constexpr double heights[] = {0.45, 0.7, 0.55, 0.85, 0.62};
    const double gap = chart.width() * 0.06;
    const double bar_w = (chart.width() - gap * 6) / 5;
    const double h = (chart.height() - 10) * heights[index];
    return {chart.x() + gap + index * (bar_w + gap), chart.bottom() - 5 - h, bar_w, h};
}

void draw_screen(QPainter &p, const WelcomeTheme &theme, const QRectF &screen) {
    p.setPen(QPen(QColor(theme.border), 1));
    p.setBrush(QColor(theme.panel_subtle));
    p.drawRoundedRect(screen, 10, 10);
    const QRectF window = window_rect(screen);
    p.setBrush(QColor(theme.panel));
    p.drawRoundedRect(window, 6, 6);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(theme.border));
    for (int i = 0; i < 3; ++i) p.drawEllipse(QPointF(window.x() + 10 + i * 8, window.y() + 9), 2.4, 2.4);
    p.setBrush(with_alpha(theme.text_soft, 70));
    p.drawRoundedRect(QRectF(window.x() + window.width() * 0.08, window.y() + window.height() * 0.2, window.width() * 0.62, 6), 3, 3);
    p.drawRoundedRect(QRectF(window.x() + window.width() * 0.08, window.y() + window.height() * 0.3, window.width() * 0.44, 6), 3, 3);
    const QRectF chart = chart_rect(screen);
    draw_chart(p, theme, chart);
    p.setBrush(with_alpha(theme.text_soft, 50));
    for (int i = 0; i < 4; ++i) p.drawRoundedRect(QRectF(chart.right() + 10, chart.y() + 4 + i * 13, window.right() - chart.right() - 20, 6), 3, 3);
}

void draw_chart(QPainter &p, const WelcomeTheme &theme, const QRectF &rect) {
    p.setPen(Qt::NoPen);
    p.setBrush(with_alpha(theme.accent, 22));
    p.drawRoundedRect(rect, 4, 4);
    const char *colors[] = {"#6F8FAB", "#6FA18F", "#8A7FA7", "#B18468", "#6F8FAB"};
    for (int i = 0; i < 5; ++i) {
        p.setBrush(QColor(colors[i]));
        p.drawRoundedRect(chart_bar(rect, i), 2, 2);
    }
}

void draw_cursor(QPainter &p, const QPointF &tip) {
    QPainterPath arrow(tip);
    arrow.lineTo(tip + QPointF(0, 15));
    arrow.lineTo(tip + QPointF(4, 11.5));
    arrow.lineTo(tip + QPointF(7, 17.5));
    arrow.lineTo(tip + QPointF(9.5, 16.3));
    arrow.lineTo(tip + QPointF(6.6, 10.5));
    arrow.lineTo(tip + QPointF(11, 10.5));
    arrow.closeSubpath();
    p.setPen(QPen(QColor("#FFFFFF"), 1.2));
    p.setBrush(QColor("#1B1F24"));
    p.drawPath(arrow);
}

QColor theme_color() { return get_theme().theme_color(); }

namespace {
// Icons are tinted with the theme's text colour, so a theme change starts a new set.
QIcon cached_icon(const QString &key, const IconSource &source) {
    static QHash<QString, QIcon> cache;
    static QString tint;
    if (const QString now = welcome_theme().text; now != tint) {
        cache.clear();
        tint = now;
    }
    auto it = cache.find(key);
    if (it == cache.end()) it = cache.insert(key, to_qicon(source));
    return *it;
}
}

QIcon demo_icon(const QString &svg_name) { return cached_icon(svg_name, ResourceManager::get_icon_path(svg_name + ".svg")); }
QIcon demo_icon(FluentIcon icon) { return cached_icon(QString("fluent:%1").arg(static_cast<int>(icon)), icon); }

QRectF chart_region(const QRectF &screen) { return chart_rect(screen).adjusted(-6, -6, 6, 6); }

void draw_shadow(QPainter &p, const WelcomeTheme &theme, const QRectF &box, double radius) {
    p.setPen(Qt::NoPen);
    for (int i = 4; i >= 1; --i) {
        p.setBrush(QColor(0, 0, 0, theme.is_dark ? 26 : 11));
        p.drawRoundedRect(box.adjusted(-i, -i + 2, i, i + 2), radius + i, radius + i);
    }
}

const QStringList &capture_tools() {
    static const QStringList tools = QString::fromUtf8("长截图 下载 翻译 gif 画笔 荧光笔 马赛克 箭头 序号 方框 圆框 文字 橡皮 撤回 复原 结束截图 钉图 确定").split(' ');
    return tools;
}

const QStringList &annotation_tools() {
    static const QStringList tools = [] {
        QStringList list = QString::fromUtf8("画笔 荧光笔 箭头 序号 方框 圆框 文字 橡皮 撤回 复原").split(' ');
        list << QString() << QString::fromUtf8("穿透 白板 光标高亮 清空 隐藏标注 结束截图").split(' ');
        return list;
    }();
    return tools;
}

namespace {
constexpr double TOOL_BUTTON = 15, TOOL_ICON = 9, TOOL_PAD = 3, GRIP_W = 7, TOOL_SEPARATOR = 6;
}

QRectF toolbar_size(const QStringList &tools) {
    double width = TOOL_PAD * 2 + GRIP_W;
    for (const auto &tool : tools) width += tool.isEmpty() ? TOOL_SEPARATOR : TOOL_BUTTON;
    return {0, 0, width, TOOL_BUTTON + TOOL_PAD * 2};
}

void draw_toolbar(QPainter &p, const WelcomeTheme &theme, const QRectF &bar, const QStringList &tools, int checked) {
    draw_shadow(p, theme, bar, bar.height() / 2);
    p.setPen(QPen(QColor(theme.border), 0.8));
    p.setBrush(QColor(theme.panel));
    p.drawRoundedRect(bar, bar.height() / 2, bar.height() / 2);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(theme.text_soft));
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 2; ++col) p.drawEllipse(QPointF(bar.x() + TOOL_PAD + 2 + col * 2.6, bar.center().y() - 2.6 + row * 2.6), 0.75, 0.75);
    double x = bar.x() + TOOL_PAD + GRIP_W;
    for (qsizetype i = 0; i < tools.size(); ++i) {
        if (tools[i].isEmpty()) {
            p.setPen(QPen(QColor(theme.separator), 1));
            p.drawLine(QPointF(x + TOOL_SEPARATOR / 2, bar.y() + 6), QPointF(x + TOOL_SEPARATOR / 2, bar.bottom() - 6));
            x += TOOL_SEPARATOR;
            continue;
        }
        const QRectF button(x, bar.y() + TOOL_PAD, TOOL_BUTTON, TOOL_BUTTON);
        if (i == checked) {
            QColor fill = theme_color();
            fill.setAlphaF(0.3f);
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            p.drawRoundedRect(button.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
        }
        const double inset = (TOOL_BUTTON - TOOL_ICON) / 2;
        demo_icon(tools[i]).paint(&p, button.adjusted(inset, inset, -inset, -inset).toRect());
        x += TOOL_BUTTON;
    }
}

namespace {
void draw_size_label(QPainter &p, const QRectF &region) {
    const QString position = QString("%1,%2").arg(qRound(region.x() * 4)).arg(qRound(region.y() * 4));
    const QString size = QString("%1 × %2 px").arg(qRound(region.width() * 4)).arg(qRound(region.height() * 4));
    const QFont font = pixel_font(8, QFont::DemiBold);
    const QFontMetricsF metrics(font);
    const QRectF chip(region.right() + 4, region.top() - 2, metrics.horizontalAdvance(position) + metrics.horizontalAdvance(size) + 18, 14);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(30, 34, 40, 225));
    p.drawRoundedRect(chip, 5, 5);
    p.setFont(font);
    p.setPen(theme_color());
    p.drawText(chip.adjusted(6, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, position);
    p.setPen(QColor("#FFFFFF"));
    p.drawText(chip.adjusted(6 + metrics.horizontalAdvance(position) + 5, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, size);
}
double move(int t, int start, int end) { return ease(QEasingCurve::InOutCubic, phase(t, start, end)); }
// Where the capture toolbar sits once it is up.
QRectF capture_toolbar_rect(const QRectF &screen) {
    const QRectF region = chart_region(screen);
    QRectF bar = toolbar_size(capture_tools());
    bar.moveTo(std::clamp(region.right() - bar.width(), screen.left() + 4, screen.right() - 4 - bar.width()), std::min(region.bottom() + 8, screen.bottom() - 4 - bar.height()));
    return bar;
}
}


void draw_capture(QPainter &p, const WelcomeTheme &theme, const QRectF &screen, int t, int checked) {
    if (t < 0) return;
    const QRectF region = chart_region(screen);
    const QPointF rest(screen.x() + screen.width() * 0.82, screen.y() + screen.height() * 0.32);
    constexpr int drag_start = 200, drag_end = 800;
    const QPointF corner = lerp(region.topLeft(), region.bottomRight(), move(t, drag_start, drag_end));
    const QRectF selection = QRectF(region.topLeft(), corner).normalized();
    QPainterPath shade;
    shade.addRect(screen);
    if (t >= drag_start) shade.addRect(selection);
    shade.setFillRule(Qt::OddEvenFill);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, static_cast<int>(phase(t, 0, 200) * 110)));
    p.drawPath(shade);
    if (t >= drag_start) {
        p.setPen(QPen(theme_color(), 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(selection);
    }
    QPointF cursor = t < drag_start ? lerp(rest, region.topLeft(), move(t, 0, drag_start - 30)) : corner;
    if (t >= drag_end) {
        // The eight handles, then the position and size label off the top right corner.
        p.setPen(Qt::NoPen);
        p.setBrush(theme_color());
        for (const QPointF &c : {region.topLeft(), region.topRight(), region.bottomLeft(), region.bottomRight(), QPointF(region.center().x(), region.top()),
                                 QPointF(region.center().x(), region.bottom()), QPointF(region.left(), region.center().y()), QPointF(region.right(), region.center().y())})
            p.drawEllipse(c, 2.8, 2.8);
        const double in = ease(QEasingCurve::OutCubic, phase(t, drag_end + 50, drag_end + 300));
        p.save();
        p.setOpacity(p.opacity() * in);
        draw_size_label(p, region);
        draw_toolbar(p, theme, capture_toolbar_rect(screen).translated(0, 6 * (1 - in)), capture_tools(), checked);
        p.restore();
        // A lit button was clicked: the pointer goes to it.
        if (checked >= 0) {
            const QRectF bar = capture_toolbar_rect(screen);
            double x = bar.x() + TOOL_PAD + GRIP_W;
            for (int i = 0; i < checked; ++i) x += capture_tools()[i].isEmpty() ? TOOL_SEPARATOR : TOOL_BUTTON;
            cursor = QPointF(x + TOOL_BUTTON / 2, bar.center().y());
        }
    }
    draw_cursor(p, cursor);
}

QRectF clipboard_window_rect(const QRectF &screen) {
    const double w = screen.width() * 0.4, h = screen.height() * 0.66;
    return {screen.right() - w - screen.width() * 0.06, screen.center().y() - h / 2, w, h};
}

void draw_clipboard_window(QPainter &p, const WelcomeTheme &theme, const QRectF &win, double fresh) {
    static const char *kinds[] = {"clipboard_image", "clipboard_file", "clipboard_text", "clipboard_link", "clipboard_text", "clipboard_image", "clipboard_text"};
    draw_shadow(p, theme, win, 6);
    p.setPen(QPen(QColor(theme.border), 0.8));
    p.setBrush(QColor(theme.panel));
    p.drawRoundedRect(win, 6, 6);
    // The group rail: close, the clipboard with the current-group bar, then add.
    const double rail_w = 16;
    const QRectF rail(win.right() - rail_w, win.y(), rail_w, win.height());
    p.setPen(QPen(QColor(theme.separator), 0.8));
    p.drawLine(QPointF(rail.left(), rail.top() + 2), QPointF(rail.left(), rail.bottom() - 2));
    demo_icon(FluentIcon::Close).paint(&p, QRectF(rail.center().x() - 3.5, rail.y() + 5, 7, 7).toRect());
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(theme.accent));
    p.drawRoundedRect(QRectF(rail.right() - 3, rail.y() + 19, 2, 9), 1, 1);
    p.setBrush(QColor("#D9822B"));
    p.drawRoundedRect(QRectF(rail.center().x() - 3.5, rail.y() + 19.5, 7, 8), 1.5, 1.5);
    p.setPen(QPen(QColor(theme.text_soft), 1));
    p.drawLine(QPointF(rail.center().x() - 2.5, rail.y() + 39), QPointF(rail.center().x() + 2.5, rail.y() + 39));
    p.drawLine(QPointF(rail.center().x(), rail.y() + 36.5), QPointF(rail.center().x(), rail.y() + 41.5));
    // The rows: number, kind icon, text and time; the newest pushes the others down.
    const QRectF list(win.x() + 4, win.y() + 4, win.width() - rail_w - 8, win.height() - 26);
    const double row_h = 15, step = row_h + 3;
    const double shift = (1 - fresh) * step;
    p.save();
    p.setClipRect(list);
    const QFont number = pixel_font(6);
    for (int row = 0; row < static_cast<int>(std::size(kinds)); ++row) {
        const QRectF item(list.x(), list.y() + row * step - shift, list.width(), row_h);
        p.save();
        if (row == 0) p.setOpacity(p.opacity() * fresh);
        p.setPen(Qt::NoPen);
        p.setBrush(row == 0 && fresh < 1 ? with_alpha(theme.accent, 40) : QColor(theme.panel_subtle));
        p.drawRoundedRect(item, 3, 3);
        p.setFont(number);
        p.setPen(QColor(theme.text_soft));
        p.drawText(QRectF(item.x() + 2, item.y(), 6, item.height()), Qt::AlignCenter, QString::number(row + 1));
        demo_icon(kinds[row]).paint(&p, QRectF(item.x() + 9, item.center().y() - 3.5, 7, 7).toRect());
        p.setPen(Qt::NoPen);
        p.setBrush(with_alpha(theme.text, 110));
        p.drawRoundedRect(QRectF(item.x() + 19, item.center().y() - 1.5, (item.width() - 38) * (row % 2 ? 0.55 : 0.8), 3), 1.5, 1.5);
        p.setBrush(with_alpha(theme.text_soft, 120));
        p.drawRoundedRect(QRectF(item.right() - 13, item.center().y() - 1, 10, 2.5), 1.2, 1.2);
        p.restore();
    }
    p.restore();
    // The search bar with the filter and settings buttons.
    const QRectF search(win.x() + 4, win.bottom() - 18, win.width() - rail_w - 30, 13);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(theme.panel_subtle));
    p.drawRoundedRect(search, 4, 4);
    demo_icon(FluentIcon::Search).paint(&p, QRectF(search.x() + 3, search.center().y() - 3, 6, 6).toRect());
    demo_icon(FluentIcon::Filter).paint(&p, QRectF(search.right() + 4, search.center().y() - 3.5, 7, 7).toRect());
    demo_icon(FluentIcon::Setting).paint(&p, QRectF(search.right() + 14, search.center().y() - 3.5, 7, 7).toRect());
}

namespace {
QFont keycap_font(double height) { return pixel_font(static_cast<int>(std::round(height * 0.42)), QFont::DemiBold); }
}

double keycap_width(QPainter &p, const QString &label, double height) {
    p.setFont(keycap_font(height));
    return std::max(height, p.fontMetrics().horizontalAdvance(label) + height * 0.62);
}

void draw_keycap(QPainter &p, const WelcomeTheme &theme, const QRectF &box, const QString &label, bool pressed) {
    const double radius = std::min(6.0, box.height() * 0.2);
    const QRectF face = pressed ? box.adjusted(0, KEYCAP_EDGE, 0, 0) : box.adjusted(0, 0, 0, -KEYCAP_EDGE);
    p.setPen(Qt::NoPen);
    if (!pressed) {
        p.setBrush(QColor(theme.border_strong));
        p.drawRoundedRect(box, radius, radius);
    }
    p.setBrush(pressed ? QColor(theme.accent) : QColor(theme.panel));
    p.setPen(QPen(pressed ? QColor(theme.accent) : QColor(theme.border_strong), 1));
    p.drawRoundedRect(face.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    p.setFont(keycap_font(box.height()));
    p.setPen(pressed ? QColor("#FFFFFF") : QColor(theme.text));
    p.drawText(face, Qt::AlignCenter, label);
}

LoopingDemo::LoopingDemo(int duration_ms, QWidget *parent) : QWidget(parent), duration_ms_(std::max(1, duration_ms)) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    timer_.setInterval(30);
    connect(&timer_, &QTimer::timeout, this, [this] { set_time(elapsed_ms_ + timer_.interval()); });
}

void LoopingDemo::set_time(int elapsed_ms) {
    elapsed_ms_ = std::max(0, elapsed_ms) % duration_ms_;
    time_changed();
    update();
}

void LoopingDemo::set_duration(int duration_ms) {
    duration_ms_ = std::max(1, duration_ms);
    elapsed_ms_ %= duration_ms_;
}

void LoopingDemo::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    timer_.start();
}

void LoopingDemo::hideEvent(QHideEvent *event) {
    timer_.stop();
    QWidget::hideEvent(event);
}

double LoopingDemo::loop_opacity(int fade_in, int fade_out) const {
    return std::max(0.05, std::min(phase(elapsed_ms_, 0, fade_in), 1.0 - phase(elapsed_ms_, duration_ms_ - fade_out, duration_ms_)));
}
}
