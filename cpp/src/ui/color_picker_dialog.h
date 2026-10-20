#pragma once
#include "ui/fluent_lite/frameless.h"
#include "ui/screen_color_picker.h"
#include <QColor>
#include <QImage>
#include <QPointer>
#include <QWidget>
#include <functional>
#include <optional>

class QComboBox;
class QFrame;
class QPushButton;
class QToolButton;

namespace jietuba {
class FluentTitleBar;
class LineEdit;
class ToolSettingsManager;

// Saturation (x) by value (y) field of one hue. The handle keeps its own position:
// at the black edge the colour loses its saturation and at greys its hue, the field does not.
class ColorSquare : public QWidget {
    Q_OBJECT
public:
    // Handle radius, and the margin around the field that keeps the handle whole at the edges.
    static constexpr double BASE_HANDLE_RADIUS = 6;
    static constexpr int BASE_INSET = 8;
    explicit ColorSquare(QWidget *parent = nullptr);
    void set_hue(double hue);
    void set_sv(double saturation, double value);
    double hue() const { return hue_; }
    double saturation() const { return saturation_; }
    double value() const { return value_; }
    QRectF field_rect() const;
    QPointF handle_center() const;
signals:
    // User input only; the setters stay silent.
    void sv_changed(double saturation, double value);
protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
private:
    void pick_at(const QPointF &pos);
    void set_and_emit(double saturation, double value);
    double hue_ = 0, saturation_ = 0, value_ = 1;
    bool dragging_ = false;
    // The gradient for one hue and size, rebuilt only when either changes.
    QImage field_cache_;
    double cached_hue_ = -1;
};

// Horizontal track with a draggable thumb: the hue spectrum, or a colour's opacity over a checkerboard.
class ColorSlider : public QWidget {
    Q_OBJECT
public:
    enum Kind { HUE, ALPHA };
    // Row height, track thickness and thumb radius; the track ends sit one inset in from the
    // widget's sides so the thumb stays whole there.
    static constexpr int BASE_HEIGHT = 20;
    static constexpr int BASE_TRACK = 10;
    static constexpr double BASE_THUMB_RADIUS = 6.5;
    static constexpr int BASE_INSET = 7;
    ColorSlider(Kind kind, QWidget *parent = nullptr);
    Kind kind() const { return kind_; }
    void set_value(double value);
    double value() const { return value_; }
    // ALPHA: the colour whose opacity the track shows.
    void set_color(const QColor &color);
    QRectF track_rect() const;
    double thumb_x() const;
signals:
    void value_changed(double value);
protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
private:
    void pick_at(double x);
    void set_and_emit(double value);
    Kind kind_;
    double value_ = 0;
    QColor color_ = Qt::black;
    bool dragging_ = false;
};

// Frameless colour dialog: saturation/value field, hue and opacity tracks beside an eyedropper,
// text entry in Hex/RGB/HSL. Used like QColorDialog; callers go through choose_color.
class ColorPickerDialog : public FrostedFramelessDialog {
    Q_OBJECT
public:
    enum Option { NoOption = 0x0, ShowAlphaChannel = 0x1 };
    Q_DECLARE_FLAGS(Options, Option)
    enum Format { HEX, RGB, HSL };
    struct ParsedColor {
        QColor color;
        // Only #RRGGBBAA carries opacity; other text keeps the current one.
        bool has_alpha = false;
    };
    // Sizes at 100% window scale. The field spans the width less ColorSquare::BASE_INSET on each side.
    static constexpr int BASE_WIDTH = 245;
    static constexpr int BASE_TITLE_HEIGHT = 28;
    static constexpr int BASE_FIELD_HEIGHT = 199;
    static constexpr int BASE_MARGIN = 9;
    static constexpr int BASE_CONTROL_HEIGHT = 30;
    static constexpr int BASE_BUTTON_WIDTH = 60;
    static constexpr int BASE_BUTTON_HEIGHT = 31;
    static constexpr int BASE_FONT = 14;

