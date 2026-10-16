#include "pin/ocr_text_layer.h"
#include "core/east_asian_width.h"
#include "core/logger.h"
#include "pin/pin_image_transform.h"
#include "pin/pin_window.h"
#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

namespace jietuba {
OCRTextItem::OCRTextItem(const QString &text, const QVector<QPointF> &box, double score) : text(text), chars(text.toUcs4()), original_box(box), score(score) {
    double min_x = box[0].x(), max_x = box[0].x(), min_y = box[0].y(), max_y = box[0].y();
    for (const auto &p : box) {
        min_x = std::min(min_x, p.x());
        max_x = std::max(max_x, p.x());
        min_y = std::min(min_y, p.y());
        max_y = std::max(max_y, p.y());
    }
    norm_rect = QRectF(min_x, min_y, max_x - min_x, max_y - min_y);
    orig_norm_rect = norm_rect;
}

QString OCRTextItem::slice(int start, int end) const {
    const int n = length();
    start = std::max(0, std::min(start, n));
    end = std::max(0, std::min(end, n));
    if (start >= end) return {};
    const auto part = chars.mid(start, end - start);
    return QString::fromUcs4(reinterpret_cast<const char32_t *>(part.constData()), part.size());
}

void OCRTextItem::calculate_char_positions(const QRect &rect) {
    if (chars.isEmpty()) return;
    // The longer side decides the direction.
    is_vertical = rect.height() > rect.width() * 1.5;
    std::vector<double> weights;
    for (const auto ch : chars) weights.push_back(is_east_asian_wide(ch) ? 2.0 : 1.0);
    double total_weight = 0;
    for (double w : weights) total_weight += w;
    if (total_weight == 0) total_weight = 1.0;
    const int total_size = is_vertical ? rect.height() : rect.width();
    const int start = is_vertical ? rect.y() : rect.x();
    char_positions.clear();
    double cumulative = 0;
    for (size_t i = 0; i < weights.size(); ++i) {
        char_positions.push_back(start + static_cast<int>(cumulative / total_weight * total_size));
        cumulative += weights[i];
    }
    // The far edge of the last character.
    char_positions.push_back(start + total_size);
}

int OCRTextItem::get_char_index_at_pos(int x, const QRect &rect, int y) const {
    if (chars.isEmpty() || char_positions.empty()) return 0;
    const int coord = is_vertical ? y : x;
    const int start = is_vertical ? rect.y() : rect.x();
    const int end = is_vertical ? rect.y() + rect.height() : rect.x() + rect.width();
    if (coord < start) return 0;
    if (coord > end) return length();
    for (size_t i = 0; i < char_positions.size(); ++i) {
        const int char_pos = char_positions[i];
        if (coord < char_pos) {
            if (i > 0) {
                const double mid = (char_positions[i - 1] + char_pos) / 2.0;
                if (coord < mid) return static_cast<int>(i) - 1;
            }
            return static_cast<int>(i);
        }
    }
    return length();
}

QRect OCRTextItem::get_scaled_rect(double scale_x, double scale_y) const {
    return QRect(static_cast<int>(norm_rect.x() * scale_x), static_cast<int>(norm_rect.y() * scale_y), static_cast<int>(norm_rect.width() * scale_x),
                 static_cast<int>(norm_rect.height() * scale_y));
}

bool OCRTextItem::contains(const QPoint &point, double scale_x, double scale_y) const {
    return get_scaled_rect(scale_x, scale_y).adjusted(-5, -5, 5, 5).contains(point);
}

OCRTextLayer::OCRTextLayer(PinWindow *parent, int original_width, int original_height)
    : QWidget(parent), original_width(original_width), original_height(original_height), pin_(parent), true_orig_width_(original_width),
      true_orig_height_(original_height) {
    // The pin's mouse gestures see this layer's events first.
    if (parent) installEventFilter(parent);
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    // Events are received and ignored rather than passed through by attribute, so hovering
    // over text is detectable without raising the layer.
}

bool OCRTextLayer::is_parent_dragging() const { return pin_ && pin_->is_dragging(); }

void OCRTextLayer::set_drawing_mode(bool active) {
    drawing_mode = active;
    apply_effective_enabled();
}

void OCRTextLayer::apply_effective_enabled() {
    if (!is_active()) {
        clear_selection();
        setCursor(Qt::ArrowCursor);
        hide();
    } else if (!isVisible()) {
        show();
        log_debug(T("显示OCR文字层"), "OCRLayer");
    }
}

void OCRTextLayer::recalculate_char_positions() {
    if (text_items.empty()) return;
    const auto [scale_x, scale_y] = get_scale_factors();
    for (auto &item : text_items) item.calculate_char_positions(item.get_scaled_rect(scale_x, scale_y));
    char_positions_calculated_ = true;
}

void OCRTextLayer::ensure_char_positions() {
    // Once, on the first mouse interaction.
    if (!char_positions_calculated_ && !text_items.empty()) recalculate_char_positions();
}

bool OCRTextLayer::is_pos_on_text(const QPoint &pos) const {
    const auto [scale_x, scale_y] = get_scale_factors();
    for (const auto &item : text_items)
        if (item.contains(pos, scale_x, scale_y)) return true;
    return false;
}

void OCRTextLayer::set_enabled(bool value) {
    enabled = value;
    apply_effective_enabled();
}

void OCRTextLayer::load_ocr_result(const OcrResult &ocr_result, int width, int height) {
    text_items.clear();
    original_width = true_orig_width_ = width;
    original_height = true_orig_height_ = height;
    text_union_rect_.reset();
    char_positions_calculated_ = false;
    auto [items, union_rect] = prepare_ocr_items(ocr_result);
    if (items.empty()) return;
    // The OCR module already sorted them into reading order.
    text_items = std::move(items);
    text_union_rect_ = union_rect;
    if (enabled) apply_effective_enabled();
}

std::pair<std::vector<OCRTextItem>, std::optional<QRectF>> OCRTextLayer::prepare_ocr_items(const OcrResult &ocr_result) {
    if (ocr_result.code != 100 || ocr_result.data.isEmpty()) return {{}, std::nullopt};
    std::vector<OCRTextItem> items;
    for (const auto &item : ocr_result.data)
        if (!item.text.isEmpty() && !item.box.isEmpty()) items.emplace_back(item.text, item.box, item.score);
    if (items.empty()) return {{}, std::nullopt};
    double min_x = items[0].norm_rect.left(), min_y = items[0].norm_rect.top(), max_x = items[0].norm_rect.right(), max_y = items[0].norm_rect.bottom();
    for (const auto &item : items) {
        min_x = std::min(min_x, item.norm_rect.left());
        min_y = std::min(min_y, item.norm_rect.top());
        max_x = std::max(max_x, item.norm_rect.right());
        max_y = std::max(max_y, item.norm_rect.bottom());
    }
    return {std::move(items), QRectF(min_x, min_y, max_x - min_x, max_y - min_y)};
}

void OCRTextLayer::load_prepared_ocr_items(std::vector<OCRTextItem> items, const std::optional<QRectF> &union_rect, int width, int height) {
    text_items = std::move(items);
    original_width = true_orig_width_ = width;
    original_height = true_orig_height_ = height;
    text_union_rect_ = union_rect;
    char_positions_calculated_ = false;
    if (enabled) apply_effective_enabled();
}

std::pair<double, double> OCRTextLayer::get_scale_factors() const {
    if (original_width == 0 || original_height == 0) return {1.0, 1.0};
    return {static_cast<double>(width()) / original_width, static_cast<double>(height()) / original_height};
}

void OCRTextLayer::set_image_transform(const PinImageTransform *transform) {
    if (text_items.empty()) return;
    const int orig_w = true_orig_width_, orig_h = true_orig_height_;
    if (!transform->has_transform()) {
        for (auto &item : text_items) item.norm_rect = item.orig_norm_rect;
        original_width = orig_w;
        original_height = orig_h;
    } else {
        for (auto &item : text_items) item.norm_rect = transform->map_ocr_rect(item.orig_norm_rect, orig_w, orig_h);
        const auto [mapped_w, mapped_h] = transform->mapped_image_size(orig_w, orig_h);
        original_width = static_cast<int>(mapped_w);
        original_height = static_cast<int>(mapped_h);
    }
    double min_x = text_items[0].norm_rect.left(), min_y = text_items[0].norm_rect.top(), max_x = text_items[0].norm_rect.right(),
           max_y = text_items[0].norm_rect.bottom();
    for (const auto &item : text_items) {
        min_x = std::min(min_x, item.norm_rect.left());
        min_y = std::min(min_y, item.norm_rect.top());
        max_x = std::max(max_x, item.norm_rect.right());
        max_y = std::max(max_y, item.norm_rect.bottom());
    }
    text_union_rect_ = QRectF(min_x, min_y, max_x - min_x, max_y - min_y);
    char_positions_calculated_ = false;
    clear_selection();
    update();
}

namespace {
// Blocks within the tolerance of the current line's centre join it with spaces.
QString join_lines(const std::vector<std::tuple<QString, double, double>> &blocks, double tolerance_ratio, const QString &separator) {
    double total_height = 0;
    for (const auto &block : blocks) total_height += std::get<2>(block);
    const double tolerance = total_height / blocks.size() * tolerance_ratio;
    QStringList lines, current;
    std::optional<double> current_y;
    for (const auto &[text, center_y, height] : blocks) {
        if (!current_y) {
            current = {text};
            current_y = center_y;
        } else if (std::abs(center_y - *current_y) <= tolerance) {
            current.append(text);
        } else {
            lines.append(current.join(' '));
            current = {text};
            current_y = center_y;
        }
    }
    if (!current.isEmpty()) lines.append(current.join(' '));
    return lines.join(separator);
}
}

QString OCRTextLayer::get_all_text(const QString &separator) const {
    if (text_items.empty()) return {};
    if (text_items.size() <= 1) return text_items[0].text;
    std::vector<std::tuple<QString, double, double>> blocks;
    for (const auto &item : text_items) blocks.emplace_back(item.text, item.norm_rect.y() + item.norm_rect.height() / 2, item.norm_rect.height());
    return join_lines(blocks, 0.8, separator);
}

void OCRTextLayer::clear_selection() {
    selection_start.reset();
    selection_end.reset();
    is_selecting = false;
    update();
}

QString OCRTextLayer::get_selected_text() const {
    if (!selection_start || !selection_end) return {};
    auto [start_item_idx, start_char_idx] = *selection_start;
    auto [end_item_idx, end_char_idx] = *selection_end;
    if (start_item_idx > end_item_idx || (start_item_idx == end_item_idx && start_char_idx > end_char_idx)) {
        std::swap(start_item_idx, end_item_idx);
        std::swap(start_char_idx, end_char_idx);
    }
    std::vector<std::tuple<QString, double, double>> selected;
    for (int idx = start_item_idx; idx <= end_item_idx; ++idx) {
        if (idx >= static_cast<int>(text_items.size())) break;
        const auto &item = text_items[idx];
        QString text;
        if (idx == start_item_idx && idx == end_item_idx) text = item.slice(start_char_idx, end_char_idx);
        else if (idx == start_item_idx) text = item.slice(start_char_idx, item.length());
        else if (idx == end_item_idx) text = item.slice(0, end_char_idx);
        else text = item.text;
        if (!text.isEmpty()) selected.emplace_back(text, item.norm_rect.y() + item.norm_rect.height() / 2, item.norm_rect.height());
    }
    if (selected.empty()) return {};
    if (selected.size() == 1) return std::get<0>(selected[0]);
    return join_lines(selected, 0.5, "\n");
}

void OCRTextLayer::paintEvent(QPaintEvent *) {
    if (!is_active() || !selection_start || !selection_end) return;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    // Translucent blue selection.
    const QColor selection_color(100, 150, 255, 100);
    painter.fillRect(rect(), Qt::transparent);
    auto [start_item_idx, start_char_idx] = *selection_start;
    auto [end_item_idx, end_char_idx] = *selection_end;
    if (start_item_idx > end_item_idx || (start_item_idx == end_item_idx && start_char_idx > end_char_idx)) {
        std::swap(start_item_idx, end_item_idx);
        std::swap(start_char_idx, end_char_idx);
    }
    const auto [scale_x, scale_y] = get_scale_factors();
    for (int idx = start_item_idx; idx <= end_item_idx; ++idx) {
        if (idx >= static_cast<int>(text_items.size())) break;
        const auto &item = text_items[idx];
        const auto r = item.get_scaled_rect(scale_x, scale_y);
        if (item.char_positions.empty()) continue;
        const int last = static_cast<int>(item.char_positions.size()) - 1;
        const int axis_start = item.is_vertical ? r.y() : r.x();
        const int axis_end = item.is_vertical ? r.y() + r.height() : r.x() + r.width();
        int p1, p2;
        if (idx == start_item_idx && idx == end_item_idx) {
            p1 = item.char_positions[std::min(start_char_idx, last)];
            p2 = item.char_positions[std::min(end_char_idx, last)];
        } else if (idx == start_item_idx) {
            p1 = item.char_positions[std::min(start_char_idx, last)];
            p2 = axis_end;
        } else if (idx == end_item_idx) {
            p1 = axis_start;
            p2 = item.char_positions[std::min(end_char_idx, last)];
        } else {
            p1 = axis_start;
            p2 = axis_end;
        }
        // Vertical text selects along y, horizontal along x.
        const auto selection_rect = item.is_vertical ? QRect(r.x(), p1, r.width(), p2 - p1) : QRect(p1, r.y(), p2 - p1, r.height());
        painter.fillRect(selection_rect, selection_color);
    }
}

bool OCRTextLayer::on_parent_button(const QPoint &pos) const {
    if (!pin_) return false;
    for (auto *button : {pin_->close_button(), pin_->toolbar_toggle_button()})
        if (button && button->isVisible() && button->geometry().contains(pos)) return true;
    return false;
}

void OCRTextLayer::mousePressEvent(QMouseEvent *event) {
    if (is_parent_dragging() || !is_active() || event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    const auto pos = event->position().toPoint();
    // Clicks on the pin's buttons belong to them.
    if (on_parent_button(pos)) {
        event->ignore();
        return;
    }
    const auto [item_idx, char_idx] = get_char_at_pos(pos, true);
    if (!item_idx) {
        // A click on empty space clears a selection first, then lets the pin drag.
        if (selection_start || selection_end) clear_selection();
        event->ignore();
        return;
    }
    event->accept();
    setFocus();
    // A single click places the caret and starts a selection; double click selects the block.
    selection_start = std::make_pair(*item_idx, *char_idx);
    selection_end = std::make_pair(*item_idx, *char_idx);
    is_selecting = true;
    update();
}

void OCRTextLayer::mouseDoubleClickEvent(QMouseEvent *event) {
    if (is_parent_dragging() || !is_active() || event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    const auto [item_idx, char_idx] = get_char_at_pos(event->position().toPoint(), true);
    if (!item_idx) {
        event->ignore();
        return;
    }
    event->accept();
    select_word(*item_idx);
    update();
}

void OCRTextLayer::mouseMoveEvent(QMouseEvent *event) {
    if (is_parent_dragging() || !is_active()) {
        is_selecting = false;
        setCursor(Qt::ArrowCursor);
        event->ignore();
        return;
    }
    ensure_char_positions();
    const auto pos = event->position().toPoint();
    if (on_parent_button(pos)) {
        setCursor(Qt::ArrowCursor);
        event->ignore();
        return;
    }
    // Dragging a selection may cross lines, so the lookup is not strict.
    if (is_selecting && selection_start) {
        const auto [item_idx, char_idx] = get_char_at_pos(pos, false);
        if (item_idx) {
            selection_end = std::make_pair(*item_idx, *char_idx);
            update();
        }
        event->accept();
        return;
    }
    // I-beam over text and the event is taken; elsewhere it passes to the pin.
    if (is_pos_on_text(pos)) {
        setCursor(Qt::IBeamCursor);
        mouse_on_text_ = true;
        event->accept();
    } else {
        setCursor(Qt::ArrowCursor);
        mouse_on_text_ = false;
        event->ignore();
    }
}

void OCRTextLayer::mouseReleaseEvent(QMouseEvent *event) {
    if (is_parent_dragging() || !is_active() || event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    if (is_selecting) {
        is_selecting = false;
        event->accept();
    } else {
        event->ignore();
    }
}

std::pair<std::optional<int>, std::optional<int>> OCRTextLayer::get_char_at_pos(const QPoint &pos, bool strict) {
    const auto [scale_x, scale_y] = get_scale_factors();
    // Quick rejection outside the union of all text.
    if (strict && text_union_rect_) {
        const QRect union_rect(static_cast<int>(text_union_rect_->x() * scale_x), static_cast<int>(text_union_rect_->y() * scale_y),
                               static_cast<int>(text_union_rect_->width() * scale_x), static_cast<int>(text_union_rect_->height() * scale_y));
        if (!union_rect.contains(pos)) return {std::nullopt, std::nullopt};
    }
    std::optional<int> nearest_idx;
    std::optional<int> nearest_dist;
    QRect nearest_rect;
    for (int item_idx = 0; item_idx < static_cast<int>(text_items.size()); ++item_idx) {
        auto &item = text_items[item_idx];
        const auto r = item.get_scaled_rect(scale_x, scale_y);
        // Vertical text measures horizontal distance, horizontal text vertical distance.
        const bool is_vert = r.height() > r.width() * 1.5;
        const int dist = is_vert ? std::abs(pos.x() - r.center().x()) : std::abs(pos.y() - r.center().y());
        if (!nearest_dist || dist < *nearest_dist) {
            nearest_dist = dist;
            nearest_idx = item_idx;
            nearest_rect = r;
        }
        if (r.adjusted(-5, -5, 5, 5).contains(pos)) {
            if (item.char_positions.empty()) item.calculate_char_positions(r);
            return {item_idx, item.get_char_index_at_pos(pos.x(), r, pos.y())};
        }
    }
    if (strict) return {std::nullopt, std::nullopt};
    if (nearest_idx) {
        auto &item = text_items[*nearest_idx];
        if (item.char_positions.empty()) item.calculate_char_positions(nearest_rect);
        // Beyond the block: before it is the start, after it the end.
        return {*nearest_idx, item.get_char_index_at_pos(pos.x(), nearest_rect, pos.y())};
    }
    return {std::nullopt, std::nullopt};
}

void OCRTextLayer::select_word(int item_idx) {
    if (item_idx >= static_cast<int>(text_items.size())) return;
    selection_start = std::make_pair(item_idx, 0);
    selection_end = std::make_pair(item_idx, text_items[item_idx].length());
    is_selecting = false;
}

void OCRTextLayer::copy_selected_text() {
    if (!selection_start || !selection_end) return;
    const auto selected_text = get_selected_text();
    if (selected_text.isEmpty()) return;
    QApplication::clipboard()->setText(selected_text);
    const auto chars = selected_text.toUcs4();
    auto preview = QString::fromUcs4(reinterpret_cast<const char32_t *>(chars.constData()), std::min<qsizetype>(chars.size(), 50));
    if (chars.size() > 50) preview += "...";
    log_info(T("已复制: {text_preview}", {{"text_preview", preview}}), "OCRTextLayer");
}

void OCRTextLayer::keyPressEvent(QKeyEvent *event) {
    // Inactive: keys go to the pin.
    if (!is_active()) {
        event->ignore();
        return;
    }
    if ((event->modifiers() & Qt::ControlModifier) && event->key() == Qt::Key_C) {
        copy_selected_text();
        event->accept();
    } else if ((event->modifiers() & Qt::ControlModifier) && event->key() == Qt::Key_A) {
        if (!text_items.empty()) {
            selection_start = std::make_pair(0, 0);
            selection_end = std::make_pair(static_cast<int>(text_items.size()) - 1, text_items.back().length());
            update();
        }
        event->accept();
    } else if (event->key() == Qt::Key_Escape) {
        // Without a selection Esc goes on to close the pin.
        if (selection_start || selection_end) {
            clear_selection();
            event->accept();
        } else {
            event->ignore();
        }
    } else {
        event->ignore();
    }
}

void OCRTextLayer::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    char_positions_calculated_ = false;
    if (is_selecting || selection_start) recalculate_char_positions();
}

void OCRTextLayer::cleanup() {
    text_items.clear();
    char_positions_calculated_ = false;
    clear_selection();
}

void OCRTextLayer::closeEvent(QCloseEvent *event) {
    cleanup();
    QWidget::closeEvent(event);
}
}
