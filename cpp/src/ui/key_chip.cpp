#include "ui/key_chip.h"
#include "core/i18n.h"
#include "core/shortcut_manager.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "ui/fluent_lite/theme.h"
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <stdexcept>

namespace jietuba {
namespace {
QString tr_text(const char *text) { return i18n_tr(text, "KeyChip"); }

QString part_display(const QString &part) {
    static const QMap<QString, QString> modifier_names{{"ctrl", "Ctrl"}, {"shift", "Shift"}, {"alt", "Alt"}, {"win", "Win"}, {"meta", "Win"}};
    if (modifier_names.contains(part)) return modifier_names.value(part);
    if (part == MOUSE_BUTTON_BACK) return tr_text("Mouse Back");
    if (part == MOUSE_BUTTON_FORWARD) return tr_text("Mouse Forward");
    if (part == MOUSE_BUTTON_MIDDLE) return i18n_tr("Middle", "InAppShortcut");
    const auto &parse_map = get_key_parse_map();
    if (const auto it = parse_map.find(part); it != parse_map.end() && get_key_display_map().contains(*it)) return get_key_display_map().value(*it);
    // Letters, digits and F keys in capitals; other rare key names capitalised.
    return part.size() <= 3 ? part.toUpper() : part.left(1).toUpper() + part.mid(1).toLower();
}
}

std::pair<QStringList, bool> shortcut_display_parts(const QString &value) {
    const QString text = value.trimmed().toLower();
    if (text.isEmpty()) return {{}, false};
    const bool pending = text.endsWith("+") && !text.endsWith("++");
    QStringList parts;
    for (const auto &part : text.split('+'))
        if (!part.isEmpty()) parts.append(part);
    if (text.endsWith("++")) parts.append("+");
    QStringList display;
    for (const auto &part : parts) display.append(part_display(part));
    return {display, pending};
}

QString format_shortcut_text(const QString &value) {
    const auto [parts, pending] = shortcut_display_parts(value);
    return parts.join(" + ") + (pending ? " +" : "");
}

KeyChipLineEdit::KeyChipLineEdit(QWidget *parent) : QLineEdit(parent) {
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    setFixedHeight(dialog_scaled(CHIP_HEIGHT));
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { update(); });
}

QSize KeyChipLineEdit::sizeHint() const { return QSize(dialog_scaled(CHIP_WIDTH), dialog_scaled(CHIP_HEIGHT)); }

QSize KeyChipLineEdit::minimumSizeHint() const { return QSize(dialog_scaled(96), dialog_scaled(CHIP_HEIGHT)); }

void KeyChipLineEdit::setErrorState(bool error) {
    if (error_ != error) {
        error_ = error;
        update();
    }
}

QRectF KeyChipLineEdit::clear_rect() const {
    const int side = height();
    return QRectF(width() - side, 0, side, side);
}

void KeyChipLineEdit::mouseMoveEvent(QMouseEvent *event) {
    const bool hover = !text().isEmpty() && clear_rect().contains(event->position());
    if (hover != hover_clear_) {
        hover_clear_ = hover;
        update();
    }
    QLineEdit::mouseMoveEvent(event);
}

void KeyChipLineEdit::leaveEvent(QEvent *event) {
    hover_clear_ = false;
    update();
    QLineEdit::leaveEvent(event);
}

void KeyChipLineEdit::enterEvent(QEnterEvent *event) {
    update();
    QLineEdit::enterEvent(event);
}

void KeyChipLineEdit::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && !text().isEmpty() && clear_rect().contains(event->position())) {
        event->accept();
        setText("");
        return;
    }
    QLineEdit::mousePressEvent(event);
}

