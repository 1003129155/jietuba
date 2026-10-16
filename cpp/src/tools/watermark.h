#pragma once
#include "canvas/items/watermark_item.h"
#include "tools/base.h"

namespace jietuba {
class CanvasScene;

// Selecting the tool only opens the watermark panel; it draws nothing on the
// canvas. The toolbar button adds the watermark (undoable); clearing the text removes it.
class WatermarkTool : public Tool {
public:
    QString id() const override { return "watermark"; }
    // Watermark colour and size are not written to ToolContext; the next tool
    // must not inherit them. The style is stored under the watermark settings.
    void load_settings(ToolContext &) override {}
    void save_settings(ToolContext &) override {}

    static WatermarkStyle load_style(ToolSettingsManager *settings_manager);
    static QString current_template(ToolSettingsManager *settings_manager);
    // Adds the watermark unless the scene has one; returns the scene's watermark.
    static WatermarkItem *add(CanvasScene *scene, bool undoable = true);
    // Saved as the new default and applied to an existing watermark; not undoable, like pen width.
    static WatermarkStyle change_style(CanvasScene *scene, const QVariantMap &changes);
    // Saves the default text; an existing watermark takes it with the current time.
    static WatermarkItem *change_text(CanvasScene *scene, const QString &template_text);
    // Text added automatically to a capture: only with auto enabled and a watermark at the end of the last capture.
    static QString auto_template(ToolSettingsManager *settings_manager);
    // Records whether the capture ended with a non-empty watermark.
    static void remember(CanvasScene *scene);
    // For captures that bypass the canvas (global mouse shortcuts).
    static QImage apply_to_image(const QImage &image, ToolSettingsManager *settings_manager, const QDateTime &when = QDateTime());
};
}
