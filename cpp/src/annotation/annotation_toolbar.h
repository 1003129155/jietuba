#pragma once
#include "ui/toolbar.h"

namespace jietuba {
class ScreenAnnotation;

// The capture toolbar as the screen annotation's floating window, with its own layout
// (toolbar_layout::ANNOTATION) and pass-through, clear, hide and exit buttons. It never takes the
// focus, so clicking it in pass-through leaves the annotated window active, and it is left out of
// captures and recordings, which keep the drawings without the controls.
class AnnotationToolbar : public Toolbar {
    Q_OBJECT
public:
    explicit AnnotationToolbar(ScreenAnnotation *owner);
    // Bottom centre of the screen under the pointer, above the taskbar.
    void place_initially();
    // Top centre of the screen under the pointer.
    void place_at_top();
    // Where a session starts it: at the bottom centre, or where the last session left it
    // (screen_annotation_toolbar_position), moved back onto a screen when that one is gone.
    void place_at_start();
    // Stores the position for place_at_start.
    void remember_position() const;
    // Edits the annotation toolbar's buttons, as its "..." popup does; for the settings window.
    static void edit_layout(QWidget *parent);
    // Back onto a screen after one is removed or resized; elsewhere a position moved by hand stays.
    void keep_on_screen();
    void set_mode_checks(bool pass_through, bool hidden);
    // The board and highlight buttons show their state; neither is a tool.
    void set_effect_checks(bool board, bool highlight);
    // The settings panels and the "..." popup: top-level windows of their own, created on first use.
    QList<QWidget *> floating_windows() const;
    // Left out of captures, and showing tooltips although they are never the active window.
    void prepare_floating_windows();
    // Closes the toolbar with its floating windows, which do not go with it otherwise.
    void close_with_floating_windows();
    QPushButton *pointer_btn = nullptr, *clear_btn = nullptr, *hide_btn = nullptr, *exit_btn = nullptr;
    QPushButton *board_btn = nullptr, *highlight_btn = nullptr;
signals:
    void pointer_clicked();
    void clear_clicked();
    void hide_clicked();
    void board_clicked();
    void highlight_clicked();
    void exit_clicked();
protected:
    // A top-level window whose host is the layer drawn on.
    QWidget *host_window() const override;
    void reposition_self() override;
    // Double-clicking the drag handle returns to the bottom centre.
    void reset_auto_position() override;
    void showEvent(QShowEvent *event) override;
private:
    ScreenAnnotation *owner_;
    // edit_layout's window, the layout dialog's parent.
    QWidget *layout_dialog_parent_ = nullptr;
};
}