void KeyChipLineEdit::paintEvent(QPaintEvent *) {
    const auto &t = get_ui_theme()->tokens();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QFont chip_font(font());
    chip_font.setPixelSize(dialog_scaled(13));
    painter.setFont(chip_font);
    const QFontMetrics metrics(chip_font);

    const int radius = dialog_scaled(8);
    const QRectF rect = QRectF(this->rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const int pad = dialog_scaled(12);
    const bool focused = hasFocus();
    const auto [parts, pending] = shortcut_display_parts(text());

    if (parts.isEmpty() && !pending && !focused) {
        QColor border = css_color(t.text_muted);
        border.setAlphaF(0.55);
        painter.setPen(QPen(border, 1.0, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(rect, radius, radius);
        paint_plus(painter, QPointF(pad + dialog_scaled(5), rect.center().y()), css_color(t.text_muted));
        painter.setPen(css_color(t.text_muted));
        const QRectF text_rect = rect.adjusted(pad + dialog_scaled(16), 0, -pad, 0);
        painter.drawText(text_rect, Qt::AlignVCenter | Qt::AlignLeft, metrics.elidedText(tr_text("Add shortcut"), Qt::ElideRight, static_cast<int>(text_rect.width())));
        return;
    }

    QColor fill, border;
    double width = 1.0;
    if (error_) {
        fill = css_color(t.danger_soft), border = css_color(t.danger);
    } else if (focused) {
        fill = css_color(t.surface_hover), border = css_color(t.accent), width = 1.5;
    } else {
        // In the dark theme input_background equals the card colour and the chip would be only an outline.
        fill = css_color(t.surface_hover);
        border = css_color(underMouse() ? t.accent : t.border_hover);
    }
    const double inset = width / 2;
    painter.setPen(QPen(border, width));
    painter.setBrush(fill);
    painter.drawRoundedRect(QRectF(this->rect()).adjusted(inset, inset, -inset, -inset), radius, radius);

    const QRectF clear = clear_rect();
    const double text_right = !text().isEmpty() ? clear.left() : rect.right() - pad;
    const int available = static_cast<int>(text_right - pad);
    const QColor text_color = css_color(t.text), sep_color = css_color(t.text_muted);
    const double baseline = rect.center().y() + (metrics.ascent() - metrics.descent()) / 2.0;

    if (parts.isEmpty()) {
        painter.setPen(sep_color);
        painter.drawText(QPointF(pad, baseline), metrics.elidedText(tr_text("Press keys…"), Qt::ElideRight, available));
    } else {
        const QString sep = " + ";
        const QString full = parts.join(sep) + (pending ? " +" : "");
        if (metrics.horizontalAdvance(full) > available) {
            painter.setPen(text_color);
            painter.drawText(QPointF(pad, baseline), metrics.elidedText(full, Qt::ElideRight, available));
        } else {
            double x = pad;
            for (int index = 0; index < parts.size(); ++index) {
                if (index) {
                    painter.setPen(sep_color);
                    painter.drawText(QPointF(x, baseline), sep);
                    x += metrics.horizontalAdvance(sep);
                }
                painter.setPen(text_color);
                painter.drawText(QPointF(x, baseline), parts[index]);
                x += metrics.horizontalAdvance(parts[index]);
            }
            if (pending) {
                painter.setPen(sep_color);
                painter.drawText(QPointF(x, baseline), " +");
            }
        }
    }

    if (!text().isEmpty()) {
        const QColor color = css_color(hover_clear_ ? t.text : t.text_muted);
        const int half = dialog_scaled(4);
        const QPointF center = clear.center();
        painter.setPen(QPen(color, 1.4, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(center.x() - half, center.y() - half), QPointF(center.x() + half, center.y() + half));
        painter.drawLine(QPointF(center.x() + half, center.y() - half), QPointF(center.x() - half, center.y() + half));
    }
}

void KeyChipLineEdit::paint_plus(QPainter &painter, const QPointF &center, const QColor &color) {
    const int half = dialog_scaled(5);
    painter.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(QPointF(center.x() - half, center.y()), QPointF(center.x() + half, center.y()));
    painter.drawLine(QPointF(center.x(), center.y() - half), QPointF(center.x(), center.y() + half));
}

StatusIcon::StatusIcon(QWidget *parent) : QWidget(parent) {
    const int side = dialog_scaled(STATUS_SIZE);
    setFixedSize(side, side);
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { update(); });
}

void StatusIcon::setState(const QString &state, const QString &tooltip) {
    if (state != "" && state != "ok" && state != "idle" && state != "error") throw std::invalid_argument(state.toStdString());
    state_ = state;
    setToolTip(tooltip);
    update();
}

void StatusIcon::paintEvent(QPaintEvent *) {
    if (state_.isEmpty()) return;
    const auto &t = get_ui_theme()->tokens();
    const QString color = state_ == "ok" ? t.success : state_ == "idle" ? t.status_idle : t.danger;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(css_color(color));
    const double side = std::min(width(), height());
    painter.drawEllipse(QRectF(0, 0, side, side));

    painter.setPen(QPen(Qt::white, std::max(1.6, side * 0.12), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    if (state_ == "ok") {
        QPainterPath path(QPointF(side * 0.29, side * 0.52));
        path.lineTo(side * 0.44, side * 0.66);
        path.lineTo(side * 0.71, side * 0.37);
        painter.drawPath(path);
    } else if (state_ == "idle") {
        painter.drawLine(QPointF(side * 0.31, side * 0.5), QPointF(side * 0.69, side * 0.5));
    } else {
        const double a = side * 0.35, b = side * 0.65;
        painter.drawLine(QPointF(a, a), QPointF(b, b));
        painter.drawLine(QPointF(b, a), QPointF(a, b));
    }
}
}
