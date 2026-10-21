#include "gif/drawing_view.h"
#include "canvas/items/background_item.h"
#include "canvas/items/selection_item.h"
#include "canvas/selection_model.h"
#include "canvas/smart_edit_controller.h"
#include "core/logger.h"
#include "core/ui_theme.h"
#include "tools/controller.h"
#include <QBrush>
#include <QGraphicsRectItem>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace jietuba {
namespace {
void set_click_through(HWND hwnd, bool enable) {
    LONG style = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (enable) style |= WS_EX_TRANSPARENT;
    else style &= ~WS_EX_TRANSPARENT;
    SetWindowLongW(hwnd, GWL_EXSTYLE, style);
    // Without SWP_NOACTIVATE the layer would take the focus when it starts letting the mouse through;
    // drawing activates it explicitly.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
}

GifDrawingScene::GifDrawingScene(int width, int height) : CanvasScene(QImage(), QRectF(0, 0, width, height)) {
    // No background or selection frame; an always-confirmed selection skips selection creation.
    background->hide();
    selection_item->hide();
    selection_model->initialize_confirmed_rect(QRectF());
    // What lies under the layer is the live desktop, not an image in the scene.
    highlighters_multiply = false;

    hit_rect = new QGraphicsRectItem(QRectF(0, 0, width, height));
    hit_rect->setBrush(QBrush(QColor(0, 0, 0, 1)));
    hit_rect->setPen(QPen(Qt::NoPen));
    hit_rect->setZValue(-1000);
    hit_rect->setVisible(false);
    addItem(hit_rect);
}

void GifDrawingScene::resize_scene(int width, int height) {
    scene_rect = QRectF(0, 0, width, height);
    setSceneRect(scene_rect);
    hit_rect->setRect(scene_rect);
}

void GifDrawingScene::set_hit_test_visible(bool visible) { hit_rect->setVisible(visible); }

bool GifDrawingScene::is_drawing(QGraphicsItem *item) const {
    return item != hit_rect && item != background && item != selection_item && !item->data(TRANSIENT_ITEM_DATA).toBool();
}

GifDrawingShortcutHandler::GifDrawingShortcutHandler(GifDrawingView *view)
    : view_(view), bindings_(load_inapp_bindings({"inapp_delete"})), mouse_bindings_(load_inapp_mouse_bindings({"inapp_delete"})) {}

bool GifDrawingShortcutHandler::is_active() { return view_ && view_->isVisible() && !view_->passthrough(); }

bool GifDrawingShortcutHandler::match(QInputEvent *event, const QString &cfg_key) const {
    return match_inapp_binding(event, cfg_key, bindings_, mouse_bindings_);
}

bool GifDrawingShortcutHandler::handle_key(QInputEvent *event) {
    // Mouse events have no key (Key_unknown), so the key-only branches skip them.
    const int key = event_key(event);
    if (key == Qt::Key_Escape) {
        view_->activate_tool("cursor");
        return true;
    }
    if (event->modifiers() & Qt::ControlModifier) {
        if (key == Qt::Key_Z) {
            view_->undo_stack()->undo();
            return true;
        }
        if (key == Qt::Key_Y) {
            view_->undo_stack()->redo();
            return true;
        }
    }
    // Deletes the selected item.
    if (match(event, "inapp_delete")) {
        if (view_->smart_edit_controller) view_->smart_edit_controller->delete_selected();
        return true;
    }
    return false;
}

GifDrawingView::GifDrawingView(const QRect &rect, bool own_shortcuts) : CanvasView(new GifDrawingScene(rect.width(), rect.height())) {
    gif_scene_ = static_cast<GifDrawingScene *>(canvas_scene);
    // Deleted after the view base, which still reaches the scene while cleaning up.
    gif_scene_->setParent(this);

    // A separate transparent top-level window instead of CanvasView's window attributes.
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground);
    set_own_style(this, "background: transparent;");
    setFocusPolicy(Qt::StrongFocus);
    setRenderHint(QPainter::Antialiasing);

    // GIF scene coordinates start at (0, 0), so the identity transform replaces CanvasView's offset.
    resetTransform();

    gif_scene_->tool_controller->add_tool_changed_callback([this](const QString &tool_id) { on_gif_tool_changed(tool_id); });

    setGeometry(rect);
    apply_passthrough(true);

    if (own_shortcuts) {
        shortcut_handler_ = std::make_unique<GifDrawingShortcutHandler>(this);
        ShortcutManager::instance()->register_handler(shortcut_handler_.get());
    }
}

GifDrawingView::~GifDrawingView() {
    if (shortcut_handler_) ShortcutManager::instance()->unregister_handler(shortcut_handler_.get());
}

CommandUndoStack *GifDrawingView::undo_stack() const { return gif_scene_->undo_stack; }

void GifDrawingView::update_rect(const QRect &rect) {
    setGeometry(rect);
    gif_scene_->resize_scene(rect.width(), rect.height());
}

void GifDrawingView::activate_tool(const QString &tool_id) { gif_scene_->tool_controller->activate(tool_id); }

void GifDrawingView::set_passthrough(bool enable) {
    if (enable == passthrough_) return;
    apply_passthrough(enable);
}

void GifDrawingView::clear_all() {
    // Only annotations (top-level tracked items) go; tool helpers such as the brush indicator stay
    // because their tools keep using them. References keep the removed items alive until the
    // undo stack lets go of its own.
    std::vector<ItemReference> removed;
    for (auto *item : gif_scene_->items()) {
        if (item->parentItem() || !gif_scene_->is_drawing(item)) continue;
        auto *tracked = dynamic_cast<DrawingItemMixin *>(item);
        if (!tracked) continue;
        removed.emplace_back(tracked);
        gif_scene_->removeItem(item);
    }
    gif_scene_->undo_stack->clear();
    removed.clear();
    if (smart_edit_controller) smart_edit_controller->clear_selection();
}

bool GifDrawingView::has_drawings() const {
    for (auto *item : gif_scene_->items())
        if (gif_scene_->is_drawing(item)) return true;
    return false;
}

void GifDrawingView::apply_passthrough(bool enable) {
    passthrough_ = enable;
    gif_scene_->set_hit_test_visible(!enable);
    set_click_through(reinterpret_cast<HWND>(winId()), enable);
    if (!enable) {
        activateWindow();
        raise();
    }
    emit passthrough_changed(enable);
    if (enable) log_debug(T("绘制层穿透=开"), "GIF");
    else log_debug(T("绘制层穿透=关"), "GIF");
}

void GifDrawingView::on_gif_tool_changed(const QString &tool_id) {
    apply_passthrough(tool_id == "cursor");
    emit tool_changed(tool_id);
}

void GifDrawingView::mousePressEvent(QMouseEvent *event) {
    if (passthrough_) {
        event->ignore();
        return;
    }
    CanvasView::mousePressEvent(event);
}

void GifDrawingView::mouseMoveEvent(QMouseEvent *event) {
    if (passthrough_) {
        event->ignore();
        return;
    }
    CanvasView::mouseMoveEvent(event);
}

void GifDrawingView::mouseReleaseEvent(QMouseEvent *event) {
    if (passthrough_) {
        event->ignore();
        return;
    }
    CanvasView::mouseReleaseEvent(event);
}

void GifDrawingView::keyPressEvent(QKeyEvent *event) {
    if (passthrough_) {
        event->ignore();
        return;
    }
    CanvasView::keyPressEvent(event);
}
}