    explicit ColorPickerDialog(const QColor &initial = Qt::white, Options options = ShowAlphaChannel, QWidget *parent = nullptr);
    ~ColorPickerDialog() override;
    Options options() const { return show_alpha_ ? ShowAlphaChannel : NoOption; }
    QColor currentColor() const { return color_; }
    void setCurrentColor(const QColor &color);
    // The colour accepted with OK; invalid until then.
    QColor selectedColor() const { return selected_; }
    // Below the anchor, above it when there is no room, kept on the anchor's screen.
    void place_near(QWidget *anchor);
    bool picking_screen() const { return !picker_.isNull(); }
    ScreenColorPicker *screen_picker() const { return picker_; }
    // Text in format as one colour; nullopt while incomplete or out of range.
    static std::optional<ParsedColor> parse_color(Format format, const QString &text);
    // hue_fallback (0..1) names the hue of greys, which have none of their own.
    static QString format_color(Format format, const QColor &color, double hue_fallback = 0);

    // Modal; nullopt when cancelled. on_change follows every change while the dialog
    // runs (for a live preview) and gets the initial colour back when it is cancelled.
    static std::optional<QColor> getColor(const QColor &initial, QWidget *anchor = nullptr, const QString &title = {},
                                          Options options = ShowAlphaChannel, const std::function<void(const QColor &)> &on_change = {});

    ColorSquare *square = nullptr;
    ColorSlider *hue_slider = nullptr, *alpha_slider = nullptr;
    QComboBox *format_combo = nullptr;
    // The value and opacity fields share one frame.
    QFrame *value_group = nullptr;
    LineEdit *value_edit = nullptr, *alpha_edit = nullptr;
    QToolButton *eyedropper_button = nullptr;
    QPushButton *ok_button = nullptr, *cancel_button = nullptr;

signals:
    void currentColorChanged(const QColor &color);
    void colorSelected(const QColor &color);

public slots:
    void done(int result) override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // Which control the change came from; that control is not written back.
    enum class Origin { SQUARE, HUE, ALPHA, VALUE_TEXT, ALPHA_TEXT, OTHER };
    Format current_format() const;
    void apply_theme();
    // From the square or the tracks: the colour follows hue, saturation and value.
    void apply_hsv(Origin origin);
    // From text, the eyedropper or a caller: hue and handle positions follow the colour where it has them.
    void apply_color(const QColor &color, Origin origin);
    void commit(const QColor &color, Origin origin);
    void refresh_texts(Origin origin);
    // finished: Enter or leaving the field, when short forms count too.
    void apply_value_text(bool finished);
    std::optional<int> parse_alpha_text() const;
    // A direct edit ends a running eyedropper and keeps what it showed.
    void end_eyedropper_for_edit();
    void start_eyedropper();
    void stop_eyedropper(bool restore);
    // Back to the colour and control positions from before the eyedropper started.
    void restore_before_pick();

    FluentTitleBar *caption_bar_ = nullptr;
    QFrame *separator_ = nullptr, *value_divider_ = nullptr;
    bool show_alpha_ = true;
    double hue_ = 0, saturation_ = 0, value_ = 1;
    QColor color_, selected_, before_pick_;
    double before_hue_ = 0, before_saturation_ = 0, before_value_ = 1;
    QPointer<ScreenColorPicker> picker_;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(ColorPickerDialog::Options)

// Whether colours are chosen in the system dialog (color_picker_style "system") rather than ColorPickerDialog.
bool use_system_color_dialog(ToolSettingsManager *config = nullptr);
// The one entry every colour control opens a picker through; the color_picker_style setting
// picks ColorPickerDialog or the system dialog. Same contract as ColorPickerDialog::getColor.
std::optional<QColor> choose_color(const QColor &initial, QWidget *anchor = nullptr, const QString &title = {},
                                   ColorPickerDialog::Options options = ColorPickerDialog::ShowAlphaChannel,
                                   const std::function<void(const QColor &)> &on_change = {});
}
