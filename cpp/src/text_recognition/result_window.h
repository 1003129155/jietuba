#pragma once
#include "ocr/ocr_manager.h"
#include "text_recognition/table.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/frameless.h"
#include "ui/toast.h"
#include <QImage>
#include <QPointer>
#include <QRect>
#include <map>
#include <optional>

class QFrame;
class QMenu;
class QSplitter;
class QStackedWidget;
class QTimer;

namespace jietuba {
class CaptionLabel;
class LabeledSwitch;
class OriginalImageView;
class RecognitionStatusIcon;
class RecognizeThread;
class RecognizedTableView;
class ResultZoom;
class SegmentedWidget;
class TextEdit;
class TitleBarPinButton;

class RecognitionCommandButton : public TransparentPushButton {
    Q_OBJECT
public:
    RecognitionCommandButton(const QString &text, const IconSource &icon, bool menu, QWidget *parent = nullptr);
    QSize sizeHint() const override;
protected:
    QString style_sheet() const override;
    void paintEvent(QPaintEvent *event) override;
private:
    bool menu_;
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
// The original image can sit on the left, its text selectable as on a pin; what the result
// side selects is shaded on it. Turning it on or off splits the window's width rather than
// resizing the window; with it on, a new window opens wide enough for both.
//
// The window has one selection at a time: selecting on the image clears the result side's
// selection and the other way round. Copy and the status take whichever selection there is,
// and everything without one.
//
// When the lines form a table, a Text | Table switch appears in the title bar. The table
// view keeps its own edits and undo stack; Copy and copy-on-close take whichever view is
// showing. The view last chosen opens next time a table is found.
//
// Closing only drops the result; the thread runs to completion (see recognizer).
class TextRecognitionWindow : public FrostedFramelessDialog {
    Q_OBJECT
public:
    static constexpr int WIDTH = 718;
    static constexpr int HEIGHT = 580;
    // The result side's width when the window opens with the original image beside it.
    static constexpr int RESULT_WIDTH_BESIDE_IMAGE = 598;
    // image is the selection's base image; recognition starts on construction.
    explicit TextRecognitionWindow(const QImage &image, QWidget *parent = nullptr, const std::optional<QRect> &anchor = std::nullopt);
    TitleBarPinButton *pin_button;
    CaptionLabel *status_label;
    TextEdit *text_edit;
    RecognitionCommandButton *layout_button;
    RecognitionCommandButton *copy_button;
    LabeledSwitch *copy_directly_switch;
    // When enabled, Copy also closes the window.
    LabeledSwitch *close_after_copy_switch;
    // Hidden unless a table is found.
    SegmentedWidget *view_switch;
    RecognizedTableView *table_view;
    // Both null without an image.
    LabeledSwitch *original_switch = nullptr;
    OriginalImageView *original_view = nullptr;
    QSplitter *splitter;
    bool showing_table() const;
    // Shows a recognition result: what the recognition thread reports. reason is empty on
    // success; table is empty unless the lines form one; result places the text on the image.
    void on_recognized(const QVector<OcrTextLine> &lines, const QString &reason, const text_table::Table &table, const OcrResult &result = {});
protected:
    void showEvent(QShowEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
private:
    void apply_theme();
    void apply_text_theme();
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
    void set_original_visible(bool visible);
    // Shades on the image what the text or table selection came from.
    void update_highlight();
    bool result_has_selection() const;
    void on_result_selection_changed();
    void on_image_selection_changed();
    // Keeps text_sources_ in step with edits: typed text comes from nowhere in the image.
    void on_text_changed(int position, int removed, int added);
    // After every change: an undo or redo back to a state seen before takes that state's
    // sources, since reinserted text would otherwise come from nowhere.
    void record_text_state();
    // Widens the window once so the table fits, up to most of the screen.
    void fit_table_width();
    void copy();

    std::optional<QRect> anchor_;
    QVector<OcrTextLine> lines_;
    bool recognized_ = false;
    // Per UTF-16 unit of the text box, the recognized character it shows.
    QVector<OcrCharSource> text_sources_;
    bool filling_text_ = false;
    struct TextState {
        QString text;
        QVector<OcrCharSource> sources;
    };
    // By the number of undo steps behind each state; the text tells a state on another branch.
    std::map<int, TextState> text_states_;
    text_table::Table table_;
    QPointer<RecognizeThread> thread_;
    QStackedWidget *pages_ = nullptr;
    QFrame *table_card_ = nullptr;
    RecognitionStatusIcon *status_icon_ = nullptr;
    QTimer *copy_feedback_timer_ = nullptr;
    ResultZoom *text_zoom_ = nullptr;
    bool table_fitted_ = false;
    // Closing from Copy: the clipboard already holds the result.
    bool closing_after_copy_ = false;
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
