#pragma once
#include "ui/fluent_lite/frameless.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/titlebar.h"
#include <QAbstractButton>
#include <QColor>
#include <QComboBox>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace jietuba {
class EllipsisAnimator;
class SpeechService;

// Rectangular surfaces keep small radii; large radii on panes and popups look
// swollen above 100% scale. Pills and circles use their own half-height radii.
constexpr int WINDOW_CORNER_RADIUS = 8;
constexpr int SURFACE_CORNER_RADIUS = 8;
constexpr int CONTROL_CORNER_RADIUS = 6;
// Maximum (unscaled) width of the engine name badge; custom names are user-chosen
// and would otherwise distort the title bar or footer.
constexpr int BADGE_MAX_WIDTH = 220;

// Elides text beyond BADGE_MAX_WIDTH and puts the full name in the tooltip.
// padding is the stylesheet's horizontal padding per side (unscaled).
void set_badge_text(QLabel *badge, const QString &text, int padding);

struct DashboardPalette {
    QString window, field, surface_strong, text, text_2, text_3, accent, accent_hover, accent_tint, fill, fill_hover;
    QColor fill_color, fill_hover_color;
    QString danger, green;
};
const DashboardPalette &dashboard_dark();
const DashboardPalette &dashboard_light();
// Fills a style sheet template: @name is a palette entry, @sN is dialog_scaled(N),
// @stroke one scaled pixel, @control_radius / @surface_radius the scaled radii.
QString fill_dashboard_sheet(QString sheet, const DashboardPalette &p);

// Crisp copy/trash/speak button drawn by Qt instead of a font glyph.
class VectorToolButton : public QAbstractButton {
    Q_OBJECT
public:
    VectorToolButton(const QString &icon_name, const QString &tooltip, QWidget *parent = nullptr);
    void set_tooltip(const QString &tooltip);
    void set_icon(const QString &icon_name);
    QString icon_name() const { return icon_name_; }
    void apply_palette(const DashboardPalette &palette);
protected:
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
private:
    QString icon_name_;
    const DashboardPalette *palette_;
};

// Drives one speak/stop button for one text. Every button has its own owner on the
// process-wide speech service, so starting one silences the others.
class SpeechButton : public QObject {
    Q_OBJECT
public:
    SpeechButton(VectorToolButton *button, std::function<QString()> text, std::function<QString()> lang_code);
    quint64 token() const { return token_; }
    bool speaking() const;
    void refresh();
    void stop();
private:
    SpeechService *service();
    void toggle();

    VectorToolButton *button_;
    std::function<QString()> text_;
    std::function<QString()> lang_code_;
    const quint64 token_;
    SpeechService *service_ = nullptr;
};

// Dashboard translation action.
class TranslateButton : public QAbstractButton {
    Q_OBJECT
public:
    explicit TranslateButton(QWidget *parent = nullptr);
    void retranslate(bool busy);
    void apply_palette(const DashboardPalette &palette);
protected:
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
private:
    const DashboardPalette *palette_;
    QString idle_text_;
};

// Pin toggle wearing the dashboard palette instead of the app theme.
class DashboardPinButton : public TitleBarPinButton {
    Q_OBJECT
public:
    using TitleBarPinButton::TitleBarPinButton;
    void apply_palette(const DashboardPalette &palette);
protected:
    std::pair<QColor, QColor> colors() const override;
private:
    const DashboardPalette *palette_ = &dashboard_dark();
};

// Shared Fluent title bar plus the always-on-top control.
class DashboardTitleBar : public FluentTitleBar {
    Q_OBJECT
public:
    explicit DashboardTitleBar(QWidget *parent);
    void update_translations();
    void apply_palette(const DashboardPalette &palette);
    QLabel *logo;
    QLabel *app_name;
    DashboardPinButton *pin_button;
protected:
    void apply_caption_colors() override;
private:
    const DashboardPalette *palette_ = &dashboard_dark();
};

class SourceEdit : public TextEdit {
    Q_OBJECT
public:
    using TextEdit::TextEdit;
signals:
    void translate_shortcut();
protected:
    void keyPressEvent(QKeyEvent *event) override;
};

// Adds the focus-within state QSS lacks.
class FocusPane : public QFrame {
    Q_OBJECT
public:
    using QFrame::QFrame;
    void watch_focus(QWidget *child) { child->installEventFilter(this); }
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
};

