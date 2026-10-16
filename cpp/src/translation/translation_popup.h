#pragma once
#include <QFrame>
#include <QLabel>
#include <QPointer>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QTimer>
#include <QWidget>
#include <optional>

namespace jietuba {
class EllipsisAnimator;
class TextEdit;
struct DashboardPalette;

// One compact, always-editable window for both selected and typed text. Entry
// points differ only in whether the popup takes keyboard focus on show
// (activate): selection translation must not steal focus, or the source
// application's selection and caret are lost.
class TranslationPopup : public QWidget {
    Q_OBJECT
public:
    static constexpr int WIDTH = 420;
    // The empty-input layout needs 132 px; a taller floor would only spread blank
    // space above and below the controls.
    static constexpr int MIN_HEIGHT = 132;
    static constexpr int MAX_HEIGHT = 520;
    static constexpr int SOURCE_MIN_HEIGHT = 44;
    static constexpr int SOURCE_MAX_HEIGHT = 120;
    static constexpr int RESULT_MIN_HEIGHT = 44;
    static constexpr int RESULT_MAX_HEIGHT = 208;

    explicit TranslationPopup(QWidget *parent = nullptr);
    // Set the target language from outside.
    void set_target_lang(const QString &lang_code);
    const QString &current_target_lang() const { return target_lang_; }
    // source_text empty means the user will type it, so the result area stays
    // hidden. position defaults to the cursor. activate moves keyboard focus into
    // the popup; keep it false for selection translation.
    void show_popup(const QString &source_text = {}, std::optional<QPoint> position = std::nullopt, bool activate = false);
    void show_result(const QString &translated_text, const QString &detected_lang = {});
    void show_error(const QString &message);
    // Backward-compatible helper for older callers.
    void set_backend_ready(bool ready) { set_backend_status("DeepL API", ready); }
    void set_backend_status(const QString &name, bool ready);
    void set_theme(const QString &theme_name);

    QLabel *backend_badge = nullptr;
    QPushButton *close_button = nullptr;
    TextEdit *source_edit = nullptr;
    QFrame *divider = nullptr;
    TextEdit *result_edit = nullptr;
    QPushButton *lang_button = nullptr;
    QPushButton *copy_button = nullptr;
    QPushButton *full_button = nullptr;

signals:
    void open_full_requested(const QString &source_text, const QString &translated_text, const QString &error_text);
    void manual_translate_requested(const QString &text);
    void manual_input_changed();
    void target_lang_changed(const QString &lang_code);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void build_ui();
    TextEdit *make_text_view(const QString &object_name, bool editable = false);
    void show_lang_menu();
    // Fills the original box without triggering auto-translation.
    void set_source_text(const QString &text);
    // Reveals the result area in its pending state.
    void enter_loading();
    // Collapses the result area while there is nothing to translate.
    void hide_result();
    void focus_source_input();
    void on_source_changed();
    void request_manual_translation();
    void refresh_result_style();
    void fit_text_view(TextEdit *view, int minimum, int maximum);
    void fit_content();
    void place_near(const QPoint &position);
    void show_animated();
    void copy_translation();
    void open_full();
    void hide_popup();

    const DashboardPalette *palette_;
    QString source_text_;
    QString translated_text_;
    QString error_text_;
    // Guards textChanged while the popup fills the box programmatically.
    bool suppress_auto_ = false;
    bool backend_ready_ = true;
    std::optional<QPoint> drag_offset_;
    QString target_lang_ = "ZH";
    EllipsisAnimator *loading_dots_ = nullptr;
    QTimer *manual_debounce_ = nullptr;
    QPropertyAnimation *fade_ = nullptr;
};
}
