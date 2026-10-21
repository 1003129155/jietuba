#include "pin/pin_canvas_view.h"
#include "core/ui_theme.h"
#include "pin/pin_canvas.h"
#include "pin/pin_hover.h"
#include "pin/pin_text_selection.h"
#include "pin/pin_toolbar.h"
#include "pin/pin_window.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainterPath>
#include <QWheelEvent>

namespace jietuba {
PinCanvasView::PinCanvasView(CanvasScene *scene, PinWindow *pin_window, PinCanvas *pin_canvas, bool cross_tool_select)
    : CanvasView(scene, nullptr, false, cross_tool_select), pin_window(pin_window), pin_canvas(pin_canvas) {
    setFrameShape(QFrame::NoFrame);
    set_own_style(this, "background: transparent;");
    viewport()->setAutoFillBackground(false);
    setBackgroundBrush(Qt::transparent);
    // Only smooth pixmap scaling, so enlarged images stay sharp enough.
    setRenderHint(QPainter::SmoothPixmapTransform, true);
    // Repaints only changed areas.
    setViewportUpdateMode(QGraphicsView::SmartViewportUpdate);
}

void PinCanvasView::set_corner_radius(double radius) {
    corner_radius_ = radius;
    update_viewport_mask();
}

void PinCanvasView::update_viewport_mask() {
    if (corner_radius_ <= 0) {
        viewport()->clearMask();
        return;
    }
    QPainterPath path;
    path.addRoundedRect(QRectF(viewport()->rect()), corner_radius_, corner_radius_);
    viewport()->setMask(QRegion(path.toFillPolygon().toPolygon()));
}

void PinCanvasView::resizeEvent(QResizeEvent *event) {
    CanvasView::resizeEvent(event);
    update_viewport_mask();
}

void PinCanvasView::enterEvent(QEnterEvent *event) {
    if (pin_canvas->is_editing()) CanvasView::enterEvent(event);
    else setCursor(Qt::ArrowCursor);
}

void PinCanvasView::mousePressEvent(QMouseEvent *event) {
    if (!pin_canvas->is_editing()) {
        if (text_selection && text_selection->mouse_press(event)) return;
        if (event->button() == Qt::LeftButton) {
            window_dragging = true;
            pin_window->start_window_drag(event->globalPosition().toPoint());
            event->accept();
            return;
        }
    }
    CanvasView::mousePressEvent(event);
}

void PinCanvasView::mouseMoveEvent(QMouseEvent *event) {
    if (pin_window->hover_controls) pin_window->hover_controls->set_pin_hovered(true);
    if (window_dragging) {
        pin_window->update_window_drag(event->globalPosition().toPoint());
        event->accept();
        return;
    }
    if (!pin_canvas->is_editing() && text_selection && text_selection->mouse_move(event)) return;
    CanvasView::mouseMoveEvent(event);
}

void PinCanvasView::mouseDoubleClickEvent(QMouseEvent *event) {
    if (!pin_canvas->is_editing() && text_selection && text_selection->mouse_double_click(event)) return;
    CanvasView::mouseDoubleClickEvent(event);
}

void PinCanvasView::drawForeground(QPainter *painter, const QRectF &rect) {
    CanvasView::drawForeground(painter, rect);
    if (text_selection) text_selection->paint(painter);
}

void PinCanvasView::mouseReleaseEvent(QMouseEvent *event) {
    if (!pin_canvas->is_editing() && text_selection && text_selection->mouse_release(event)) return;
    if (window_dragging && event->button() == Qt::LeftButton) {
        window_dragging = false;
        pin_window->end_window_drag();
        event->accept();
        return;
    }
    if (event->button() == Qt::RightButton) {
        pin_window->show_context_menu(event->globalPosition().toPoint());
        event->accept();
        return;
    }
    CanvasView::mouseReleaseEvent(event);
}

void PinCanvasView::wheelEvent(QWheelEvent *event) {
    if (!pin_canvas->is_editing()) {
        event->ignore();
        return;
    }
    CanvasView::wheelEvent(event);
}

void PinCanvasView::keyPressEvent(QKeyEvent *event) {
    if (!pin_canvas->is_editing() && text_selection && text_selection->key_press(event)) return;
    if (event->key() == Qt::Key_Escape && pin_canvas->is_editing()) {
        // Esc while editing leaves the tool and keeps the window.
        pin_canvas->deactivate_tool();
        auto *toolbar = pin_window->toolbar;
        if (toolbar && toolbar->current_tool) {
            for (auto *button : toolbar->tool_buttons) button->setChecked(false);
            toolbar->current_tool.reset();
        }
        event->accept();
        return;
    }
    CanvasView::keyPressEvent(event);
}
}
