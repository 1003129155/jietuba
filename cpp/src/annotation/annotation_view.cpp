#include "annotation/annotation_view.h"
#include "annotation/annotation_toolbar.h"
#include "annotation/cursor_highlight.h"
#include "annotation/screen_annotation.h"
#include "core/ui_theme.h"
#include "tools/controller.h"
#include <QGraphicsTextItem>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>

namespace jietuba {
// The board of one screen. Layered, so nothing shows before its first paint, which comes after the
// session has stacked it; it never activates, so a click on it does not bring it above the pins.
class AnnotationBoard : public QWidget {
public:
    AnnotationBoard(const QRect &geometry, const QColor &color)
        : QWidget(nullptr, Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus), color_(color) {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setGeometry(geometry);
    }
    QColor color() const { return color_; }
    void set_color(const QColor &color) {
        color_ = color;
        update();
    }
protected:
    void paintEvent(QPaintEvent *) override { QPainter(this).fillRect(rect(), color_); }
private:
    QColor color_;
};

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
    if (!screen_) return;
    update_rect(screen_->geometry());
    if (board_) board_->setGeometry(screen_->geometry());
}

Toolbar *AnnotationView::canvas_toolbar() const { return owner_ ? owner_->toolbar() : nullptr; }

void AnnotationView::set_board(const std::optional<QColor> &color) {
    if (!color) {
        board_.reset();
        return;
    }
    if (board_) {
        board_->set_color(*color);
        return;
    }
    board_ = std::make_unique<AnnotationBoard>(geometry(), *color);
    board_->show();
}

std::optional<QColor> AnnotationView::board() const {
    if (!board_) return std::nullopt;
    return board_->color();
}

QWidget *AnnotationView::board_window() const { return board_.get(); }

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
