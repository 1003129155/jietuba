#pragma once
#include <QColor>
#include <QPushButton>
#include <QVector>

namespace jietuba {
class ColorSwatch;

// Button that opens the colour picker (choose_color) and shows the current colour; round_style
// draws a circle to sit in a row of round preset swatches.
class ColorPickerButton : public QPushButton {
    Q_OBJECT
public:
    explicit ColorPickerButton(const QColor &initial_color, bool show_alpha = true, int size = 28, bool rainbow_ring = true,
                               bool round_style = false, QWidget *parent = nullptr);
    QColor color() const { return color_; }
    // Updates silently, without color_changed.
    void set_color(const QColor &color);
    // Registers the preset swatches of the same row. Each colour change marks
    // the matching swatch, or this button when none matches (alpha ignored).
    void link_swatches(const QVector<ColorSwatch *> &swatches);
signals:
    void color_changed(const QColor &color);
protected:
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
private:
    void sync_swatches();
    void pick_color();
    void refresh();
    void paint_round(QPainter *painter);
    void update_style();
    QColor color_;
    bool show_alpha_, rainbow_ring_, round_style_;
    QVector<ColorSwatch *> swatches_;
    bool custom_active_ = false;
};
}
