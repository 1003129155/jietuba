#include "annotation/annotation_view.h"
#include "annotation/annotation_toolbar.h"
#include "annotation/cursor_highlight.h"
#include "annotation/screen_annotation.h"
#include "core/ui_theme.h"
#include "tools/controller.h"
#include <QGraphicsRectItem>
#include <QGraphicsTextItem>
#include <QMouseEvent>
#include <QScreen>

namespace jietuba {
AnnotationView::AnnotationView(QScreen *screen, ScreenAnnotation *owner) : GifDrawingView(screen->geometry(), false), screen_(screen), owner_(owner) {
    // The base class's transparent background is styled for its own class name, which a style sheet
    // class selector does not match in a subclass; without it the viewport fills with opaque white.
    set_own_style(this, "background: transparent;");
}

AnnotationView::~AnnotationView() = default;

PointerLight *AnnotationView::pointer_light(bool create) {
    if (!light_ && create) light_ = std::make_unique<PointerLight>(scene_obj());
    return light_.get();
}

void AnnotationView::remove_pointer_light() { light_.reset(); }

void AnnotationView::fit_screen() {
    if (screen_) update_rect(screen_->geometry());
}

Toolbar *AnnotationView::canvas_toolbar() const { return owner_ ? owner_->toolbar() : nullptr; }

void AnnotationView::set_board(const std::optional<QColor> &color) {
    if (!color) {
        delete board_item_;
        board_item_ = nullptr;
        return;
    }
    if (!board_item_) {
        board_item_ = new QGraphicsRectItem(scene_obj()->sceneRect());
        board_item_->setPen(Qt::NoPen);
        // Above the hit layer, below every drawing.
        board_item_->setZValue(-900);
        board_item_->setData(TRANSIENT_ITEM_DATA, true);
        board_item_->setAcceptedMouseButtons(Qt::NoButton);
        scene_obj()->addItem(board_item_);
    }
    board_item_->setBrush(*color);
}

std::optional<QColor> AnnotationView::board() const {
    if (!board_item_) return std::nullopt;
    return board_item_->brush().color();
}

void AnnotationView::mouseMoveEvent(QMouseEvent *event) {
    GifDrawingView::mouseMoveEvent(event);
    // The highlight's disc moves with the stroke in one repaint: a repaint of its own for every frame
    // would make this window, which is as big as the screen, flush twice as often.
    if (light_ && !passthrough()) light_->set_pointer(mapToScene(event->pos()), event->buttons() != Qt::NoButton);
}

void AnnotationView::leaveEvent(QEvent *event) {
    // The light stays where the pointer left otherwise, at the edge of this screen.
    if (light_) light_->set_pointer(std::nullopt, false);
    GifDrawingView::leaveEvent(event);
}

void AnnotationView::on_item_auto_select_requested(QGraphicsItem *item) {
    if (dynamic_cast<QGraphicsTextItem *>(item)) GifDrawingView::on_item_auto_select_requested(item);
}

void AnnotationView::mousePressEvent(QMouseEvent *event) {
    if (!passthrough()) emit pressed(this);
    GifDrawingView::mousePressEvent(event);
}
}
