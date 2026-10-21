#include "ui/magnifier.h"
#include "canvas/handle_editor.h"
#include "canvas/items/background_item.h"
#include "canvas/scene.h"
#include "canvas/selection_model.h"
#include "canvas/smart_edit_controller.h"
#include "canvas/view.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "core/native.h"
#include "core/theme.h"
#include "core/ui_scale.h"
#include "settings/tool_settings.h"
#include "tools/controller.h"
#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>

namespace jietuba {
namespace {
QString mag_tr(const char *text) { return i18n_tr(text, "MagnifierOverlay"); }
}

MagnifierOverlay::MagnifierOverlay(QWidget *parent, CanvasScene *scene, CanvasView *view, ToolSettingsManager *config_manager)
    : QWidget(parent), scene(scene), view(view), config_manager(config_manager) {
    // setBold rather than a Bold weight avoids font variant issues.
    font_ = QFont("Microsoft YaHei", 10);
    font_.setBold(true);
    info_font_ = QFont("Microsoft YaHei", 11);
    info_font_.setBold(true);
    // Zoom default and range come from the app defaults.
    const auto &defaults = get_tool_settings_manager()->app_defaults();
    const double default_zoom = defaults.value("magnifier_zoom").toDouble();
    zoom_min_ = defaults.value("magnifier_zoom_min").toDouble();
    zoom_max_ = defaults.value("magnifier_zoom_max").toDouble();
    if (config_manager) {
        zoom_factor_ = config_manager->qsettings.value("app/magnifier_zoom", default_zoom).toDouble();
        zoom_factor_ = std::max(zoom_min_, std::min(zoom_max_, zoom_factor_));
    } else {
        zoom_factor_ = default_zoom;
    }
    refresh_theme_colors();
    refresh_display_options();
    // Connection ends with this widget.
    connect(&get_ui_scale(), &UIScaleManager::scale_changed, this, &MagnifierOverlay::apply_scale);
    apply_scale();
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(false);
    // Shown by update_cursor.
    hide();
    raise();
}

void MagnifierOverlay::refresh_theme_colors() {
    const auto tc = get_theme().theme_color();
    QColor tc_semi(tc);
    tc_semi.setAlpha(120);
    pen_teal_2_ = QPen(tc, 2);
    pen_teal_1_ = QPen(tc, 1);
    brush_crosshair_ = QBrush(tc_semi);
    QColor tc_grid(tc);
    // Shows pixel edges without covering the pixel colours.
    tc_grid.setAlpha(160);
    pen_grid_ = QPen(tc_grid, 1);
}

void MagnifierOverlay::refresh_display_options() {
    if (!config_manager) {
        enabled_ = true;
        show_grid_ = false;
        show_hint_ = true;
        formats_ = color_formats::enabled_formats(color_formats::default_formats());
        format_index_ = 0;
        return;
    }
    enabled_ = config_manager->get_app_setting("magnifier_enabled").toBool();
    show_grid_ = config_manager->get_app_setting("magnifier_grid").toBool();
    show_hint_ = config_manager->get_app_setting("magnifier_hint").toBool();
    formats_ = color_formats::enabled_formats(color_formats::load(config_manager));
    // The enabled list may have changed, so cycling restarts at the first format.
    format_index_ = 0;
    // The line count may have changed; font sizes are refitted to the longest line.
    fixed_info_font_.reset();
    fixed_info_metrics_.reset();
}

int MagnifierOverlay::combined_height() const {
    const int lines = 2 + static_cast<int>(hint_texts().size());
    return mag_height_ + info_line_height_ * lines + info_padding_v_;
}

QStringList MagnifierOverlay::hint_texts() const {
    // With a single format Shift does nothing, so that hint is left out.
    if (!show_hint_) return {};
    QStringList hints;
    if (formats_.size() > 1) hints.append(mag_tr("Shift: Switch color format"));
    hints.append(mag_tr("C: Copy color value"));
    return hints;
}

void MagnifierOverlay::clear_sample_cache() {
    last_sample_pt_.reset();
    cached_source_rect_.reset();
    cached_sample_image_ = QImage();
}

void MagnifierOverlay::clear_font_cache() {
    fixed_info_font_.reset();
    fixed_hint_font_.reset();
    fixed_info_metrics_.reset();
    fixed_hint_metrics_.reset();
}

void MagnifierOverlay::apply_scale() {
    mag_width_ = scaled(BASE_MAG_WIDTH);
    mag_height_ = scaled(BASE_MAG_HEIGHT);
    info_width_ = scaled(BASE_INFO_WIDTH);
    info_line_height_ = scaled(BASE_INFO_LINE_HEIGHT);
    info_padding_v_ = scaled(BASE_INFO_PADDING_V);
    edge_margin_ = scaled(BASE_EDGE_MARGIN);
    radius_ = scaled(6);
    // The zoom badge font scales too.
    font_.setPointSize(std::max(1, static_cast<int>(python_round(10 * scale_factor()))));
    // New sizes: font templates and the sample cache are recomputed.
    clear_font_cache();
    clear_sample_cache();
    setFixedSize(info_width_, combined_height());
    update();
}

void MagnifierOverlay::set_sample_image(const QImage &image, const QRectF &global_rect) {
    sample_image_ = image;
    if (image.isNull()) sample_rect_.reset();
    else sample_rect_ = global_rect;
    // The picture changes under a still cursor, so the previous small image cannot be reused.
    clear_sample_cache();
    refresh();
}

void MagnifierOverlay::cycle_color_format() {
    if (formats_.size() <= 1) return;
    format_index_ = (format_index_ + 1) % static_cast<int>(formats_.size());
    update();
}

void MagnifierOverlay::update_cursor(const QPointF &scene_pos) {
    cursor_scene_pos = scene_pos;
    if (!should_render()) {
        if (isVisible()) hide();
        return;
    }
    move(calc_widget_pos(scene_pos));
    if (!isVisible()) {
        show();
        raise();
    }
    // Dirty area is just this widget.
    update();
}

void MagnifierOverlay::clear_cursor() {
    if (!cursor_scene_pos) return;
    cursor_scene_pos.reset();
    hide();
}

void MagnifierOverlay::refresh() {
    if (!cursor_scene_pos) return;
    if (!should_render()) {
        if (isVisible()) hide();
        return;
    }
    update();
}

QString MagnifierOverlay::get_color_info_text() const {
    const auto color = sample_color(background_image());
    const auto formats = !formats_.isEmpty() ? formats_ : color_formats::enabled_formats(color_formats::default_formats());
    const int index = format_index_ < formats.size() ? format_index_ : 0;
    return formats[index].render(color);
}

bool MagnifierOverlay::copy_color_info() {
    if (!cursor_scene_pos) return false;
    const auto text = get_color_info_text();
    if (text.isEmpty()) return false;
    // The marker tells the clipboard history this text is a color, whatever format it is written in.
    auto *mime = new QMimeData;
    mime->setText(text);
    mime->setData(QString("application/x-qt-windows-mime;value=\"%1\"").arg(QLatin1String(jb_clipboard_color_marker_format())), QByteArrayLiteral("1"));
    QApplication::clipboard()->setMimeData(mime);
    return true;
}

void MagnifierOverlay::adjust_zoom(int delta) {
    zoom_factor_ += delta * 0.25;
    zoom_factor_ = std::max(zoom_min_, std::min(zoom_max_, zoom_factor_));
    if (config_manager) config_manager->qsettings.setValue("app/magnifier_zoom", zoom_factor_);
    log_debug(T("调整倍数: {zoom_factor:.2f}x", {{"zoom_factor", zoom_factor_}}), "Magnifier");
    // The zoom changed, so the cached sample is stale.
    clear_sample_cache();
    update();
}

int MagnifierOverlay::sample_size() const {
    const int size = std::max(static_cast<int>(SAMPLE_SIZE / zoom_factor_), 4);
    return size % 2 == 0 ? size + 1 : size;
}

void MagnifierOverlay::paintEvent(QPaintEvent *) {
    if (!should_render()) return;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    // The whole widget is the magnifier plus info box, drawn from (0, 0).
    const QRect combined_rect(0, 0, info_width_, combined_height());
    const auto image = background_image();
    const auto color = sample_color(image);
    update_sample_cache(image);
    draw_combined_magnifier(painter, combined_rect, color);
    painter.end();
}

QFont MagnifierOverlay::fitted_font(QPainter &painter, const QString &text, int max_width, int base_size, int min_size) {
    QFont font("Microsoft YaHei", base_size);
    font.setBold(true);
    painter.setFont(font);
    int text_width = painter.fontMetrics().horizontalAdvance(text);
    int current_size = base_size;
    while (text_width > max_width && current_size > min_size) {
        --current_size;
        font.setPointSize(current_size);
        painter.setFont(font);
        text_width = painter.fontMetrics().horizontalAdvance(text);
    }
    return font;
}

QString MagnifierOverlay::widest(QPainter &painter, const QStringList &texts) {
    QFont test_font("Microsoft YaHei", 11);
    test_font.setBold(true);
    painter.setFont(test_font);
    const auto tm = painter.fontMetrics();
    QString best;
    int best_width = -1;
    for (const auto &text : texts) {
        const int width = tm.horizontalAdvance(text);
        if (width > best_width) {
            best = text;
            best_width = width;
        }
    }
    return best;
}

void MagnifierOverlay::ensure_fixed_fonts(QPainter &painter, int text_width, int value_width) {
    // Coordinates and colour value share a size; the value line is narrower (swatch),
    // so each is fitted to its own width and the smaller size wins.
    if (fixed_info_font_) return;
    int max_x = 15360, max_y = 4320;  // conservative: two 8K screens
    if (scene) {
        max_x = static_cast<int>(scene->scene_rect.right());
        max_y = static_cast<int>(scene->scene_rect.bottom());
    }
    // The upper bound follows the UI scale, or text stays small in a wider box.
    const int info_size = std::max(5, static_cast<int>(python_round(INFO_FONT_PT * scale_factor())));
    const int hint_size = std::max(5, static_cast<int>(python_round(HINT_FONT_PT * scale_factor())));
    // The widest line depends on pixel width, so each format renders a colour with three-digit components.
    const QColor widest_color(255, 255, 255);
    QStringList rendered;
    for (const auto &format : formats_) rendered.append(format.render(widest_color));
    if (rendered.isEmpty()) rendered.append("#FFFFFF");
    const auto worst_value = widest(painter, rendered);
    const auto coord_font = fitted_font(painter, QString("(%1, %2)").arg(max_x).arg(max_y), text_width, info_size, 5);
    const auto value_font = fitted_font(painter, worst_value, value_width, info_size, 5);
    fixed_info_font_ = value_font.pointSize() < coord_font.pointSize() ? value_font : coord_font;
    // Fitted to the wider of both hints whether or not the Shift line shows, so the size stays put.
    const QStringList hints = {mag_tr("Shift: Switch color format"), mag_tr("C: Copy color value")};
    fixed_hint_font_ = fitted_font(painter, widest(painter, hints), text_width, hint_size, 5);
    painter.setFont(*fixed_info_font_);
    fixed_info_metrics_.emplace(*fixed_info_font_);
    fixed_hint_metrics_.emplace(*fixed_hint_font_);
}

void MagnifierOverlay::draw_pixel_grid(QPainter &painter, const QRect &mag_rect, const QRect &src) const {
    if (src.width() <= 0 || src.height() <= 0) return;
    const double cell_w = static_cast<double>(mag_rect.width()) / src.width();
    const double cell_h = static_cast<double>(mag_rect.height()) / src.height();
    if (std::min(cell_w, cell_h) < GRID_MIN_CELL) return;
    painter.setPen(pen_grid_);
    for (int i = 1; i < src.width(); ++i) {
        const int x = static_cast<int>(python_round(mag_rect.left() + i * cell_w));
        painter.drawLine(x, mag_rect.top(), x, mag_rect.bottom());
    }
    for (int j = 1; j < src.height(); ++j) {
        const int y = static_cast<int>(python_round(mag_rect.top() + j * cell_h));
        painter.drawLine(mag_rect.left(), y, mag_rect.right(), y);
    }
}

void MagnifierOverlay::draw_combined_magnifier(QPainter &painter, const QRect &rect, const QColor &color) {
    const QRect mag_rect(rect.x(), rect.y(), mag_width_, mag_height_);
    const QRect info_rect(rect.x(), rect.y() + mag_height_, info_width_, rect.height() - mag_height_);
    // Cells are square, sized from MAG_WIDTH; the source is square too, and only its
    // vertically centred MAG_HEIGHT rows are shown.
    int ss = std::max(static_cast<int>(SAMPLE_SIZE / zoom_factor_), 4);
    if (ss % 2 == 0) ++ss;
    const double px = static_cast<double>(mag_width_) / ss;
    const double crop_h_px = mag_height_ / px;
    const double crop_top = (ss - crop_h_px) / 2.0;

    painter.setPen(Qt::NoPen);
    painter.setBrush(brush_bg_);
    painter.drawRoundedRect(rect, radius_, radius_);

    // Image: 1:1 horizontally, cropped vertically, never stretched.
    painter.setBrush(Qt::NoBrush);
    if (!cached_sample_image_.isNull() && cached_source_rect_) {
        painter.save();
        QPainterPath clip_path;
        clip_path.addRoundedRect(rect.x(), rect.y(), rect.width(), rect.height(), radius_, radius_);
        painter.setClipPath(clip_path);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        const auto &sr = *cached_source_rect_;
        const QRectF src_crop(sr.x(), sr.y() + crop_top, sr.width(), crop_h_px);
        const auto drawn_src = src_crop.toRect();
        painter.drawImage(mag_rect, cached_sample_image_, drawn_src);
        if (show_grid_) draw_pixel_grid(painter, mag_rect, drawn_src);
        painter.restore();
    }

    // Crosshair through the centre pixel.
    painter.setPen(pen_teal_2_);
    painter.drawLine(mag_rect.left(), mag_rect.center().y(), mag_rect.right(), mag_rect.center().y());
    painter.drawLine(mag_rect.center().x(), mag_rect.top(), mag_rect.center().x(), mag_rect.bottom());

    // Zoom badge at the top-left corner.
    const auto zoom_text = python_fixed(zoom_factor_, 1) + "x";
    painter.setFont(font_);
    painter.setPen(pen_white_2_);
    painter.setBrush(brush_black_a_);
    const int text_margin = scaled(4);
    const auto metrics = painter.fontMetrics();
    const int text_width = metrics.horizontalAdvance(zoom_text);
    const int text_height = metrics.height();
    const QRect text_bg_rect(mag_rect.left() + scaled(2), mag_rect.top() + scaled(2), text_width + text_margin * 2, text_height + text_margin);
    painter.drawRoundedRect(text_bg_rect, scaled(3), scaled(3));
    painter.setBrush(Qt::NoBrush);
    painter.drawText(text_bg_rect.x() + text_margin, text_bg_rect.y() + text_height - scaled(2), zoom_text);

    painter.setPen(pen_teal_1_);
    painter.drawLine(info_rect.left(), info_rect.top(), info_rect.right(), info_rect.top());
    draw_info_lines(painter, info_rect, color);

    // Outer frame last, above everything.
    painter.setPen(pen_teal_2_);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(rect, radius_, radius_);
}

void MagnifierOverlay::draw_info_lines(QPainter &painter, const QRect &info_rect, const QColor &color) {
    const int padding = scaled(6);
    const int swatch = scaled(SWATCH_SIZE);
    const int gap = scaled(6);
    const int line_height = info_line_height_;
    const int text_width = info_rect.width() - padding * 2;
    ensure_fixed_fonts(painter, text_width, text_width - swatch - gap);
    const int top = info_rect.y() + scaled(4);
    const auto centered_x = [&](int width) { return info_rect.x() + python_floordiv(info_rect.width() - width, 2); };
    const auto baseline = [&](int row, const QFontMetrics &metrics) {
        return top + line_height * row + python_floordiv(line_height + metrics.height(), 2) - metrics.descent();
    };
    const auto &info_metrics = *fixed_info_metrics_;
    painter.setFont(*fixed_info_font_);
    const auto pos = cursor_scene_pos.value_or(QPointF(0, 0));
    const auto pos_text = QString("(%1, %2)").arg(static_cast<int>(pos.x())).arg(static_cast<int>(pos.y()));
    painter.setPen(color_pos_);
    painter.drawText(centered_x(info_metrics.horizontalAdvance(pos_text)), baseline(0, info_metrics), pos_text);

    // Swatch and value are centred as one group.
    QString value_text;
    if (!formats_.isEmpty()) {
        const int index = format_index_ < formats_.size() ? format_index_ : 0;
        value_text = formats_[index].render(color);
    }
    const int left = centered_x(swatch + gap + info_metrics.horizontalAdvance(value_text));
    painter.setPen(pen_swatch_);
    painter.setBrush(QBrush(color));
    painter.drawRect(QRect(left, top + line_height + python_floordiv(line_height - swatch, 2), swatch, swatch));
    painter.setBrush(Qt::NoBrush);
    painter.setPen(color_value_);
    painter.drawText(left + swatch + gap, baseline(1, info_metrics), value_text);

    const auto &hint_metrics = *fixed_hint_metrics_;
    painter.setFont(*fixed_hint_font_);
    painter.setPen(color_hint_);
    const auto hints = hint_texts();
    for (int offset = 0; offset < hints.size(); ++offset)
        painter.drawText(centered_x(hint_metrics.horizontalAdvance(hints[offset])), baseline(2 + offset, hint_metrics), hints[offset]);
}

bool MagnifierOverlay::should_render() const {
    if (!enabled_) return false;
    if (!cursor_scene_pos || !scene || !view) return false;
    if (view->drawing && view->drawing->active) return false;
    if (view->text_drag && view->text_drag->active) return false;
    if (view->smart_edit_controller && view->smart_edit_controller->layer_editor && view->smart_edit_controller->layer_editor->dragging_handle) return false;
    if (scene->selection_model->is_confirmed() && scene->tool_controller->current_tool_id() != "cursor") return false;
    return true;
}

QImage MagnifierOverlay::background_image() const {
    if (!sample_image_.isNull()) return sample_image_;
    if (scene && scene->background) return scene->background->image();
    return {};
}

std::optional<QPoint> MagnifierOverlay::scene_to_image_point(const QPointF &scene_pos) const {
    std::optional<QRectF> rect = sample_rect_;
    if (!rect && scene) rect = scene->scene_rect;
    if (!rect) return std::nullopt;
    // Truncation picks the pixel block under the pointer.
    return QPoint(static_cast<int>(scene_pos.x() - rect->left()), static_cast<int>(scene_pos.y() - rect->top()));
}

std::optional<QPointF> MagnifierOverlay::scene_to_pixel_center(const QPointF &scene_pos) const {
    std::optional<QRectF> rect = sample_rect_;
    if (!rect && scene) rect = scene->scene_rect;
    if (!rect) return std::nullopt;
    const double x = scene_pos.x() - rect->left();
    const double y = scene_pos.y() - rect->top();
    return QPointF(static_cast<int>(x) + 0.5 + rect->left(), static_cast<int>(y) + 0.5 + rect->top());
}

QColor MagnifierOverlay::sample_color(const QImage &image) const {
    if (image.isNull() || !cursor_scene_pos) return QColor(255, 255, 255);
    const auto pt = scene_to_image_point(*cursor_scene_pos);
    if (pt && image.rect().contains(*pt)) return image.pixelColor(*pt);
    return QColor(255, 255, 255);
}

void MagnifierOverlay::update_sample_cache(const QImage &image) {
    if (image.isNull() || !cursor_scene_pos) return;
    // Sampling at the pixel centre aligns the crosshair with it.
    const auto center_pos = scene_to_pixel_center(*cursor_scene_pos);
    if (!center_pos) return;
    const auto pt = scene_to_image_point(*center_pos);
    if (!pt) return;
    if (pt == last_sample_pt_ && !cached_sample_image_.isNull()) return;
    // Odd sample size so the centre pixel is in the middle.
    const int size = sample_size();
    const int half = size / 2;
    const auto src = QRect(pt->x() - half, pt->y() - half, size, size).intersected(image.rect());
    if (src.width() <= 0 || src.height() <= 0) return;
    // A small local copy, so frames do not address the large image.
    last_sample_pt_ = *pt;
    cached_source_rect_ = QRect(0, 0, src.width(), src.height());
    cached_sample_image_ = image.copy(src);
}

QPoint MagnifierOverlay::calc_widget_pos(const QPointF &scene_pos) const {
    // Scene coordinates are global screen coordinates (virtual desktop).
    auto *parent = parentWidget();
    if (!parent) return QPoint(0, 0);
    const QPoint global_pt(static_cast<int>(python_round(scene_pos.x())), static_cast<int>(python_round(scene_pos.y())));
    const auto local = parent->mapFromGlobal(global_pt);
    const int margin = edge_margin_;
    const int w = info_width_;
    const int h = combined_height();
    const int pw = parent->width();
    const int ph = parent->height();
    // Bottom-right of the pointer by default; flips left or up on overflow.
    int x = local.x() + margin;
    int y = local.y() + margin;
    if (x + w > pw - margin) x = std::max(margin, local.x() - w - margin);
    if (y + h > ph - margin) y = std::max(margin, local.y() - h - margin);
    return QPoint(x, y);
}
}
