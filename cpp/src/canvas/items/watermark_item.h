#pragma once
#include "canvas/items/item_reference.h"
#include <QDateTime>
#include <QFont>
#include <QGraphicsItem>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <optional>
#include <utility>
#include <vector>

class QPainter;

namespace jietuba {
// A text layer over the selection: tiled, centred, in a corner or at an edge
// midpoint. Only parameters are stored; paint draws just the copies that meet
// the exposed area (about 3 ms for a full 2560x1440 repaint).
inline const QString LAYOUT_TILE = "tile";
inline const QString LAYOUT_CENTER = "center";
// Layout -> relative position of the text block centre within the selection.
const std::vector<std::pair<QString, QPointF>> &watermark_anchors();
const QStringList &watermark_layouts();

constexpr int MIN_FONT_SIZE = 8, MAX_FONT_SIZE = 96;
constexpr int MIN_SPACING = 0, MAX_SPACING = 100;
constexpr int MIN_ANGLE = -90, MAX_ANGLE = 90;
// Spacing is in tenths of the font size so density looks the same at any size.
constexpr double SPACING_UNIT = .1;
constexpr double VERTICAL_GAP_RATIO = .75;
constexpr double EDGE_MARGIN = .6;
constexpr double GLYPH_OVERHANG = .25;

struct WatermarkStyle {
    QString color = "#808080";
    int font_size = 24;   // pixels, so exports match the screen at any DPI
    double opacity = .35;
    QString layout = LAYOUT_TILE;
    int spacing = 40;
    int angle = -30;      // tile layout only
    QString font_family;  // empty: the UI language's default font
    WatermarkStyle normalized() const { return from_values(to_values()); }
    // Builds a style from raw (possibly hand-edited) values, clamping every field.
    static WatermarkStyle from_values(const QVariantMap &values);
    QVariantMap to_values() const;
    WatermarkStyle with_changes(const QVariantMap &changes) const;
    bool operator==(const WatermarkStyle &other) const;
    bool operator!=(const WatermarkStyle &other) const { return !(*this == other); }
};

// Expands $...$ (a Qt date format) to the moment when; unpaired $ is kept.
QString expand_text(const QString &template_text, const QDateTime &when = QDateTime());
QFont watermark_font(const WatermarkStyle &style);
// (block centre, rotation) of every copy to draw; an empty exposed rect means the whole selection.
std::vector<std::pair<QPointF, double>> watermark_placements(const QRectF &rect, const QString &text, const WatermarkStyle &style,
                                                             const QRectF &exposed = QRectF(), const std::optional<QFont> &font = std::nullopt);
// Draws into rect, clipped to it. Shared by the canvas item and direct exports.
void paint_watermark(QPainter *painter, const QRectF &rect, const QString &text, const WatermarkStyle &style,
                     const QRectF &exposed = QRectF(), const std::optional<QFont> &font = std::nullopt);
// Returns a watermarked copy; the original is left untouched.
QImage apply_watermark(const QImage &image, const QString &text, const WatermarkStyle &style);

// The single watermark of a scene, always covering that scene's selection.
// Time placeholders expand at created_at, fixed when the watermark is applied.
// Not hit-testable: clicks, hover and the eraser fall through to annotations.
class WatermarkItem : public QGraphicsItem, public TrackedItem {
public:
    // Above text (30), below the selection item (101).
    static constexpr qreal Z_VALUE = 50;
    WatermarkItem(const QString &template_text, const WatermarkStyle &style, const QDateTime &created_at = QDateTime());
    static WatermarkItem *find(QGraphicsScene *scene);
    QString template_text() const { return template_; }
    QDateTime created_at() const { return created_at_; }
    QString text() const { return text_; }
    // With when, the watermark's time changes to that moment too.
    void set_template(const QString &template_text, const QDateTime &when = QDateTime());
    WatermarkStyle style() const { return style_; }
    void set_style(const WatermarkStyle &style);
    void set_scene_rect(const QRectF &rect);
    QRectF rect() const { return rect_; }
    QRectF boundingRect() const override { return rect_; }
    QPainterPath shape() const override { return {}; }
    void paint(QPainter *, const QStyleOptionGraphicsItem *, QWidget * = nullptr) override;
protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override;
private:
    QDateTime created_at_;
    QString template_, text_;
    WatermarkStyle style_;
    QFont font_;
    QRectF rect_;
};
}
