#pragma once
#include "ui/toolbar.h"
#include <QSet>

namespace jietuba {
class PinWindow;
class ToolSettingsManager;

// The capture toolbar adapted for pins:
// 1. its own layout setting (toolbar_layout::PIN), adjusted separately from the capture toolbar
// 2. a separate top-level window kept next to its pin
// 3. draggable by hand; visibility belongs to the pin's PinHoverControls
class PinToolbar : public Toolbar {
    Q_OBJECT
public:
    PinToolbar(PinWindow *parent_pin_window = nullptr, ToolSettingsManager *config_manager = nullptr);
    // Below the pin, right-aligned, counting the tallest panel so an open panel still
    // fits; above when it does not; clamped to the screen otherwise.
    void position_near_window(PinWindow *pin_window);
    // Keeps the toolbar and its panels with the pin after it moves or resizes.
    void sync_with_pin_window();
    // Showing rereads the layout (another pin may have changed it) and snaps next to the pin first.
    void setVisible(bool visible) override;
    PinWindow *parent_pin_window;
    ToolSettingsManager *config_manager;
protected:
    // A top-level window whose parent is null; the host comes from here.
    QWidget *host_window() const override;
    void reposition_self() override;
    // Double-clicking the drag handle returns to snapping.
    void reset_auto_position() override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void show_more_popup() override;
    void hide_more_popup() override;
    void open_layout_dialog() override;
private:
    void set_part_hovered(const QString &part, bool hovered);
    void report_hover(bool hovered);
    // The toolbar and its "..." popup are two top-level windows; the pointer over either counts as over the toolbar.
    QSet<QString> hovered_parts_;
};
}
