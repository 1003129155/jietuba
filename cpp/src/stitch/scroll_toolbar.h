#pragma once
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QWidget>
#include <functional>
#include <vector>

class QHBoxLayout;

// The scrolling capture window's draggable floating toolbar: long image size,
// direction, auto scroll, crop, cancel, pin and finish.
namespace jietuba {
// Drag handle at the toolbar's left end: a column of grey dots, never theme coloured.
// Light dots in automatic placement, darker ones in manual placement (double click resets).
class DragHandle : public QWidget {
    Q_OBJECT
public:
    static constexpr int BASE_WIDTH = 14;
    static constexpr int BASE_DOT_RADIUS = 2;
    static constexpr int BASE_DOT_GAP = 7;
    explicit DragHandle(QWidget *parent = nullptr);
    void apply_scale();
    void set_manual_mode(bool manual);
signals:
    // Double click: back to automatic placement.
    void reset_requested();
protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
private:
    bool manual_mode_ = false;
};

class FloatingToolbar : public QWidget {
    Q_OBJECT
public:
    // Base sizes (actual pixels at 100%); button cells match the capture toolbar's.
    static constexpr int BASE_HEIGHT = 40;
    static constexpr int BASE_MIN_WIDTH = 200;
    static constexpr int BASE_BTN_WIDTH = 42;
    static constexpr int BASE_ICON = 24;
    static constexpr int BASE_PILL_HEIGHT = 26;
    explicit FloatingToolbar(QWidget *parent = nullptr);
    // Recomputes sizes for the current scale; direction and button states stay.
    void apply_scale();
    // Updates the direction button's icon and text.
    void update_direction(const QString &direction);
    void set_result_size(int width, int height);
    // The auto scroll button follows the real state (Qt toggles it once on click).
    void set_auto_scrolling(bool running);
    bool manual_positioned() const { return manual_positioned_; }
    // Puts the toolbar back beside the capture window; set by the window.
    std::function<void()> reposition;

    DragHandle *left_handle;
    QLabel *size_label;
    QPushButton *direction_btn;
    QPushButton *auto_scroll_btn;
    QPushButton *crop_btn;
    QPushButton *cancel_btn;
    QPushButton *ocr_btn;
    QPushButton *pin_btn;
    QPushButton *finish_btn;

signals:
    void direction_changed();
    void auto_scroll_clicked();
    // "top" removes the content before the current view, "bottom" after it.
    void crop_requested(const QString &side);
    void ocr_clicked();
    void pin_clicked();
    void finish_clicked();
    void cancel_clicked();

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    // Handles the drag handle's mouse events for dragging; double clicks pass through.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setup_toolbar_window();
    void setup_toolbar_ui();
    // The toolbar touches the capture region; the system shadow would be captured into
    // the long image's top. Turning rounded corners off removes it.
    void drop_system_shadow();
    QPushButton *add_icon_button(QHBoxLayout *layout, const QString &icon, const QString &tooltip, std::function<void()> slot);
    static QString direction_btn_style();
    static QString icon_btn_style();
    QMenu *crop_menu();
    void show_crop_menu();
    // Sets manual or automatic placement and the handle's look.
    void set_manual_positioned(bool manual);
    // Double click on the handle: back to automatic placement, repositioned at once.
    void reset_auto_position();

    bool dragging_ = false;
    QPoint drag_offset_;
    // True after the user drags it, which stops automatic placement.
    bool manual_positioned_ = false;
    QString direction_ = "vertical";
    QHBoxLayout *row_layout_ = nullptr;
    std::vector<QPushButton *> icon_buttons_;
};
}
