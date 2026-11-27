#pragma once
#include <QColor>
#include <QFont>
#include <QStyle>

class QPainter;
class QWidget;

namespace jietuba {
// Fill and frame of a rounded input or button; the bottom edge may differ in colour and width.
struct ControlBox {
    QColor fill, edge, bottom_edge;
    int bottom_width = 1;
};

// One instance shared by ComboBox, LineEdit and PushButton while they paint themselves. It
// reproduces their style sheets' frames and metrics, so a widget can fall back to the sheet
// (when another sheet's rule reaches it) without changing size. A per-widget style sheet
// costs 7 to 15 times the widget itself to build.
QStyle *fluent_control_style();

// Whether the widget's own sheet, an ancestor's or the application's has a rule that can
// select it (or one of its subcontrols). Selectors are read conservatively: ancestors in a
// selector are not checked. Qt's WA_StyleSheetTarget does not tell, as Qt's built-in rules
// (such as QLineEdit:no-frame) set it on every line edit under any sheet.
bool style_sheet_rule_reaches(const QWidget *widget);

void paint_control_box(QPainter *painter, const QRect &rect, int radius, const ControlBox &box);
// The font a fluent control's sheet sets: its family stack at a scaled pixel size.
QFont fluent_control_font(const QWidget *widget, int base_pixel_size, QFont::Weight weight = QFont::Normal);
// The palette roles a style sheet's color, background and selection-background-color set.
void apply_sheet_palette(QWidget *widget, const QColor &text, const QColor &disabled_text, const QColor &fill,
                         const QColor &disabled_fill, const QColor &highlight = {});
}
