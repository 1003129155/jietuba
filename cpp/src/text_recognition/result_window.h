#pragma once
#include "ocr/ocr_manager.h"
#include "text_recognition/table.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/frameless.h"
#include "ui/toast.h"
#include <QImage>
#include <QPointer>
#include <QRect>
#include <optional>

class QFrame;
class QMenu;
class QStackedWidget;

namespace jietuba {
class CaptionLabel;
class CheckBox;
class RecognizeThread;
class RecognizedTableView;
class SegmentedWidget;
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
// When the lines form a table, a Text | Table switch appears in the title bar. The table
// view keeps its own edits and undo stack; Copy and copy-on-close take whichever view is
// showing. The view last chosen opens next time a table is found.
//
// Closing only drops the result; the thread runs to completion (see recognizer).
class TextRecognitionWindow : public FrostedFramelessDialog {
    Q_OBJECT
public:
    static constexpr int WIDTH = 624;
    static constexpr int HEIGHT = 504;
    // image is the selection's base image; recognition starts on construction.
    explicit TextRecognitionWindow(const QImage &image, QWidget *parent = nullptr, const std::optional<QRect> &anchor = std::nullopt);
    TitleBarPinButton *pin_button;
    CaptionLabel *status_label;
    TextEdit *text_edit;
    LayoutMenuButton *layout_button;
    PrimaryPushButton *copy_button;
    CheckBox *copy_directly_check;
    // Hidden unless a table is found.
    SegmentedWidget *view_switch;
    RecognizedTableView *table_view;
    bool showing_table() const;
    // Shows a recognition result: what the recognition thread reports. reason is empty on
    // success; table is empty unless the lines form one.
    void on_recognized(const QVector<OcrTextLine> &lines, const QString &reason, const text_table::Table &table);
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
    // chosen: the user picked the view, which then opens next time.
    void set_view(const QString &view, bool chosen);
    void update_status();
    // Widens the window once so the table fits, up to most of the screen.
    void fit_table_width();
    void copy();

    std::optional<QRect> anchor_;
    QVector<OcrTextLine> lines_;
    text_table::Table table_;
    QPointer<RecognizeThread> thread_;
    QStackedWidget *pages_ = nullptr;
    QFrame *table_card_ = nullptr;
    bool table_fitted_ = false;
};

// Shows "Recognizing..." while recognizing, then copies the result and shows the
// count; on failure the clipboard is left alone.
class RecognitionToast : public Toast {
    Q_OBJECT
public:
    explicit RecognitionToast(const QImage &image);
private:
    void on_recognized(const QVector<OcrTextLine> &lines, const QString &reason, const text_table::Table &table);
};

// Recognizes image and opens the result window; the screenshot toolbar's text
// recognition button. anchor is the recognized region on screen (global), which
// the window position setting refers to.
TextRecognitionWindow *show_text_recognition(const QImage &image, const std::optional<QRect> &anchor = std::nullopt);
// Recognizes image and copies the text, reporting only next to the cursor; used
// when the "copy recognized text directly" shortcut behaviour is on. A table is copied
// instead when the table view was the last one chosen.
RecognitionToast *copy_text_recognition(const QImage &image);
}
