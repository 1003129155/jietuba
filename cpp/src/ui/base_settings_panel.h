#pragma once
#include <QColor>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPen>
#include <QPixmap>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QWidget>
#include <functional>
#include <optional>

namespace jietuba {
class ColorPickerButton;

constexpr double PANEL_RADIUS = 12.0;
// Frosted panel background shared by settings panels, the screenshot toolbar
// and its "..." popup. Drawn into a pixmap cached on the widget and redrawn
// only when size, DPR or UI scale changes. Corners stay transparent through WA_TranslucentBackground.
void paint_rounded_panel(QWidget *widget);

// Ink for values and line previews, a lighter grey for glyphs and chevrons.
inline const QColor CONTROL_INK = QColor("#2a3243");
inline const QColor CONTROL_SUB = QColor("#6a7387");
inline const QColor CONTROL_FILL = QColor(15, 23, 42, 15);
inline const QColor CONTROL_FILL_HOVER = QColor(15, 23, 42, 26);
inline const QColor DIVIDER_COLOR = QColor(15, 23, 42, 30);

// The theme colour for selected states; user configurable.
QColor accent_color();
void set_step_button_icon(QToolButton *button, const QString &direction);
void style_step_button(QToolButton *button, const QString &direction);
// Collapsed line-style combo preview: one 2 px line, dashed by style.
void paint_line_style_preview(QPainter *painter, const QRectF &rect, const QVariant &style);
// Settings panel style. Widths are 100% base values, scaled now; call again after a scale change.
QString build_settings_panel_stylesheet(bool combo_enabled = false, const QString &combo_padding = "2px 8px 2px 4px",
                                        int combo_min_width = 32, int combo_max_width = 90, bool combo_padding_compact = false);

// Pill-shaped number control with an optional leading glyph ("width" or "opacity"),
// up/down chevrons and wheel support.
class StepperWidget : public QWidget {
    Q_OBJECT
public:
    static constexpr int BASE_HEIGHT = 26;
    static constexpr int BASE_GLYPH_COLUMN = 18;  // 12 glyph + 6 gap
    static constexpr int BASE_ARROW_COLUMN = 16;
    static constexpr int BASE_LABEL_MIN_WIDTH = 26;
    StepperWidget(int value = 0, int minimum = 0, int maximum = 100, const QString &suffix = {}, QWidget *parent = nullptr, const QString &glyph = {});
    void setRange(int minimum, int maximum);
    void setValue(int value);
    int value() const { return value_; }
    void setSuffix(const QString &suffix);
    void setToolTip(const QString &text);
    // Includes the glyph column; already scaled pixels.
    void setFixedWidth(int width);
signals:
    void valueChanged(int value);
protected:
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
private:
    int glyph_width() const;
    QRect pill_rect() const;
    void apply_scale_sizes();
    void layout_children();
    int clamp(int value) const { return std::max(min_, std::min(max_, value)); }
    void step(int delta) { setValue(value_ + delta); }
    void refresh_label();
    int min_, max_, value_;
    QString suffix_, glyph_;
    QLabel *label_;
    QToolButton *up_btn_, *down_btn_;
};

// Pill-shaped combo box drawn collapsed by itself: a preview painter (line style),
// the item icon (arrow style) or the text. The popup list keeps the style sheet.
class PillComboBox : public QComboBox {
    Q_OBJECT
public:
    static constexpr int BASE_HEIGHT = 26;
    static constexpr int BASE_PADDING_LEFT = 14;
    static constexpr int BASE_ARROW_COLUMN = 30;
    explicit PillComboBox(QWidget *parent = nullptr, Qt::Alignment text_alignment = Qt::AlignLeft);
    void set_preview_painter(std::function<void(QPainter *, const QRectF &, const QVariant &)> painter_fn);
protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
private:
    std::function<void(QPainter *, const QRectF &, const QVariant &)> preview_painter_;
    Qt::Alignment text_alignment_;
};

// The same pill background behind a group of controls (number panel's next value).
class PillFrame : public QWidget {
    Q_OBJECT
public:
    static constexpr int BASE_HEIGHT = 26;
    explicit PillFrame(QWidget *parent = nullptr);
    void apply_scale();
protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
};

// Round preset colour. Selected shows a theme ring; the hit area is larger than
// the circle so the ring is never clipped.
class ColorSwatch : public QPushButton {
    Q_OBJECT
public:
    static constexpr int BASE_SIDE = 28;
    static constexpr int BASE_RADIUS = 10;
    static constexpr double BASE_RING_GAP = 3.5;
    explicit ColorSwatch(const QString &color_hex, QWidget *parent = nullptr);
    void apply_scale();
    void set_selected(bool selected);
    bool is_selected() const { return selected_; }
    QString color_hex;
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    QColor color_;
    bool selected_ = false;
};

// Custom-colour button followed by round presets. Every colour entry ends at
// the picker's set_color or its dialog, so selection is decided there once.
class ColorRow : public QWidget {
    Q_OBJECT
public:
    // Swatch hit areas are already 8 px wider than the circles.
    static constexpr int BASE_SPACING = 1;
    ColorRow(ColorPickerButton *picker, const QStringList &colors, std::function<void(const QString &)> on_preset, QWidget *parent = nullptr);
    void apply_scale();
    ColorPickerButton *picker;
    QVector<std::pair<ColorSwatch *, QString>> swatches;
};

inline const QStringList PRESET_COLORS = {"#FF0000", "#FFFF00", "#00FF00", "#0000FF", "#000000", "#FFFFFF"};
QFrame *make_separator();
// Where a hover popup opens: away from the toolbar, flipping to the other side
// when it does not fit. The toolbar registers itself as the panel's owner_toolbar
// property, since the panel's Qt parent is the toolbar's parent.
QPoint popup_position(QWidget *panel, QWidget *anchor, QWidget *popup);

// Small settings layer opened by hovering a panel control. Leaving waits a
// moment so the pointer can cross the gap; entering the popup cancels the close.
class HoverPopup : public QWidget {
    Q_OBJECT
public:
    static constexpr int CLOSE_DELAY_MS = 260;
    explicit HoverPopup(QWidget *parent = nullptr);
    // Style of the popup's own controls, reapplied with the base on scale changes.
    void set_extra_stylesheet(const QString &qss);
    virtual void apply_scale();
    void show_beside(QWidget *panel, QWidget *anchor);
    void keep_open() { close_timer_.stop(); }
    void close_soon() { close_timer_.start(); }
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    // Subclasses overriding apply_scale set this directly: set_extra_stylesheet calls apply_scale.
    QString extra_qss_;
private:
    QTimer close_timer_;
};

// Panels with a line-style selector (pen and shape panels).
class LineStylePanel {
public:
    virtual ~LineStylePanel() = default;
    virtual QString line_style() const = 0;
    // Sets the style without emitting line_style_changed.
    virtual void set_line_style(const QString &value) = 0;
};
// Line style read from a pen: dense dashes are [1, 2], any other dash is "dashed".
QString line_style_of_pen(const QPen &pen);

// Base settings panel: size and opacity steppers, a separator and the colour row.
class BaseSettingsPanel : public QWidget {
    Q_OBJECT
public:
    // 100% base sizes; every scale change recomputes from these.
    static constexpr int BASE_MARGIN_H = 12;
    static constexpr int BASE_MARGIN_V = 6;  // 28 swatch + 6 + 6 = 40, the toolbar height
    static constexpr int BASE_SPACING = 10;
    static constexpr int BASE_SIZE_SPIN_WIDTH = 68;     // 18 glyph + 50 pill
    static constexpr int BASE_OPACITY_SPIN_WIDTH = 80;  // 18 glyph + 62 pill
    static constexpr int OPACITY_DEFAULT = 255;
    explicit BaseSettingsPanel(QWidget *parent = nullptr);
    // Builds the controls. The most derived constructor calls it, so overrides apply.
    virtual void init_ui();
    void apply_scale();
    void set_color(const QColor &color);
    void set_size(int size);
    void set_opacity(int opacity);
    QColor current_color = QColor(255, 0, 0);
    int current_size;
    int current_opacity = OPACITY_DEFAULT;
    StepperWidget *size_spin = nullptr;
    StepperWidget *opacity_spin = nullptr;
    ColorPickerButton *color_picker_btn = nullptr;
signals:
    void color_changed(const QColor &color);
    void size_changed(int size);
    void opacity_changed(int opacity);
protected:
    virtual QString translation_context() const { return "ArrowSettingsPanel"; }
    virtual QString size_glyph() const { return "width"; }
    virtual std::pair<int, int> size_range() const { return {1, 99}; }
    virtual int size_default() const { return 5; }
    virtual const char *size_tooltip() const { return "Line Width"; }
    virtual const char *opacity_tooltip() const { return "Opacity (%)"; }
    virtual void build_size_and_opacity(QHBoxLayout *layout);
    ColorRow *build_color_row();
    virtual void build_extra_controls(QHBoxLayout *layout) { Q_UNUSED(layout); }
    // Override to customise combo parameters; called again after scale changes.
    virtual QString build_stylesheet() const { return build_settings_panel_stylesheet(); }
    // Subclasses extend with their controls and call the base first.
    virtual void apply_scale_sizes();
    QString tr_panel(const char *text) const;
    void on_size_changed(int value);
    void on_opacity_changed(int value);
    void on_color_picked(const QColor &color);
    void apply_preset_color(const QString &color_hex);
    static int opacity_to_percent(int opacity);
    static int percent_to_opacity(int percent);
    void paintEvent(QPaintEvent *event) override;
    ColorRow *color_row_ = nullptr;
};
}
