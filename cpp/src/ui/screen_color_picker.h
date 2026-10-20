#pragma once
#include <QColor>
#include <QImage>
#include <QPointer>
#include <QRectF>
#include <QWidget>
#include <functional>
#include <utility>

namespace jietuba {
class MagnifierOverlay;

// Eyedropper over a frozen capture of the whole virtual desktop. The screen keeps its look
// while the capture magnifier follows the pointer; the window closes itself when done.
class ScreenColorPicker : public QWidget {
    Q_OBJECT
public:
    using CaptureFunction = std::function<std::pair<QImage, QRectF>()>;
    // Parentless: an owned window would always cover its owner, and a window-modal dialog would block it.
    ScreenColorPicker();
    // Captures the desktop and shows; false when the capture fails.
    bool start();
    // window stays visible above the eyedropper (its dialog), without taking the keyboard.
    void keep_above(QWidget *window);
    QColor color_at(const QPoint &global) const;
    // Replaces the desktop capture; empty restores it.
    static void set_capture_for_test(CaptureFunction capture);
    MagnifierOverlay *magnifier = nullptr;
signals:
    void hovered(const QColor &color);
    // The pointer moved onto the window kept above.
    void left();
    void picked(const QColor &color);
    void cancelled();
protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void changeEvent(QEvent *event) override;
private:
    void take_focus();
    void track(const QPoint &global);
    void finish(const QColor &color);
    void cancel();
    QImage image_;
    QRectF rect_;
    QPointer<QWidget> keep_above_;
    bool finished_ = false;
};
}
