#include "canvas/canvas_host.h"
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QWidget>

namespace jietuba {
CanvasHost *canvas_host_of(QWidget *widget) {
    return widget ? dynamic_cast<CanvasHost *>(widget->window()) : nullptr;
}

CanvasHost *canvas_host_of_scene(QGraphicsScene *scene) {
    if (!scene) return nullptr;
    const auto views = scene->views();
    return views.isEmpty() ? nullptr : canvas_host_of(views.front());
}
}
