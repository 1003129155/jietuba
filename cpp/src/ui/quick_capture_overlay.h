#pragma once
#include <QPoint>
#include <QRect>
#include <QRectF>
#include <QWidget>

// Transparent overlay for quick capture: reuses the normal capture's selection frame
// and size info, without the magnifier.
namespace jietuba {
class SelectionInfoPanel;
class SelectionItem;
class SelectionModel;
class SelectionOverlayWidget;
class ToolSettingsManager;

// Physical pixel end points to a selection. The end point is excluded from width and
// height, so the two-point QRect constructor is not used.
QRect selection_rect(const QPoint &start, const QPoint &end, const QRect &bounds);

// The live desktop stays visible; every decoration is the normal capture's own widget.
class QuickCaptureOverlay : public QWidget {
    Q_OBJECT
public:
    explicit QuickCaptureOverlay(ToolSettingsManager *config_manager = nullptr);
    ~QuickCaptureOverlay() override;
    // Updates the capture widgets from absolute physical coordinates; draws no base
    // image or grey mask.
    void show_selection(const QPoint &start, const QPoint &end, const QRect &bounds);
    // Ends this drag and hides. A hidden layered window keeps its last frame, which
    // would flash the previous frame on the next show, so it is cleared first.
    void dismiss();
    void clear();
    ToolSettingsManager *config_manager;
    SelectionModel *model;
    SelectionItem *selection_item;
    SelectionOverlayWidget *selection_overlay;
    SelectionInfoPanel *info_panel;
protected:
    void hideEvent(QHideEvent *event) override;
private:
    QRect selection_rect_;
    bool native_frame_configured_ = false;
    bool session_active_ = false;
    bool hide_info_ = false;
    QRectF bounds_;
};
}