// Translation window keeping the manager-facing API.
class TranslationDialog : public FramelessWindow {
    Q_OBJECT
public:
    static constexpr int MINIMUM_WIDTH = 860;
    static constexpr int MINIMUM_HEIGHT = 560;
    static constexpr int DEFAULT_WIDTH = 1000;
    static constexpr int DEFAULT_HEIGHT = 700;
    explicit TranslationDialog(const QString &original_text = {}, const QString &translated_text = {}, QWidget *parent = nullptr,
                               std::optional<QPoint> position = std::nullopt, const QString &source_lang = "auto", const QString &target_lang = "ZH");
    // The initial size, reduced to the screen when possible.
    static std::pair<int, int> initial_size_for_available_geometry(const QRect &available_geometry);

    void set_translation_result(const QString &translated_text, const QString &detected_lang = {});
    void set_translation_error(const QString &error_msg);
    void set_loading();
    void set_busy(bool busy);
    void on_translation_finished(bool success, const QString &translated_text, const QString &error, const QString &detected_lang = {});
    QString get_source_lang() const;
    QString get_target_lang() const;
    void update_content(const QString &text, const QString &target_lang = {}, const QString &source_lang = {});
    void set_backend_badge(const QString &text, bool configured = true);
    void set_theme(const QString &theme_name);
    quint64 speech_token() const { return target_speech_->token(); }
    quint64 source_speech_token() const { return source_speech_->token(); }
    QString target_text_lang() const { return target_text_lang_; }
    // The source combo's language, else the language the last translation detected for
    // this exact text, else a guess from its script.
    QString source_text_lang() const;

    QString original_text;
    QString translated_text;
    QString source_lang;
    QString target_lang;
    DashboardTitleBar *dashboard_title_bar = nullptr;
    QComboBox *source_language = nullptr;
    QComboBox *target_language = nullptr;
    QPushButton *swap_button = nullptr;
    FocusPane *source_pane = nullptr;
    SourceEdit *source_edit = nullptr;
    VectorToolButton *copy_source_button = nullptr;
    VectorToolButton *clear_source_button = nullptr;
    VectorToolButton *speak_source_button = nullptr;
    QLabel *character_count = nullptr;
    QFrame *target_pane = nullptr;
    TextEdit *target_edit = nullptr;
    VectorToolButton *copy_target_button = nullptr;
    VectorToolButton *speak_target_button = nullptr;
    QLabel *backend_badge = nullptr;
    TranslateButton *translate_button = nullptr;

signals:
    void translate_requested(const QString &text, const QString &source_lang, const QString &target_lang);

protected:
    void showEvent(QShowEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void place_initial_window(std::optional<QPoint> position);
    void build_ui();
    VectorToolButton *tool_button(const QString &icon_name, const QString &tooltip);
    static void set_combo_data(QComboBox *combo, const QString &value, const QString &fallback);
    static QComboBox *language_combo(bool include_auto);
    void swap_languages();
    void request_translation();
    void update_count();
    static QString source_placeholder();
    void retranslate_ui();
    void on_target_lang_changed(int index);
    void on_toggle_pin(bool checked);
    void update_pin_tooltip();
    void copy_source();
    void copy_target();
    QString speakable_text() const;
    void stop_speaking();
    void set_target_state(bool error, bool loading);
    static QString style_sheet(const DashboardPalette &p);

    QString detected_source_lang_;
    QString theme_name_ = "dark";
    const DashboardPalette *palette_ = &dashboard_dark();
    std::vector<VectorToolButton *> tool_buttons_;
    bool is_on_top_ = false;
    bool backend_configured_ = false;
    std::optional<QString> last_error_message_;
    EllipsisAnimator *loading_dots_ = nullptr;
    SpeechButton *source_speech_ = nullptr;
    SpeechButton *target_speech_ = nullptr;
    QString target_text_lang_;
    // The source text the detected language belongs to.
    QString detected_for_text_;
};

// Compatibility class used by TranslationManager.
class TranslationLoadingDialog : public TranslationDialog {
    Q_OBJECT
public:
    explicit TranslationLoadingDialog(const QString &original_text = {}, QWidget *parent = nullptr, std::optional<QPoint> position = std::nullopt,
                                      const QString &source_lang = "auto", const QString &target_lang = "ZH");
};
}
