#pragma once
#include <QPointer>
#include <QRect>
#include <QTextEdit>
#include <QWidget>
#include <optional>

namespace jietuba {
class OcrTextLayout;
class PinWindow;
class ToolSettingsManager;

// The recognized text laid out by the text recognition window's settings.
QString pin_layout_text(const OcrTextLayout &layout, ToolSettingsManager *settings);
// The pixel size that shows text about as large as the recognized lines appear on the pin.
int pin_text_pixel_size(const OcrTextLayout &layout, double view_scale);

// Plain text only. The wheel scrolls and Ctrl+wheel sizes the text, never zooming the pin;
// the context menu is the pin's.
class PinTextEdit : public QTextEdit {
    Q_OBJECT
public:
    PinTextEdit(PinWindow *pin, QWidget *parent);
    PinWindow *pin() const { return pin_; }
protected:
    void wheelEvent(QWheelEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
private:
    PinWindow *pin_;
};

// An opaque page over the whole pin holding the editor; its margin moves the window.
class PinTextPanel : public QWidget {
    Q_OBJECT
public:
    explicit PinTextPanel(PinWindow *pin);
protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
private:
    PinWindow *pin_;
};

// The pin shown as text in place of its image. It opens read-only, where the mouse selects
// and the pin's keys keep working; editing is a step further in, so plain keys are typed only
// then. The editor is kept while the pin lives, so switching back and forth keeps the edits
// and their undo history.
class PinTextMode {
public:
    explicit PinTextMode(PinWindow *win) : win_(win) {}
    bool active() const { return active_; }
    bool editing() const { return editing_; }
    PinTextEdit *editor() const { return editor_; }
    // Recognizes first when the pin has no text yet, then switches once it has.
    void enter();
    void exit();
    void toggle() { active_ ? exit() : enter(); }
    // The editor takes the keyboard, keeping the selection or else putting the caret at the end.
    void start_editing();
    void stop_editing();
    // Follows the window's size.
    void sync_geometry();
    QString text() const;
    // The selected text, or all of it without a selection.
    QString text_to_copy() const;
private:
    void show_text();
    void build(const OcrTextLayout &layout);
    void set_read_only(bool read_only);
    // On entry a pin too short for its text grows downwards to fit, within its screen.
    void fit_height();
    PinWindow *win_;
    QPointer<PinTextPanel> panel_;
    QPointer<PinTextEdit> editor_;
    bool active_ = false;
    bool editing_ = false;
    bool waiting_ = false;
    std::optional<QRect> prev_geometry_;
    QPoint entry_pos_;
};
}
