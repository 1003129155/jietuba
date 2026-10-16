#pragma once
#include <QAbstractButton>
#include <QColor>
#include <QDialog>
#include <QHBoxLayout>
#include <QWidget>

namespace jietuba {
class TitleBar;

// Native frameless window: the system frame is removed in WM_NCCALCSIZE while
// WS_THICKFRAME/WS_CAPTION stay, so Windows still provides the shadow, snap
// layouts and minimise/maximise animations. Resize borders come from WM_NCHITTEST.
class FramelessHelper {
public:
    static constexpr int BORDER_WIDTH = 5;
    explicit FramelessHelper(QWidget *window) : window_(window) {}
    void install();
    bool native_event(const QByteArray &event_type, void *message, qintptr *result);
    bool resize_enabled = true;
private:
    QWidget *window_;
};

class FramelessWindow : public QWidget {
    Q_OBJECT
public:
    explicit FramelessWindow(QWidget *parent = nullptr);
    void setTitleBar(TitleBar *title_bar);
    TitleBar *titleBar() const { return title_bar_; }
    void setResizeEnabled(bool enabled) { helper_.resize_enabled = enabled; }
protected:
    bool nativeEvent(const QByteArray &event_type, void *message, qintptr *result) override;
    void resizeEvent(QResizeEvent *event) override;
    TitleBar *title_bar_ = nullptr;
    FramelessHelper helper_;
};

// Frameless dialog: like FramelessWindow but not resizable and with only a close button.
class FramelessDialog : public QDialog {
    Q_OBJECT
public:
    explicit FramelessDialog(QWidget *parent = nullptr);
    void setTitleBar(TitleBar *title_bar);
    TitleBar *titleBar() const { return title_bar_; }
    void setResizeEnabled(bool enabled) { helper_.resize_enabled = enabled; }
protected:
    bool nativeEvent(const QByteArray &event_type, void *message, qintptr *result) override;
    void resizeEvent(QResizeEvent *event) override;
    TitleBar *title_bar_ = nullptr;
    FramelessHelper helper_;
};

// Frameless dialog with one consistent opaque background (paint_solid_background).
class FrostedFramelessDialog : public FramelessDialog {
    Q_OBJECT
public:
    explicit FrostedFramelessDialog(QWidget *parent = nullptr);
protected:
    void paintEvent(QPaintEvent *event) override;
    int frosted_corner_radius_ = 8;
};

// A caption button with configurable colours per state.
class TitleBarButton : public QAbstractButton {
    Q_OBJECT
public:
    explicit TitleBarButton(QWidget *parent = nullptr);
    void setNormalColor(const QColor &c) { normal_color_ = c; update(); }
    void setHoverColor(const QColor &c) { hover_color_ = c; update(); }
    void setPressedColor(const QColor &c) { pressed_color_ = c; update(); }
    void setNormalBackgroundColor(const QColor &c) { normal_bg_ = c; update(); }
    void setHoverBackgroundColor(const QColor &c) { hover_bg_ = c; update(); }
    void setPressedBackgroundColor(const QColor &c) { pressed_bg_ = c; update(); }
protected:
    // (glyph colour, background) for the current state.
    std::pair<QColor, QColor> colors() const;
    // The base draws only the state background; subclasses add their glyph.
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
private:
    QColor normal_color_ = QColor(0, 0, 0), hover_color_ = QColor(0, 0, 0), pressed_color_ = QColor(0, 0, 0);
    QColor normal_bg_ = QColor(0, 0, 0, 0), hover_bg_ = QColor(0, 0, 0, 26), pressed_bg_ = QColor(0, 0, 0, 51);
};

// Title bar base: drags the window, double click toggles maximise, and hosts
// the minimise/maximise/close buttons in hBoxLayout.
class TitleBar : public QWidget {
    Q_OBJECT
public:
    explicit TitleBar(QWidget *parent);
    QHBoxLayout *hBoxLayout;
    TitleBarButton *minBtn = nullptr;
    TitleBarButton *maxBtn = nullptr;
    TitleBarButton *closeBtn = nullptr;
    void setDoubleClickEnabled(bool enabled) { double_click_enabled_ = enabled; }
protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    // Whether the press lands on the bar itself rather than one of its buttons.
    virtual bool is_drag_region(const QPoint &pos) const;
    virtual void update_max_state();
private:
    bool double_click_enabled_ = true;
};

void toggle_max_state(QWidget *window);
}
