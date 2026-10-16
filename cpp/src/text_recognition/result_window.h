#pragma once
#include "ocr/ocr_manager.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/frameless.h"
#include "ui/toast.h"
#include <QImage>
#include <QPointer>
#include <QRect>
#include <optional>

class QMenu;

namespace jietuba {
class CaptionLabel;
class CheckBox;
class RecognizeThread;
class TextEdit;
class TitleBarPinButton;

// The footer's layout entry; a drop-down arrow on the right marks it as a menu, not a toggle.
class LayoutMenuButton : public TransparentPushButton {
    Q_OBJECT
public:
    explicit LayoutMenuButton(const QString &text, QWidget *parent = nullptr);
protected:
    QString style_sheet() const override;
    void paintEvent(QPaintEvent *event) override;
};

// Text recognition result: the recognized text in an editable box with a copy button.
//
// Recognition runs on a worker thread and the window shows "Recognizing..." first:
// the screenshot UI has already closed, and an empty screen is a worse wait. The
// text is editable once in, since recognition errors are easier to fix in place.
//
// Layout re-renders from the original lines every time, so switching modes never
// drifts. Re-rendering goes through the undo stack: after the user edits and then
// changes the layout, one Ctrl+Z brings the edits back.
//
// Closing only drops the result; the thread runs to completion (see recognizer).
class TextRecognitionWindow : public FrostedFramelessDialog {
    Q_OBJECT
public:
    static constexpr int WIDTH = 520;
    static constexpr int HEIGHT = 420;
    // image is the selection's base image; recognition starts on construction.
    explicit TextRecognitionWindow(const QImage &image, QWidget *parent = nullptr, const std::optional<QRect> &anchor = std::nullopt);
    TitleBarPinButton *pin_button;
    CaptionLabel *status_label;
    TextEdit *text_edit;
    LayoutMenuButton *layout_button;
    PrimaryPushButton *copy_button;
    CheckBox *copy_directly_check;
protected:
    void showEvent(QShowEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
private:
    void apply_theme();
    // Places by the configured position, shrinking on small screens.
    void place();
    void on_toggle_pin(bool checked);
    void update_pin_tooltip();
    void show_layout_menu();
    // Rebuilt on every open from the current settings, so check states need no syncing.
    QMenu *build_layout_menu();
    void set_layout_mode(const QString &mode);
    void set_remove_spaces(bool enabled);
    void set_punctuation(const QString &punctuation);
    void relayout();
    // Replaces the text through the cursor rather than setPlainText, so re-rendering
    // enters the undo stack; also updates the character count.
    void show_result(const QString &text);
    void on_recognized(const QVector<OcrTextLine> &lines, const QString &reason);
    void copy();

    std::optional<QRect> anchor_;
    QVector<OcrTextLine> lines_;
    QPointer<RecognizeThread> thread_;
};

// Shows "Recognizing..." while recognizing, then copies the result and shows the
// count; on failure the clipboard is left alone.
class RecognitionToast : public Toast {
    Q_OBJECT
public:
    explicit RecognitionToast(const QImage &image);
private:
    void on_recognized(const QVector<OcrTextLine> &lines, const QString &reason);
};

// Recognizes image and opens the result window; the screenshot toolbar's text
// recognition button. anchor is the recognized region on screen (global), which
// the window position setting refers to.
TextRecognitionWindow *show_text_recognition(const QImage &image, const std::optional<QRect> &anchor = std::nullopt);
// Recognizes image and copies the text, reporting only next to the cursor; used
// when the "copy recognized text directly" shortcut behaviour is on.
RecognitionToast *copy_text_recognition(const QImage &image);
}
