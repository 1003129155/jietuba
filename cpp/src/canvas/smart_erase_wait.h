#pragma once
#include <QRectF>
#include <functional>

class QGraphicsScene;
class QObject;

namespace jietuba {
// Whether a smart erase drawn in rect is still rebuilding its pixels; one waiting out its edit delay
// starts now.
bool smart_erase_pending(QGraphicsScene *scene, const QRectF &rect);
// Calls then on the UI thread once every smart erase in rect has its pixels: right away when they all
// have, else when the last one finishes. Returns whether it ran right away. It never runs once context
// is destroyed.
bool when_smart_erase_ready(QGraphicsScene *scene, const QRectF &rect, QObject *context, std::function<void()> then);
}
