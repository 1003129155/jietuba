#pragma once
#include "ui/base_settings_panel.h"
#include <QIcon>
#include <QStyledItemDelegate>

class QGraphicsItem;

namespace jietuba {
// Base sizes of the style previews: big enough for the head at the preview stroke
// width; the row adds side margins so tips do not touch the row edges.
QSize preview_size();
QSize preview_row_size();
// Paints a real ArrowItem, so previews can never drift from the drawn shapes.
QPixmap render_arrow_style_preview(const QString &style, const QColor &ink, const QSize &size, double ratio = 1.0);
QIcon arrow_style_icon(const QString &style, double ratio = 2.0);

// Centres the preview in each popup row; Qt would push an icon-only item to the left.
class StylePreviewDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

class ArrowSettingsPanel : public QWidget {
    Q_OBJECT
public:
    static constexpr int BASE_MARGIN_H = 12;
    static constexpr int BASE_MARGIN_V = 6;
    static constexpr int BASE_SPACING = 10;
    static constexpr int BASE_SIZE_SPIN_WIDTH = 68;
    static constexpr int BASE_OPACITY_SPIN_WIDTH = 80;
    // Wider than the line-style combo: the collapsed box shows an arrow preview.
    static constexpr int BASE_COMBO_WIDTH = 104;
    explicit ArrowSettingsPanel(QWidget *parent = nullptr);
    void apply_scale();
    void set_color(const QColor &color);
    void set_size(int size);
    void set_opacity(int opacity);
    QString arrow_style() const { return current_arrow_style; }
    // Sets without emitting arrow_style_changed.
    void set_arrow_style(const QString &value);
    QString current_arrow_style;
    int current_size = 3;
    QColor current_color = QColor(Qt::red);
    PillComboBox *arrow_style_combo = nullptr;
    StepperWidget *size_spin = nullptr;
    StepperWidget *opacity_spin = nullptr;
    ColorPickerButton *color_btn = nullptr;
signals:
    void arrow_style_changed(const QString &style);
    void size_changed(int size);
    void color_changed(const QColor &color);
    void opacity_changed(int opacity);
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    void init_ui();
    void connect_signals();
    void on_color_picked(const QColor &color);
    void on_preset_color_clicked(const QString &color);
    static int opacity_to_percent(int opacity);
    static int percent_to_opacity(int percent);
    int cached_opacity_ = 255;
    ColorRow *color_row_ = nullptr;
};
}
