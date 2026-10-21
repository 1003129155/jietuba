#pragma once
#include "canvas/items/text_item.h"
#include "canvas/undo.h"
#include <QColor>
#include <QGraphicsScene>
#include <QImage>
#include <QPointer>
#include <memory>
#include <optional>

namespace jietuba {
class BackgroundItem;
class CanvasView;
class SelectionItem;
class SelectionModel;
class ToolController;
class ToolSettingsManager;

// Holds the background, the selection hit item, the annotations, the undo
// stack and the tool controller. Talks to its view only through signals.
class CanvasScene : public QGraphicsScene, public NumberRefreshContext, public TextUndoContext {
    Q_OBJECT
public:
    // Z order: 0 background, 5 mosaic, 6 spotlight curtain, 7 spotlight holes,
    // 10 highlighter, 20 drawings, 30 text, 50 watermark, 101 selection item.
    CanvasScene(const QImage &background_image, const QRectF &scene_rect, bool enable_mosaic = false);
    ~CanvasScene() override;
    QRectF scene_rect;
    SelectionModel *selection_model = nullptr;
    BackgroundItem *background = nullptr;
    SelectionItem *selection_item = nullptr;
    CommandUndoStack *undo_stack = nullptr;
    ToolSettingsManager *tool_settings_manager = nullptr;
    std::unique_ptr<ToolController> tool_controller;
    // Set by the view that shows this scene.
    QPointer<CanvasView> view;
    // Highlighters multiply onto the captured image under them. A layer over the live desktop has none
    // of its pixels to multiply, so there they are laid on translucent instead.
    bool highlighters_multiply = true;

    void confirm_selection();
    // An already confirmed selection, shown and announced like a dragged one.
    void preset_selection(const QRectF &rect);
    void activate_tool(const QString &tool_id);
    // Single entry for colour/width/opacity; emits the cursor refresh signals.
    void update_style(const std::optional<QColor> &color = std::nullopt, const std::optional<double> &width = std::nullopt,
                      const std::optional<double> &opacity = std::nullopt);
    void on_tool_changed(const QString &tool_id);
    // Annotations within rect in drawing order (earliest first).
    QList<QGraphicsItem *> get_drawing_items_in_rect(const QRectF &rect);

    void refresh_number_toolbar(int) override {}
    void refresh_number_cursor_if_active(bool force) override;
    // Text commits and clears on focus-out go to the scene's stack, so they undo.
    QUndoStack *text_undo_stack() const override { return undo_stack; }
signals:
    void selectionConfirmed();
    void cursor_color_update_requested(const QColor &color);
    void cursor_opacity_update_requested();
    void cursor_tool_update_requested(const QString &tool_id, bool force);
    void item_auto_select_requested(QGraphicsItem *item);
    void editing_cleanup_requested();
private:
    void sync_watermark_rect(const QRectF &rect);
    void on_undo_stack_changed();
};
}
