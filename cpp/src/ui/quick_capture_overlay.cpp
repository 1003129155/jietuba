#include "ui/quick_capture_overlay.h"
#include "canvas/items/selection_item.h"
#include "canvas/selection_model.h"
#include "core/platform_utils.h"
#include "settings/tool_settings.h"
#include "ui/selection_info/panel.h"
#include "ui/selection_overlay.h"
#include <cstdlib>

namespace jietuba {
QRect selection_rect(const QPoint &start, const QPoint &end, const QRect &bounds) {
    const QRect rect(std::min(start.x(), end.x()), std::min(start.y(), end.y()), std::abs(end.x() - start.x()), std::abs(end.y() - start.y()));
    return rect.intersected(bounds);
}

QuickCaptureOverlay::QuickCaptureOverlay(ToolSettingsManager *config) : QWidget(nullptr), config_manager(config ? config : get_tool_settings_manager()) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus |
                   Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setFocusPolicy(Qt::NoFocus);
    setAutoFillBackground(false);

    model = new SelectionModel();
    model->setParent(this);
    model->min_size = QSizeF(0, 0);
    selection_item = new SelectionItem(model);
    selection_overlay = new SelectionOverlayWidget(this, selection_item, model);
    // The size panel only needs coordinate mapping, not a canvas.
    InfoPanelView view{[this] { return static_cast<QWidget *>(this); }, [this](const QPointF &point) { return mapFromGlobal(point.toPoint()); }};
    info_panel = new SelectionInfoPanel(this, view);
}

QuickCaptureOverlay::~QuickCaptureOverlay() {
    // The overlay widget draws the item, so it goes first.
    delete selection_overlay;
    delete selection_item;
}

void QuickCaptureOverlay::show_selection(const QPoint &start, const QPoint &end, const QRect &bounds) {
    if (bounds.isEmpty()) {
        hide();
        return;
    }
    const bool new_session = !session_active_;
    if (new_session) {
        session_active_ = true;
        hide_info_ = config_manager->get_screenshot_info_hide_on_drag();
        info_panel->set_confirmed(false);
        info_panel->apply_scale();
        model->activate();
        model->start_dragging();
    }
    const QRectF scene_bounds(bounds);
    const bool bounds_changed = scene_bounds != bounds_;
    if (bounds_changed) {
        setGeometry(bounds);
        bounds_ = scene_bounds;
        selection_overlay->setGeometry(rect());
    }
    if (new_session) selection_overlay->show();

    const auto selection = selection_rect(start, end, bounds);
    const bool rect_changed = selection != selection_rect_;
    if (new_session || rect_changed) {
        selection_rect_ = selection;
        QRectF requested(selection);
        model->set_rect(requested);
    }
    // A moved desktop origin moves the local drawing even when the absolute selection stays.
    if (bounds_changed) selection_overlay->refresh();
    if (selection.isEmpty()) {
        if (info_panel->isVisible()) info_panel->hide();
    } else if (!hide_info_) {
        // Text and layout only change with the selection, so a still drag lays nothing out.
        if (new_session || rect_changed) info_panel->update_info_text(QRectF(selection));
        if (new_session || rect_changed || bounds_changed) info_panel->follow_rect(QRectF(selection));
        if (!info_panel->isVisible()) {
            info_panel->show();
            info_panel->raise();
        }
    }
    if (!isVisible()) show();
    if (!native_frame_configured_) {
        disable_native_frame(static_cast<quintptr>(winId()));
        // No capture exclusion: remote control software must see the frame too. The
        // controller hides the overlay before grabbing and the worker waits for DwmFlush.
        native_frame_configured_ = true;
    }
}

void QuickCaptureOverlay::dismiss() {
    if (isVisible()) {
        clear();
        repaint();
    }
    hide();
}

void QuickCaptureOverlay::clear() {
    session_active_ = false;
    selection_rect_ = QRect();
    model->stop_dragging();
    model->deactivate();
    selection_overlay->hide();
    info_panel->hide();
}

void QuickCaptureOverlay::hideEvent(QHideEvent *event) {
    clear();
    QWidget::hideEvent(event);
}
}
