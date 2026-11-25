#include "ui/fluent_lite/control_style.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/theme.h"
#include <QApplication>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QProxyStyle>
#include <QRegularExpression>
#include <QStyleOption>

namespace jietuba {
namespace {
// The sheets' border is one unscaled pixel; padding, radius and heights scale.
constexpr int BORDER = 1;
constexpr int DROP_DOWN_WIDTH = 24;

const PushButton *painted_button(const QWidget *widget) {
    const auto *button = qobject_cast<const PushButton *>(widget);
    return button && button->paints_itself() ? button : nullptr;
}

const ComboBox *painted_combo(const QWidget *widget) {
    const auto *combo = qobject_cast<const ComboBox *>(widget);
    return combo && combo->paints_itself() ? combo : nullptr;
}

const LineEdit *painted_line_edit(const QWidget *widget) {
    const auto *edit = qobject_cast<const LineEdit *>(widget);
    return edit && edit->paints_itself() ? edit : nullptr;
}

// The sheets' min-height is the content height: the box adds padding and border to it.
QSize box_size(const QWidget *widget, QSize content, int padding_h) {
    content.setHeight(std::max(content.height(), widget_scaled(widget, INPUT_CONTENT_HEIGHT)));
    return content + QSize(2 * (padding_h + BORDER), 2 * (widget_scaled(widget, INPUT_PADDING_V) + BORDER));
}

QRect contents_rect(const QWidget *widget, const QRect &rect, int padding_h, int bottom_inset) {
    const int top = BORDER + widget_scaled(widget, INPUT_PADDING_V);
    return rect.adjusted(BORDER + padding_h, top, -(BORDER + padding_h), -bottom_inset);
}

// The label as QCommonStyle lays it out: icon and text centred together, 4px apart.
void paint_button_label(const PushButton *button, const QStyleOptionButton &option, QPainter *painter, const QStyle *style) {
    QRect text_rect = contents_rect(button, option.rect, button->horizontal_padding(), BORDER + widget_scaled(button, INPUT_PADDING_V));
    int flags = Qt::AlignVCenter | Qt::TextShowMnemonic;
    if (!style->styleHint(QStyle::SH_UnderlineShortcut, &option, button)) flags |= Qt::TextHideMnemonic;
    if (!option.icon.isNull()) {
        QIcon::Mode mode = option.state & QStyle::State_Enabled ? QIcon::Normal : QIcon::Disabled;
        if (mode == QIcon::Normal && option.state & QStyle::State_HasFocus) mode = QIcon::Active;
        const QIcon::State state = option.state & QStyle::State_On ? QIcon::On : QIcon::Off;
        const QPixmap pixmap = option.icon.pixmap(option.iconSize, painter->device()->devicePixelRatio(), mode, state);
        const int pixmap_width = qRound(pixmap.width() / pixmap.devicePixelRatio());
        const int pixmap_height = qRound(pixmap.height() / pixmap.devicePixelRatio());
        constexpr int icon_spacing = 4;
        int label_width = pixmap_width;
        if (!option.text.isEmpty()) label_width += option.fontMetrics.boundingRect(option.rect, flags, option.text).width() + icon_spacing;
        QRect icon_rect(text_rect.x() + (text_rect.width() - label_width) / 2, text_rect.y() + (text_rect.height() - pixmap_height) / 2,
                        pixmap_width, pixmap_height);
        icon_rect = QStyle::visualRect(option.direction, text_rect, icon_rect);
        if (option.direction == Qt::RightToLeft) text_rect.setRight(icon_rect.left() - icon_spacing / 2);
        else text_rect.setLeft(icon_rect.right() + 1 + icon_spacing / 2);
        flags |= painter->layoutDirection() == option.direction ? Qt::AlignLeft : Qt::AlignRight;
        painter->drawPixmap(icon_rect, pixmap);
    } else {
        flags |= Qt::AlignHCenter;
    }
    // The sheet sets its font on the painter, so a caller's own painter draws the same text.
    painter->setFont(button->font());
    painter->setPen(button->text_color(option.state));
    painter->drawText(text_rect, flags, option.text);
}

// The last compound selector of a rule: what the rule styles.
struct SelectorSubject {
    QString type;         // empty for a universal selector
    QString id;           // #name
    QString exact_class;  // .Class without a type: that class only, not subclasses
};

QList<SelectorSubject> parse_subjects(const QString &sheet) {
    static const QRegularExpression comments(R"(/\*.*?\*/)", QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression combinators(R"([\s>+~]+)");
    static const QRegularExpression type_part(R"(^(\*|[A-Za-z_][\w-]*))");
    static const QRegularExpression id_part(R"(#([\w-]+))");
    static const QRegularExpression class_part(R"(^\.([\w-]+))");
    QString text = sheet;
    text.remove(comments);
    QList<SelectorSubject> subjects;
    qsizetype pos = 0;
    while (true) {
        const qsizetype open = text.indexOf('{', pos);
        if (open < 0) break;
        for (const QString &selector : text.mid(pos, open - pos).split(',', Qt::SkipEmptyParts)) {
            const QStringList parts = selector.trimmed().split(combinators, Qt::SkipEmptyParts);
            if (parts.isEmpty()) continue;
            const QString &subject = parts.last();
            SelectorSubject parsed;
            const auto type = type_part.match(subject);
            if (type.hasMatch() && type.captured(1) != "*") parsed.type = type.captured(1);
            const QString rest = subject.mid(type.hasMatch() ? type.capturedLength() : 0);
            if (const auto id = id_part.match(rest); id.hasMatch()) parsed.id = id.captured(1);
            if (parsed.type.isEmpty())
                if (const auto cls = class_part.match(rest); cls.hasMatch()) parsed.exact_class = cls.captured(1);
            subjects.append(parsed);
        }
        const qsizetype close = text.indexOf('}', open);
        if (close < 0) break;
        pos = close + 1;
    }
    return subjects;
}

const QList<SelectorSubject> &subjects_of(const QString &sheet) {
    // Sheets are generated per theme and scale, so only a few distinct texts exist at a time.
    static QHash<QString, QList<SelectorSubject>> cache;
    if (cache.size() > 256) cache.clear();
    auto found = cache.find(sheet);
    if (found == cache.end()) found = cache.insert(sheet, parse_subjects(sheet));
    return *found;
}

class FluentControlStyle : public QProxyStyle {
public:
    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget) const override {
        if (const auto *edit = painted_line_edit(widget); edit && (element == PE_PanelLineEdit || element == PE_FrameLineEdit)) {
            if (element == PE_PanelLineEdit) paint_control_box(painter, option->rect, widget_scaled(edit, INPUT_RADIUS), edit->control_box(option->state));
            return;
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }

    void drawControl(ControlElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget) const override {
        if (const auto *button = painted_button(widget); button && element == CE_PushButton) {
            if (const auto *button_option = qstyleoption_cast<const QStyleOptionButton *>(option)) {
                paint_control_box(painter, option->rect, widget_scaled(button, INPUT_RADIUS), button->control_box(option->state));
                paint_button_label(button, *button_option, painter, this);
                return;
            }
        }
        QProxyStyle::drawControl(element, option, painter, widget);
    }

    void drawComplexControl(ComplexControl control, const QStyleOptionComplex *option, QPainter *painter, const QWidget *widget) const override {
        // The combo box draws its own text and chevron over the frame.
        if (const auto *combo = painted_combo(widget); combo && control == CC_ComboBox) {
            paint_control_box(painter, option->rect, widget_scaled(combo, INPUT_RADIUS), combo->control_box(option->state));
            return;
        }
        QProxyStyle::drawComplexControl(control, option, painter, widget);
    }

    QRect subControlRect(ComplexControl control, const QStyleOptionComplex *option, SubControl sub, const QWidget *widget) const override {
        if (const auto *combo = painted_combo(widget); combo && control == CC_ComboBox) {
            const int padding_h = widget_scaled(combo, INPUT_PADDING_H);
            const QRect contents = contents_rect(combo, option->rect, padding_h, BORDER + widget_scaled(combo, INPUT_PADDING_V));
            const int drop_down = widget_scaled(combo, DROP_DOWN_WIDTH);
            switch (sub) {
            case SC_ComboBoxFrame: return option->rect;
            case SC_ComboBoxEditField: return visualRect(option->direction, option->rect, contents.adjusted(0, 0, -drop_down, 0));
            case SC_ComboBoxArrow: {
                const QRect padding_box = option->rect.adjusted(BORDER, BORDER, -BORDER, -BORDER);
                return visualRect(option->direction, option->rect, QRect(padding_box.right() + 1 - drop_down, padding_box.top(), drop_down, padding_box.height()));
            }
            default: break;
            }
        }
        return QProxyStyle::subControlRect(control, option, sub, widget);
    }

    QRect subElementRect(SubElement element, const QStyleOption *option, const QWidget *widget) const override {
        if (const auto *edit = painted_line_edit(widget); edit && element == SE_LineEditContents) {
            // Focus thickens the bottom edge to 2 and takes the extra pixel from the padding.
            const bool focused = option->state & State_HasFocus;
            const int bottom = focused ? widget_scaled(edit, 2) + widget_scaled(edit, INPUT_PADDING_V - 1) : BORDER + widget_scaled(edit, INPUT_PADDING_V);
            return visualRect(option->direction, option->rect, contents_rect(edit, option->rect, widget_scaled(edit, INPUT_PADDING_H), bottom));
        }
        if (const auto *button = painted_button(widget); button && element == SE_PushButtonContents) {
            return visualRect(option->direction, option->rect,
                              contents_rect(button, option->rect, button->horizontal_padding(), BORDER + widget_scaled(button, INPUT_PADDING_V)));
        }
        return QProxyStyle::subElementRect(element, option, widget);
    }

    QSize sizeFromContents(ContentsType type, const QStyleOption *option, const QSize &size, const QWidget *widget) const override {
        if (const auto *button = painted_button(widget); button && type == CT_PushButton) return box_size(button, size, button->horizontal_padding());
        if (const auto *combo = painted_combo(widget); combo && type == CT_ComboBox) {
            // The style sheet adds the drop-down column and the 2px QCommonStyle keeps around a combo's label.
            return box_size(combo, size + QSize(widget_scaled(combo, DROP_DOWN_WIDTH) + 2, 0), widget_scaled(combo, INPUT_PADDING_H));
        }
        if (const auto *edit = painted_line_edit(widget); edit && type == CT_LineEdit) return box_size(edit, size, widget_scaled(edit, INPUT_PADDING_H));
        return QProxyStyle::sizeFromContents(type, option, size, widget);
    }

    int pixelMetric(PixelMetric metric, const QStyleOption *option, const QWidget *widget) const override {
        // A box-styled button neither shifts its label when pressed nor reserves a default frame.
        if (painted_button(widget) && (metric == PM_ButtonShiftHorizontal || metric == PM_ButtonShiftVertical || metric == PM_ButtonMargin || metric == PM_ButtonDefaultIndicator))
            return 0;
        return QProxyStyle::pixelMetric(metric, option, widget);
    }
};
}

bool style_sheet_rule_reaches(const QWidget *widget) {
    if (!widget->styleSheet().isEmpty()) return true;
    // Type selectors match the class and its bases, spelt with "--" for "::".
    QStringList types;
    for (const QMetaObject *meta = widget->metaObject(); meta; meta = meta->superClass())
        types.append(QString::fromLatin1(meta->className()).replace("::", "--"));
    const QString &exact = types.first();
    auto reaches = [&](const QString &sheet) {
        if (sheet.isEmpty()) return false;
        for (const auto &subject : subjects_of(sheet)) {
            if (!subject.id.isEmpty() && subject.id != widget->objectName()) continue;
            if (!subject.exact_class.isEmpty()) {
                if (subject.exact_class == exact) return true;
                continue;
            }
            if (subject.type.isEmpty() || types.contains(subject.type)) return true;
        }
        return false;
    };
    for (const QWidget *ancestor = widget->parentWidget(); ancestor; ancestor = ancestor->parentWidget())
        if (reaches(ancestor->styleSheet())) return true;
    return qApp && reaches(qApp->styleSheet());
}

QStyle *fluent_control_style() {
    static auto *style = new FluentControlStyle;
    return style;
}

// Follows the style sheet's drawing (QRenderRule::drawFrame): the background fills a path
// inset by half the border, then the edges are filled and the corners stroked over it, in the
// order bottom, right, left, top, so translucent theme colours blend as under the sheet.
void paint_control_box(QPainter *painter, const QRect &rect, int radius, const ControlBox &box) {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(Qt::NoPen);
    const QRectF r(rect);
    const qreal side = BORDER, bottom = box.bottom_width;
    // The sheet drops the rounding when two corners do not fit.
    const qreal radius_f = 2 * radius > rect.width() || 2 * radius > rect.height() ? 0 : radius;
    if (radius_f <= 0) {
        painter->fillRect(r, box.fill);
        painter->fillRect(QRectF(r.left(), r.bottom() - bottom, r.width(), bottom), box.bottom_edge);
        painter->fillRect(QRectF(r.right() - side, r.top(), side, r.height() - bottom), box.edge);
        painter->fillRect(QRectF(r.left(), r.top(), side, r.height() - bottom), box.edge);
        painter->fillRect(QRectF(r.left() + side, r.top(), r.width() - 2 * side, side), box.edge);
        painter->restore();
        return;
    }
    const qreal d = 2 * radius_f;
    QPainterPath fill;
    qreal y = r.top() + side / 2, x = r.right() - side / 2;
    fill.moveTo(r.left() + radius_f, y);
    fill.lineTo(r.right() - radius_f, y);
    fill.arcTo(x - d + side, y, d - side, d - side, 90, -90);
    fill.lineTo(x, r.bottom() - radius_f);
    y = r.bottom() - bottom / 2;
    fill.arcTo(x - d + side, y - d + bottom, d - side, d - bottom, 0, -90);
    fill.lineTo(r.left() + radius_f, y);
    x = r.left() + side / 2;
    fill.arcTo(x, r.bottom() - d + bottom / 2, d - side, d - bottom, 270, -90);
    fill.lineTo(x, r.top() + radius_f);
    fill.arcTo(x, r.top() + side / 2, d - side, d - side, 180, -90);
    fill.closeSubpath();
    painter->fillPath(fill, box.fill);

    const auto stroke = [painter](const QColor &color, qreal width, const QRectF &arc, int start, int span) {
        QPen pen(color, width);
        pen.setCapStyle(Qt::SquareCap);
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);
        painter->drawArc(arc, start * 16, span * 16);
        painter->setPen(Qt::NoPen);
    };
    // Bottom
    qreal x1 = r.left() + radius_f, x2 = r.right() - radius_f, y1 = r.bottom() - bottom, y2 = r.bottom();
    painter->fillRect(QRectF(x1, y1, x2 - x1, y2 - y1), box.bottom_edge);
    stroke(box.bottom_edge, bottom, QRectF(x1 - radius_f + bottom / 2, y2 - d + bottom / 2, d - bottom, d - bottom), -90, -45);
    stroke(box.bottom_edge, bottom, QRectF(x2 - radius_f + bottom / 2, y2 - d + bottom / 2, d - bottom, d - bottom), -90, 45);
    // Right
    x1 = r.right() - side, x2 = r.right(), y1 = r.top() + radius_f, y2 = r.bottom() - radius_f;
    painter->fillRect(QRectF(x1, y1, x2 - x1, y2 - y1), box.edge);
    stroke(box.edge, side, QRectF(x2 - d + side / 2, y1 - radius_f + side / 2, d - side, d - side), 45, -45);
    stroke(box.edge, side, QRectF(x2 - d + side / 2, y2 - radius_f + side / 2, d - side, d - side), 315, 45);
    // Left
    x1 = r.left(), x2 = r.left() + side;
    painter->fillRect(QRectF(x1, y1, x2 - x1, y2 - y1), box.edge);
    stroke(box.edge, side, QRectF(x1 + side / 2, y1 - radius_f + side / 2, d - side, d - side), 135, 45);
    stroke(box.edge, side, QRectF(x1 + side / 2, y2 - radius_f + side / 2, d - side, d - side), 180, 45);
    // Top
    x1 = r.left() + radius_f, x2 = r.right() - radius_f, y1 = r.top(), y2 = r.top() + side;
    painter->fillRect(QRectF(x1, y1, x2 - x1, y2 - y1), box.edge);
    stroke(box.edge, side, QRectF(x1 - radius_f + side / 2, y1 + side / 2, d - side, d - side), 135, -45);
    stroke(box.edge, side, QRectF(x2 - radius_f + side / 2, y1 + side / 2, d - side, d - side), 45, 45);
    painter->restore();
}

QFont fluent_control_font(const QWidget *widget, int base_pixel_size, QFont::Weight weight) {
    QFont font;
    font.setFamilies(FONT_FAMILIES());
    font.setPixelSize(widget_scaled(widget, base_pixel_size));
    // The sheets' font shorthand also resets weight and style.
    font.setWeight(weight);
    font.setStyle(QFont::StyleNormal);
    return font;
}

void apply_sheet_palette(QWidget *widget, const QColor &text, const QColor &disabled_text, const QColor &fill, const QColor &disabled_fill,
                         const QColor &highlight) {
    QPalette palette = widget->palette();
    for (const auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        const bool disabled = group == QPalette::Disabled;
        const QColor &fg = disabled ? disabled_text : text;
        const QColor &bg = disabled ? disabled_fill : fill;
        for (const auto role : {QPalette::Base, QPalette::Button, QPalette::Window}) palette.setColor(group, role, bg);
        for (const auto role : {QPalette::ButtonText, QPalette::WindowText, QPalette::Text}) palette.setColor(group, role, fg);
        QColor placeholder = fg;
        placeholder.setAlpha((placeholder.alpha() + 1) / 2);
        palette.setColor(group, QPalette::PlaceholderText, placeholder);
        if (highlight.isValid()) palette.setColor(group, QPalette::Highlight, highlight);
    }
    widget->setPalette(palette);
}
}
