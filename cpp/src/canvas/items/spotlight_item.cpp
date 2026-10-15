#include "canvas/items/spotlight_item.h"
#include <QGraphicsScene>
#include <QPainter>
#include <algorithm>

namespace jietuba {
SpotlightCurtain::SpotlightCurtain() {
    setZValue(Z_VALUE);
    setOpacity(DEFAULT_DARKNESS);
    setAcceptedMouseButtons(Qt::NoButton);
}

SpotlightCurtain *SpotlightCurtain::find(QGraphicsScene *scene) {
    if (!scene) return nullptr;
    for (auto *item : scene->items())
        if (auto *curtain = dynamic_cast<SpotlightCurtain *>(item)) return curtain;
    return nullptr;
}

SpotlightCurtain *SpotlightCurtain::of(QGraphicsScene *scene) {
    auto *curtain = find(scene);
    if (!curtain) {
        curtain = new SpotlightCurtain;
        scene->addItem(curtain);
    }
    return curtain;
}

QRectF SpotlightCurtain::boundingRect() const {
    auto *s = scene();
    return s ? s->sceneRect() : QRectF();
}

void SpotlightCurtain::paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *) {
    QList<SpotlightItem *> spotlights;
    for (auto *item : scene()->items())
        if (auto *spotlight = dynamic_cast<SpotlightItem *>(item); spotlight && spotlight->isVisible()) spotlights.append(spotlight);
    // "Any spotlight" rather than "non-empty hole": a just-pressed hole has zero
    // area, and the curtain must appear now or only the hole's area repaints later.
    if (spotlights.isEmpty()) return;
    QPainterPath holes;
    // United one by one: odd-even filling of a combined path would re-darken overlaps.
    for (auto *spotlight : spotlights) holes = holes.united(spotlight->hole_path());
    QPainterPath curtain;
    curtain.addRect(boundingRect());
    painter->setRenderHint(QPainter::Antialiasing);
    painter->fillPath(curtain.subtracted(holes), QColor(0, 0, 0));
}

SpotlightItem::SpotlightItem(const QRectF &rect, qreal corner_radius) : RectItem(rect, QPen(Qt::NoPen), corner_radius) {
    // Above the curtain so the selection frame on the hole edge is not half darkened.
    setZValue(SpotlightCurtain::Z_VALUE + 1);
}

QPainterPath SpotlightItem::shape() const {
    QPainterPath path;
    const auto radius = get_corner_radius();
    path.addRoundedRect(rect(), radius, radius);
    return path;
}

QPainterPath SpotlightItem::hole_path() const { return mapToScene(shape()); }

QVariant SpotlightItem::itemChange(GraphicsItemChange change, const QVariant &value) {
    // The first hole appearing or the last one leaving changes the whole curtain.
    // On ItemSceneChange the hole is still in the old scene; on
    // ItemSceneHasChanged it is in the new one, whose curtain may not exist yet.
    if (change == ItemSceneChange || change == ItemVisibleHasChanged) {
        if (auto *curtain = SpotlightCurtain::find(scene())) curtain->update();
    } else if (change == ItemSceneHasChanged && scene()) {
        SpotlightCurtain::of(scene())->update();
    }
    return RectItem::itemChange(change, value);
}

bool SpotlightItem::set_visual_opacity(qreal opacity) {
    if (!scene()) return false;
    SpotlightCurtain::of(scene())->setOpacity(std::max(0.0, std::min(1.0, opacity)));
    return true;
}

std::optional<qreal> SpotlightItem::get_visual_opacity() const {
    auto *curtain = SpotlightCurtain::find(scene());
    return curtain ? curtain->opacity() : SpotlightCurtain::DEFAULT_DARKNESS;
}
}
