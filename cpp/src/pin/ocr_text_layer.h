#pragma once
#include "ocr/ocr_manager.h"
#include <QRect>
#include <QRectF>
#include <QWidget>
#include <optional>
#include <utility>
#include <vector>

namespace jietuba {
class PinImageTransform;
class PinWindow;

// One recognized text block. Text is indexed by code point, as the reference indexes strings.
class OCRTextItem {
public:
    // box: four corners in original image coordinates.
    OCRTextItem(const QString &text, const QVector<QPointF> &box, double score);
    // Character positions along the main axis, weighted by width (fullwidth : halfwidth ≈ 2 : 1).
    void calculate_char_positions(const QRect &rect);
    // The nearest character index; uses y for vertical text, x otherwise.
    int get_char_index_at_pos(int x, const QRect &rect, int y = 0) const;
    QRect get_scaled_rect(double scale_x, double scale_y) const;
    // Inside the scaled block, grown by 5 px on every side for easier clicking.
    bool contains(const QPoint &point, double scale_x, double scale_y) const;
    int length() const { return static_cast<int>(chars.size()); }
    // Python-style slice [start:end] by code point.
    QString slice(int start, int end) const;
    QString text;
    QList<uint> chars;
    QVector<QPointF> original_box;
    double score;
    QRectF norm_rect;
    // Kept for remapping after image transforms.
    QRectF orig_norm_rect;
    std::vector<int> char_positions;
    bool is_vertical = false;
};

// A fully transparent, Word-style text selection layer over a pin: an I-beam over text,
// click to place the caret, drag to select across blocks; follows pin scaling and is
// disabled while drawing.
class OCRTextLayer : public QWidget {
    Q_OBJECT
public:
    explicit OCRTextLayer(PinWindow *parent = nullptr, int original_width = 100, int original_height = 100);
    // Enabled from outside and not drawing.
    bool is_active() const { return enabled && !drawing_mode; }
    void set_drawing_mode(bool active);
    // Toolbar buttons: a drawing tool selected (not actual drawing) disables the layer.
    void set_draw_tool_active(bool active) { set_drawing_mode(active); }
    // Computes positions only when first needed, so loading never stalls the UI.
    void recalculate_char_positions();
    void set_enabled(bool enabled);
    void load_ocr_result(const OcrResult &ocr_result, int original_width, int original_height);
    // Turns a result into blocks and their union (safe on a worker thread).
    static std::pair<std::vector<OCRTextItem>, std::optional<QRectF>> prepare_ocr_items(const OcrResult &ocr_result);
    // Loads prepared blocks on the main thread.
    void load_prepared_ocr_items(std::vector<OCRTextItem> items, const std::optional<QRectF> &union_rect, int original_width, int original_height);
    std::pair<double, double> get_scale_factors() const;
    // Maps every block into the transformed image and updates the reference size, so the
    // scaled-rect code works unchanged.
    void set_image_transform(const PinImageTransform *transform);
    // All text in reading order; one line joined with spaces, lines with separator.
    QString get_all_text(const QString &separator = "\n") const;
    bool has_text() const { return !text_items.empty(); }
    void clear_selection();
    QString get_selected_text() const;
    void copy_selected_text();
    void cleanup();
    int original_width, original_height;
    std::vector<OCRTextItem> text_items;
    bool enabled = true;
    bool drawing_mode = false;
    // (item index, character index)
    std::optional<std::pair<int, int>> selection_start, selection_end;
    bool is_selecting = false;
protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    // Uses Qt's double click rather than timing two presses: the pin's mouse shortcuts
    // take double clicks first in the event filter, and the system interval applies.
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    // Only marks positions stale; recomputed at once while a selection is shown.
    void resizeEvent(QResizeEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
private:
    bool is_parent_dragging() const;
    // Shown whenever active: without text, events pass through anyway.
    void apply_effective_enabled();
    void ensure_char_positions();
    bool is_pos_on_text(const QPoint &pos) const;
    // On the pin's close or toolbar button.
    bool on_parent_button(const QPoint &pos) const;
    // The hit block's character; otherwise nothing when strict, or the nearest line when
    // dragging, so selections can cross lines.
    std::pair<std::optional<int>, std::optional<int>> get_char_at_pos(const QPoint &pos, bool strict = false);
    void select_word(int item_idx);
    PinWindow *pin_;
    std::optional<QRectF> text_union_rect_;
    int true_orig_width_, true_orig_height_;
    bool mouse_on_text_ = false;
    bool char_positions_calculated_ = false;
};
}
