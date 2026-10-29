#include "canvas/smart_erase_wait.h"
#include "canvas/items/mosaic_item.h"
#include "canvas/items/smart_erase_fill.h"
#include <QGraphicsScene>
#include <QPointer>

namespace jietuba {
namespace {
// Rechecks the scene whenever a fill it waits for changes, so an item edited, undone or deleted
// meanwhile is judged as it is then.
class SmartEraseWaiter : public QObject {
public:
    SmartEraseWaiter(QGraphicsScene *scene, const QRectF &rect, QObject *context, std::function<void()> then)
        : QObject(context), scene_(scene), rect_(rect), then_(std::move(then)) {}
    // False once nothing is left to wait for.
    bool watch() {
        bool waiting = false;
        const auto type = Qt::ConnectionType(Qt::QueuedConnection | Qt::UniqueConnection);
        for (auto *mosaic : pending(scene_, rect_)) {
            waiting = true;
            const auto *fill = mosaic->smart_erase_fill();
            connect(fill, &SmartEraseFill::state_changed, this, &SmartEraseWaiter::recheck, type);
            connect(fill, &QObject::destroyed, this, &SmartEraseWaiter::recheck, type);
        }
        return waiting;
    }
    static QList<MosaicItem *> pending(QGraphicsScene *scene, const QRectF &rect) {
        QList<MosaicItem *> items;
        if (!scene) return items;
        for (auto *item : scene->items(rect, Qt::IntersectsItemBoundingRect))
            if (auto *mosaic = dynamic_cast<MosaicItem *>(item); mosaic && mosaic->isVisible() && mosaic->smart_erase_in_progress()) items.append(mosaic);
        return items;
    }

private:
    void recheck() {
        if (done_ || watch()) return;
        done_ = true;
        // then may release the context; this goes on its own once it returns.
        setParent(nullptr);
        deleteLater();
        const auto then = std::move(then_);
        then();
    }
    QPointer<QGraphicsScene> scene_;
    QRectF rect_;
    std::function<void()> then_;
    bool done_ = false;
};
}

bool smart_erase_pending(QGraphicsScene *scene, const QRectF &rect) { return !SmartEraseWaiter::pending(scene, rect).isEmpty(); }

bool when_smart_erase_ready(QGraphicsScene *scene, const QRectF &rect, QObject *context, std::function<void()> then) {
    if (!smart_erase_pending(scene, rect)) {
        then();
        return true;
    }
    (new SmartEraseWaiter(scene, rect, context, std::move(then)))->watch();
    return false;
}
}
