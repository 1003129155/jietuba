#pragma once
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QWidget>

namespace jietuba {
class PinWindow;

// Drag to move the pin, through the pin's own drag logic (also blocked when locked).
class PinClickThroughMoveHandle : public QLabel {
public:
    PinClickThroughMoveHandle(QWidget *bar, PinWindow *pin);
protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
private:
    PinWindow *pin_;
};

// While click-through, the pin gets no mouse input: moving, opacity and leaving go
// through this small bar above it. It is a child window of the pin, owned by it at the
// system level, so it sinks below the capture layer together with the pin.
class PinClickThroughBar : public QWidget {
    Q_OBJECT
public:
    static constexpr int BASE_HEIGHT = 34;
    static constexpr int BASE_ICON = 16;
    static constexpr int BASE_SLIDER_WIDTH = 96;
    explicit PinClickThroughBar(PinWindow *pin_window);
    void apply_scale();
    // Moves the slider without writing back to the pin.
    void set_opacity(double opacity);
    // Above the pin's top-left corner; below when there is no room; inside the pin when neither fits.
    void follow_pin();
    // Showing always places the bar first.
    void setVisible(bool visible) override;
    PinWindow *pin_window;
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    void on_slider_changed(int value);
    PinClickThroughMoveHandle *handle_;
    QLabel *title_;
    QSlider *slider_;
    QLabel *value_;
    QPushButton *exit_button_;
    QHBoxLayout *layout_;
};
}
