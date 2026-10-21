#include "pin/pin_window.h"
#include "canvas/scene.h"
#include "canvas/items/background_item.h"
#include "core/clipboard_utils.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "core/platform_utils.h"
#include "core/qt_utils.h"
#include "core/save.h"
#include "core/shortcut_manager.h"
#include "core/theme.h"
#include "core/ui_scale.h"
#include "pin/pin_border_overlay.h"
#include "pin/pin_canvas.h"
#include "pin/pin_canvas_view.h"
#include "pin/pin_click_through_bar.h"
#include "pin/pin_shortcut.h"
#include "pin/pin_toolbar.h"
#include "settings/tool_settings.h"
#include "tools/controller.h"
#include "tools/cursor_manager.h"
#include "ui/dialogs.h"
#include "ui/save_image_dialog.h"
#include "ui/arrow_settings_panel.h"
#include "ui/mosaic_settings_panel.h"
#include "ui/number_settings_panel.h"
#include "ui/paint_settings_panel.h"
#include "ui/shape_settings_panel.h"
#include "ui/text_settings_panel.h"
#include "ui/watermark_settings_panel.h"
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDateTime>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QScreen>
#include <QWheelEvent>
#include <cmath>

namespace jietuba {
namespace {
QString pin_tr(const char *text) { return i18n_tr(text, "PinWindow"); }

// The window point zoom keeps still, as fractions of width and height; mouse uses the pointer.
std::optional<std::pair<double, double>> zoom_anchor(const QString &name) {
    if (name == "top_right") return std::make_pair(1.0, 0.0);
    if (name == "bottom_left") return std::make_pair(0.0, 1.0);
    if (name == "bottom_right") return std::make_pair(1.0, 1.0);
    if (name == "center") return std::make_pair(0.5, 0.5);
    if (name == "mouse") return std::nullopt;
    return std::make_pair(0.0, 0.0);
}

Qt::CursorShape resize_cursor(const std::pair<int, int> &edge) {
    const auto [sx, sy] = edge;
    if (sy == 0) return Qt::SizeHorCursor;
    if (sx == 0) return Qt::SizeVerCursor;
    return sx == sy ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor;
}

// The kind of lock a mouse gesture falls under once locked; the same items in the
// context menu are explicit and always run.
std::optional<QString> locked_mouse_action(const QString &action) {
    if (action == "close") return QString("click_close");
    if (action == "reset" || action == "thumbnail") return QString("move");
    return std::nullopt;
}

// Python round(value, 4).
double round4(double value) { return python_fixed(value, 4).toDouble(); }
}

PinWindow::PinWindow(const QImage &image, const QPoint &position, ToolSettingsManager *config_manager, const QList<QGraphicsItem *> &items,
                     const std::optional<QPoint> &offset, std::optional<int> number_next)
    : QWidget(nullptr), config_manager(config_manager), drawing_items(items), selection_offset(offset.value_or(QPoint(0, 0))) {
    // Border style.
    border_enabled = config_manager ? config_manager->get_app_setting("pin_auto_border").toBool() : true;
    border_color = get_theme().theme_color();
    border_color.setAlpha(200);
    mouse_click_timer_ = new QTimer(this);
    mouse_click_timer_->setSingleShot(true);
    connect(mouse_click_timer_, &QTimer::timeout, this, &PinWindow::finish_mouse_click);
    click_through_opacity_ = opacity_setting("pin_click_through_opacity");
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_DeleteOnClose, true);
    // The same window with or without a border.
    setAttribute(Qt::WA_TranslucentBackground, true);
    setMouseTracking(true);
    orig_size_ = image.size();
    base_pixmap_ = QPixmap::fromImage(image);
    scale_timer_ = new QTimer(this);
    scale_timer_->setSingleShot(true);
    scale_timer_->setInterval(80);
    connect(scale_timer_, &QTimer::timeout, this, &PinWindow::apply_smooth_scaling);
    win_opacity_ = opacity_setting("pin_default_opacity");
    if (win_opacity_ < 1.0) setWindowOpacity(win_opacity_);
    // Zoom percentage hint.
    zoom_label_ = new QLabel(this);
    zoom_label_->setStyleSheet(".QLabel {"
                               "  color: #2EC4B6;"
                               "  background: rgba(0, 0, 0, 160);"
                               "  border-radius: 4px;"
                               "  padding: 2px 6px;"
                               "  font-size: 12px;"
                               "  font-weight: bold;"
                               "}");
    zoom_label_->setAlignment(Qt::AlignCenter);
    zoom_label_->hide();
    zoom_hide_timer_ = new QTimer(this);
    zoom_hide_timer_->setSingleShot(true);
    zoom_hide_timer_->setInterval(1000);
    connect(zoom_hide_timer_, &QTimer::timeout, zoom_label_, &QLabel::hide);
    setGeometry(initial_geometry(position, image.size()));
    setup_control_buttons();
    canvas = new PinCanvas(this, orig_size_, image);
    if (!drawing_items.isEmpty()) canvas->initialize_from_items(drawing_items, selection_offset, number_next);
    cross_tool_selection_enabled_ = config_manager && config_manager->get_cross_tool_selection_enabled();
    view = new PinCanvasView(canvas->scene, this, canvas, cross_tool_selection_enabled_);
    view->setParent(this);
    view->setGeometry(0, 0, width(), height());
    view->set_corner_radius(corner);
    image_transform_ = std::make_unique<PinImageTransform>();
    update_view_transform();
    view->viewport()->installEventFilter(this);
    if (scale_factor != 1.0) scale_timer_->start();
    // The toolbar is created on demand.
    hover_controls = std::make_unique<PinHoverControls>(this);
    ocr_mgr_ = std::make_unique<PinOCRManager>(this, config_manager);
    thumbnail_ = std::make_unique<PinThumbnailMode>(this);
    // One theme-colour outline, no shadow.
    if (border_enabled) {
        border_overlay = new PinBorderOverlay(this, corner, border_color);
        border_overlay->setGeometry(0, 0, width(), height());
        border_overlay->raise();
    }
    show();
    update_button_positions();
    PinShortcutController::instance()->register_pin(this);
    // OCR starts 300 ms later, once the pin is fully shown; recognition runs on a worker thread.
    QTimer::singleShot(300, this, [this] { ocr_mgr_->init_now(); });
    log_info(T("创建成功: {width}x{height}, 位置: ({x}, {y})", {{"width", image.width()}, {"height", image.height()}, {"x", position.x()}, {"y", position.y()}}),
             "PinWindow");
    if (!drawing_items.isEmpty()) log_debug(T("继承了 {count} 个绘制项目（向量数据）", {{"count", static_cast<int>(drawing_items.size())}}), "PinWindow");
}

PinWindow::~PinWindow() {
    if (!is_closed_) {
        is_closed_ = true;
        cleanup();
    }
}

void PinWindow::setup_control_buttons() {
    control_buttons_ = std::make_unique<PinControlButtons>(this);
    control_buttons_->connect_signals([this] { close_unless_locked("click_close"); }, [this] { toggle_toolbar(); });
    context_menu_ = std::make_unique<PinContextMenu>(this);
    translation_helper_ = std::make_unique<PinTranslationHelper>(this, config_manager);
    update_button_positions();
}

void PinWindow::update_button_positions() {
    if (control_buttons_) control_buttons_->update_positions(width());
}

void PinWindow::raise_control_buttons() { control_buttons_->raise_all(); }

void PinWindow::refresh_appearance() {
    if (is_closed_) return;
    // A pin in click-through keeps its opacity; the new value applies next time.
    click_through_opacity_ = opacity_setting("pin_click_through_opacity");
    apply_window_corners();
    hover_controls->sync();
}

QVariant PinWindow::pin_setting(const QString &key) const {
    return config_manager ? config_manager->get_app_setting(key) : get_tool_settings_manager()->app_defaults().value(key);
}

double PinWindow::opacity_setting(const QString &key) const { return std::max(MIN_OPACITY, std::min(1.0, pin_setting(key).toDouble())); }

QRect PinWindow::initial_geometry(const QPoint &position, const QSize &size) {
    if (pin_setting("pin_auto_shrink").toBool()) {
        auto *screen = QApplication::screenAt(position);
        if (!screen) screen = QApplication::primaryScreen();
        const auto area = screen ? screen->geometry() : QRect();
        if (!area.isEmpty() && (size.width() > area.width() || size.height() > area.height())) {
            scale_factor = std::min(AUTO_SHRINK_RATIO * area.width() / size.width(), AUTO_SHRINK_RATIO * area.height() / size.height());
            const int w = std::max(1, static_cast<int>(python_round(size.width() * scale_factor)));
            const int h = std::max(1, static_cast<int>(python_round(size.height() * scale_factor)));
            return QRect(area.x() + (area.width() - w) / 2, area.y() + (area.height() - h) / 2, w, h);
        }
    }
    return QRect(position, size);
}

void PinWindow::apply_window_corners() {
    const bool rounded = config_manager && config_manager->get_app_setting("pin_rounded_corners").toBool();
    // Without ever squaring the corners the system setting stays untouched (Windows default).
    if (rounded && !square_corners_) return;
    set_window_rounded_corners(static_cast<quintptr>(winId()), rounded);
    square_corners_ = !rounded;
}

void PinWindow::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    // Each show restores the system default corners (hiding, toggling topmost), so they are set again.
    apply_window_corners();
    if (click_through_) {
        // Toggling topmost resets the window flags and drops the click-through style.
        set_window_click_through(static_cast<quintptr>(winId()), true);
        click_through_bar_->show();
    }
}

void PinWindow::hideEvent(QHideEvent *event) {
    QWidget::hideEvent(event);
    if (click_through_bar_) click_through_bar_->hide();
}

void PinWindow::start_window_drag(const QPoint &global_pos) {
    is_dragging_ = true;
    drag_start_pos_ = global_pos;
    drag_start_window_pos_ = pos();
    if (!lock_blocks("move")) setCursor(Qt::ClosedHandCursor);
}

void PinWindow::update_window_drag(const QPoint &global_pos) {
    if (!is_dragging_) return;
    const auto delta = global_pos - drag_start_pos_;
    if (lock_blocks("move")) {
        // No hint for a click; only a real drag gets one, once per drag.
        if (delta.manhattanLength() >= QApplication::startDragDistance()) {
            is_dragging_ = false;
            show_locked_hint();
        }
        return;
    }
    move(drag_start_window_pos_ + delta);
    if (toolbar && toolbar->isVisible()) toolbar->sync_with_pin_window();
}

void PinWindow::end_window_drag() {
    if (is_dragging_) {
        is_dragging_ = false;
        setCursor(Qt::ArrowCursor);
    }
}

std::optional<std::pair<int, int>> PinWindow::resize_edge_at(const QPoint &p) {
    if (is_closed_ || thumbnail_mode() || is_dragging_ || (canvas && canvas->is_editing()) || !pin_setting("pin_border_resize").toBool() || lock_blocks("move"))
        return std::nullopt;
    auto *scr = screen();
    int margin = static_cast<int>(python_round(RESIZE_MARGIN * (scr ? scr->logicalDotsPerInch() : 96) / 96));
    // Small pins keep at least half for dragging.
    margin = std::min({margin, width() / 4, height() / 4});
    const int sx = p.x() < margin ? -1 : (p.x() >= width() - margin ? 1 : 0);
    const int sy = p.y() < margin ? -1 : (p.y() >= height() - margin ? 1 : 0);
    if (!sx && !sy) return std::nullopt;
    return std::make_pair(sx, sy);
}

bool PinWindow::handle_border_resize(QObject *watched, QEvent *event) {
    auto *widget = qobject_cast<QWidget *>(watched);
    if (!widget) return false;
    const auto kind = event->type();
    if (resize_edge_) {
        if (kind == QEvent::MouseMove) {
            update_border_resize(static_cast<QMouseEvent *>(event)->globalPosition().toPoint());
        } else if (kind == QEvent::MouseButtonRelease && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
            end_border_resize();
        }
        return kind == QEvent::MouseMove || kind == QEvent::MouseButtonPress || kind == QEvent::MouseButtonRelease || kind == QEvent::MouseButtonDblClick;
    }
    if (kind == QEvent::MouseMove) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        // No cursor change while dragging something else (the window, a text selection) across an edge.
        if (mouse->buttons() != Qt::NoButton) return false;
        const auto edge = resize_edge_at(widget->mapTo(this, mouse->position().toPoint()));
        show_resize_cursor(widget, edge);
        return edge.has_value();
    }
    if (kind != QEvent::MouseButtonPress) return false;
    auto *mouse = static_cast<QMouseEvent *>(event);
    if (mouse->button() != Qt::LeftButton || mouse->modifiers() != Qt::NoModifier) return false;
    const auto edge = resize_edge_at(widget->mapTo(this, mouse->position().toPoint()));
    if (!edge) return false;
    resize_edge_ = edge;
    resize_start_pos_ = mouse->globalPosition().toPoint();
    resize_start_geometry_ = geometry();
    return true;
}

void PinWindow::show_resize_cursor(QWidget *widget, const std::optional<std::pair<int, int>> &edge) {
    if (edge) {
        widget->setCursor(resize_cursor(*edge));
        resize_cursor_owner_ = widget;
    } else if (resize_cursor_owner_) {
        // While editing the drawing tool owns the cursor.
        if (!(canvas && canvas->is_editing())) resize_cursor_owner_->setCursor(Qt::ArrowCursor);
        resize_cursor_owner_ = nullptr;
    }
}

void PinWindow::update_border_resize(const QPoint &global_pos) {
    const auto [sx, sy] = *resize_edge_;
    const auto delta = global_pos - resize_start_pos_;
    const auto start = resize_start_geometry_;
    const auto base = display_base_size();
    const double start_scale = static_cast<double>(start.width()) / base.width();
    std::vector<double> candidates;
    if (sx) candidates.push_back(static_cast<double>(start.width() + sx * delta.x()) / base.width());
    if (sy) candidates.push_back(static_cast<double>(start.height() + sy * delta.y()) / base.height());
    // Dragging a corner follows the direction that changes more.
    double scale = candidates.front();
    for (double value : candidates)
        if (std::abs(value - start_scale) > std::abs(scale - start_scale)) scale = value;
    const double fx = sx < 0 ? 1.0 : 0.0;
    const double fy = sy < 0 ? 1.0 : 0.0;
    scale_around(scale, fx, fy, QPointF(start.x() + fx * start.width(), start.y() + fy * start.height()));
}

void PinWindow::update_display() {
    if (view) view->viewport()->update();
    else update();
}

void PinWindow::update_view_transform() {
    if (!view || !canvas) return;
    const auto scene_rect = canvas->scene->sceneRect();
    if (scene_rect.width() == 0 || scene_rect.height() == 0) return;
    view->resetTransform();
    const auto cr = content_rect();
    const double scale_x = cr.width() / scene_rect.width();
    const double scale_y = cr.height() / scene_rect.height();
    view_scale_x_ = scale_x;
    view_scale_y_ = scale_y;
    if (image_transform_ && image_transform_->has_transform())
        view->setTransform(image_transform_->build_view_transform(cr.width(), cr.height(), scene_rect.width(), scene_rect.height()));
    else view->scale(scale_x, scale_y);
    // Widths are stored in image pixels, so the cursor follows the visual scale to preview
    // real strokes. The determinant, not scale_x: 90/270 rotation swaps the axes.
    if (view->cursor_manager) view->cursor_manager->update_view_scale(std::sqrt(std::abs(view->transform().determinant())));
}

void PinWindow::refresh_background_for_scale() {
    if (!canvas || !canvas->scene) return;
    auto *background_item = canvas->scene->background;
    if (!background_item || view_scale_x_ <= 0 || view_scale_y_ <= 0) return;
    if (base_pixmap_.isNull()) return;
    // Pre-scaled so pixel density matches the display; after 90/270 rotation each scene
    // axis maps to the other display axis.
    const auto cr = content_rect();
    const auto scene_rect = canvas->scene->sceneRect();
    double bg_scale_x, bg_scale_y;
    if (image_transform_ && image_transform_->is_rotated_90_or_270()) {
        bg_scale_x = cr.height() / scene_rect.width();
        bg_scale_y = cr.width() / scene_rect.height();
    } else {
        bg_scale_x = view_scale_x_;
        bg_scale_y = view_scale_y_;
    }
    const int tw = std::max(1, static_cast<int>(python_round(orig_size_.width() * bg_scale_x)));
    const int th = std::max(1, static_cast<int>(python_round(orig_size_.height() * bg_scale_y)));
    const auto target_size = std::make_pair(tw, th);
    if (last_background_scale_size_ == target_size) return;
    const auto scaled = base_pixmap_.scaled(tw, th, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    // Only the rendered pixmap changes, not update_image(): that would feed this
    // display-resolution copy into the content cache, and mosaic reductions would then
    // sample display pixels instead of the original's, misplacing mosaics after zooming.
    background_item->set_display_pixmap(scaled, QTransform::fromScale(1.0 / bg_scale_x, 1.0 / bg_scale_y));
    last_background_scale_size_ = target_size;
}

void PinWindow::resizeEvent(QResizeEvent *event) {
    if (view) {
        view->setGeometry(0, 0, width(), height());
        if (thumbnail_mode()) thumbnail_->update_view();
        else update_view_transform();
        view->update_viewport_mask();
    }
    if (!thumbnail_mode()) {
        update_button_positions();
        if (toolbar && toolbar->isVisible()) toolbar->sync_with_pin_window();
    }
    if (border_overlay) {
        border_overlay->setGeometry(0, 0, width(), height());
        border_overlay->raise();
    }
    sync_click_through_bar();
    QWidget::resizeEvent(event);
}

void PinWindow::moveEvent(QMoveEvent *event) {
    QWidget::moveEvent(event);
    sync_click_through_bar();
}

void PinWindow::sync_click_through_bar() {
    if (click_through_bar_ && click_through_bar_->isVisible()) click_through_bar_->follow_pin();
}

void PinWindow::mousePressEvent(QMouseEvent *event) {
    if (handle_mouse_gesture(event)) {
        event->accept();
        return;
    }
    hover_controls->set_pin_hovered(true);
    if (event->button() == Qt::LeftButton && !(canvas && canvas->is_editing())) {
        start_window_drag(event->globalPosition().toPoint());
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void PinWindow::mouseMoveEvent(QMouseEvent *event) {
    if (handle_mouse_gesture(event)) {
        event->accept();
        return;
    }
    hover_controls->set_pin_hovered(true);
    if (is_dragging_) {
        update_window_drag(event->globalPosition().toPoint());
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void PinWindow::mouseReleaseEvent(QMouseEvent *event) {
    if (handle_mouse_gesture(event)) {
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && is_dragging_) {
        end_window_drag();
        event->accept();
        return;
    }
    if (event->button() == Qt::RightButton) {
        show_context_menu(event->globalPosition().toPoint());
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void PinWindow::wheelEvent(QWheelEvent *event) {
    if (thumbnail_mode()) {
        event->ignore();
        return;
    }
    // Windows delivers Alt+wheel as horizontal scrolling.
    const int delta = event->angleDelta().y() ? event->angleDelta().y() : event->angleDelta().x();
    if (delta == 0) {
        event->ignore();
        return;
    }
    for (const char *action : {"zoom", "fine_zoom", "opacity"}) {
        if (mouse_binding_matches(get_pin_mouse_binding(config_manager, action), event, "wheel")) {
            adjust_mouse_value(action, delta);
            return;
        }
    }
    event->ignore();
}

void PinWindow::adjust_mouse_value(const QString &action, int delta) {
    if (action == "opacity") {
        double step = pin_setting("pin_opacity_step").toDouble() / 100;
        if (delta < 0) step = -step;
        win_opacity_ = std::max(MIN_OPACITY, std::min(1.0, round4(win_opacity_ + step)));
        setWindowOpacity(win_opacity_);
        show_hint_label(QString::fromUtf8("α %1%").arg(python_round(win_opacity_ * 100)));
    } else if (action == "zoom" || action == "fine_zoom") {
        if (lock_blocks("move")) {
            show_locked_hint();
            return;
        }
        const double step = pin_setting(action == "zoom" ? "pin_zoom_step" : "pin_fine_zoom_step").toDouble() / 100;
        // Fixed points of the original size per notch, so scrolling back always returns to the same size.
        const double scale = round4(scale_factor + (delta > 0 ? step : -step));
        auto fraction = zoom_anchor(pin_setting("pin_zoom_anchor").toString());
        if (!fraction) {
            const auto p = mapFromGlobal(QCursor::pos());
            fraction = std::make_pair(std::min(std::max(static_cast<double>(p.x()) / std::max(1, width()), 0.0), 1.0),
                                      std::min(std::max(static_cast<double>(p.y()) / std::max(1, height()), 0.0), 1.0));
        }
        scale_around(scale, fraction->first, fraction->second);
    }
}

QSize PinWindow::display_base_size() const { return image_transform_ ? image_transform_->display_size(orig_size_) : orig_size_; }

void PinWindow::scale_around(double scale, double fx, double fy, std::optional<QPointF> anchor) {
    const auto base = display_base_size();
    const double min_scale = std::max(static_cast<double>(MIN_SIDE) / base.width(), static_cast<double>(MIN_SIDE) / base.height());
    scale = std::max(min_scale, std::min(scale, MAX_SCALE));
    if (std::abs(scale - 1.0) < 1e-6) scale = 1.0;
    // Always from the original size, never re-rounding the current integer window size.
    const int nw = std::max(1, static_cast<int>(python_round(base.width() * scale)));
    const int nh = std::max(1, static_cast<int>(python_round(base.height() * scale)));
    if (!anchor) {
        const auto geo = geometry();
        anchor = QPointF(geo.x() + fx * geo.width(), geo.y() + fy * geo.height());
    }
    scale_factor = scale;
    is_scaling_ = true;
    setGeometry(static_cast<int>(python_round(anchor->x() - fx * nw)), static_cast<int>(python_round(anchor->y() - fy * nh)), nw, nh);
    if (canvas) canvas->invalidate_cache();
    update();
    scale_timer_->start();
    show_zoom_percent();
}

bool PinWindow::blocked_by_modal() const {
    // Mouse moves come here too: activeModalWidget first rules out the common case of no modal at all.
    return QApplication::activeModalWidget() && blocking_modal(const_cast<PinWindow *>(this));
}

bool PinWindow::handle_mouse_alternative_key(QInputEvent *event) {
    const int key = event_key(event);
    if (key != Qt::Key_Plus && key != Qt::Key_Equal && key != Qt::Key_Minus) return false;
    if (blocked_by_modal() || thumbnail_mode()) return false;
    QList<Qt::KeyboardModifiers> modifier_options = {event->modifiers()};
    // An explicit Shift binding first, then Shift used to type '+'.
    if (key == Qt::Key_Plus) modifier_options.append(event->modifiers() & ~Qt::ShiftModifier);
    for (const auto &modifiers : modifier_options) {
        for (const char *action : {"zoom", "fine_zoom", "opacity"}) {
            const auto binding = get_pin_mouse_binding(config_manager, action);
            if (binding.isEmpty()) continue;
            if (modifiers == binding_modifiers(binding)) {
                adjust_mouse_value(action, key != Qt::Key_Minus ? 120 : -120);
                return true;
            }
        }
    }
    return false;
}

std::optional<QString> PinWindow::matching_mouse_action(QMouseEvent *event, const QString &gesture) {
    for (const auto &entry : PIN_MOUSE_ACTIONS()) {
        if (QString(entry.kind) != "wheel" && mouse_binding_matches(get_pin_mouse_binding(config_manager, entry.key), event, gesture)) {
            if (QString(entry.key) == "copy_text" && !(text_selection() && text_selection()->has_selection())) continue;
            return QString(entry.key);
        }
    }
    return std::nullopt;
}

void PinWindow::run_mouse_action(const QString &action, const std::optional<QPoint> &position) {
    if (is_closed_) return;
    const auto blocked_by = locked_mouse_action(action);
    if (blocked_by && lock_blocks(*blocked_by)) {
        show_locked_hint();
        return;
    }
    if (action == "close") close_window();
    else if (action == "reset" && !thumbnail_mode()) reset_to_original_size();
    else if (action == "thumbnail") toggle_thumbnail_mode();
    else if (action == "copy_text" && text_selection()) text_selection()->copy_selected_text();
    else if (action == "context_menu" && position) show_context_menu(*position);
}

void PinWindow::finish_mouse_click() {
    const auto action = pending_mouse_action_;
    const auto position = pending_mouse_position_;
    pending_mouse_action_.reset();
    pending_mouse_position_.reset();
    if (action) run_mouse_action(*action, position);
}

bool PinWindow::handle_mouse_gesture(QMouseEvent *event) {
    if (is_closed_ || (canvas && canvas->is_editing())) return false;
    if (blocked_by_modal()) return false;
    const auto kind = event->type();
    if (kind != QEvent::MouseButtonPress && kind != QEvent::MouseButtonDblClick && kind != QEvent::MouseMove && kind != QEvent::MouseButtonRelease) return false;
    const auto button = event->button();
    QString gesture;
    if (button == Qt::LeftButton) gesture = "left";
    else if (button == Qt::MiddleButton) gesture = "middle";
    else if (button == Qt::RightButton) gesture = "right";
    if (kind == QEvent::MouseButtonRelease && mouse_swallow_release_ && button == *mouse_swallow_release_) {
        mouse_swallow_release_.reset();
        return true;
    }
    if (kind == QEvent::MouseButtonDblClick && !gesture.isEmpty()) {
        if (const auto action = matching_mouse_action(event, "double" + gesture)) {
            mouse_click_timer_->stop();
            pending_mouse_action_.reset();
            pending_mouse_position_.reset();
            mouse_gesture_.reset();
            end_window_drag();
            view->window_dragging = false;
            mouse_swallow_release_ = button;
            run_mouse_action(*action);
            return true;
        }
    }
    if (kind == QEvent::MouseButtonPress && !gesture.isEmpty()) {
        const auto action = matching_mouse_action(event, gesture);
        const bool region = matching_mouse_action(event, "drag" + gesture) == QString("region");
        const bool defer_menu = gesture == "right" && matching_mouse_action(event, "doubleright").has_value();
        if (action || (region && !thumbnail_mode()) || defer_menu) {
            mouse_gesture_ = MouseGesture{button, event->globalPosition().toPoint(), action, region};
            return true;
        }
    }
    if (!mouse_gesture_) return false;
    const auto state = *mouse_gesture_;
    const auto current = event->globalPosition().toPoint();
    const bool dragged = (current - state.start).manhattanLength() >= QApplication::startDragDistance();
    if (kind == QEvent::MouseMove) {
        if (state.region && dragged) {
            if (!mouse_band_) mouse_band_ = new QRubberBand(QRubberBand::Rectangle, view->viewport());
            const auto r = QRect(view->viewport()->mapFromGlobal(state.start), view->viewport()->mapFromGlobal(current)).normalized();
            mouse_band_->setGeometry(r.intersected(view->viewport()->rect()));
            mouse_band_->show();
            mouse_band_->raise();
        }
        return true;
    }
    if (kind == QEvent::MouseButtonRelease && button == state.button) {
        mouse_gesture_.reset();
        if (mouse_band_) mouse_band_->hide();
        if (state.region && dragged) {
            auto r = QRect(view->viewport()->mapFromGlobal(state.start), view->viewport()->mapFromGlobal(current)).normalized();
            r = r.intersected(view->viewport()->rect());
            if (r.width() > 2 && r.height() > 2) {
                if (lock_blocks("move")) show_locked_hint();
                else thumbnail_->enter_region(view->mapToScene(r).boundingRect());
            }
        } else if (!dragged) {
            auto action = state.action;
            if (!action && button == Qt::RightButton) action = QString("context_menu");
            if (action) {
                if (matching_mouse_action(event, "double" + gesture)) {
                    pending_mouse_action_ = action;
                    pending_mouse_position_ = current;
                    mouse_click_timer_->start(QApplication::doubleClickInterval());
                } else {
                    run_mouse_action(*action, current);
                }
            }
        }
        return true;
    }
    return false;
}

void PinWindow::mouseDoubleClickEvent(QMouseEvent *event) {
    if (handle_mouse_gesture(event)) {
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void PinWindow::apply_smooth_scaling() {
    if (is_closed_) return;
    is_scaling_ = false;
    refresh_background_for_scale();
    update();
}

void PinWindow::show_zoom_percent() {
    // The logical scale, so window rounding never hides the real ratio.
    show_hint_label(QString("%1%").arg(python_round(scale_factor * 100)));
}

void PinWindow::show_hint_label(const QString &text) {
    zoom_label_->setText(text);
    zoom_label_->adjustSize();
    zoom_label_->move(8, 8);
    zoom_label_->raise();
    zoom_label_->show();
    zoom_hide_timer_->start();
}

void PinWindow::enterEvent(QEnterEvent *event) {
    QWidget::enterEvent(event);
    hover_controls->set_pin_hovered(true);
}

void PinWindow::leaveEvent(QEvent *event) {
    QWidget::leaveEvent(event);
    hover_controls->recheck_pin_hovered();
}

void PinWindow::keyPressEvent(QKeyEvent *event) {
    // Pin shortcuts go through the ShortcutManager; this is only a fallback.
    QWidget::keyPressEvent(event);
}

bool PinWindow::eventFilter(QObject *watched, QEvent *event) {
    if (view && watched == view->viewport()) {
        if (handle_border_resize(watched, event)) return true;
        const auto type = event->type();
        if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick || type == QEvent::MouseMove || type == QEvent::MouseButtonRelease)
            if (handle_mouse_gesture(static_cast<QMouseEvent *>(event))) return true;
        if (type == QEvent::Enter || type == QEvent::HoverEnter || type == QEvent::MouseMove) hover_controls->set_pin_hovered(true);
        else if (type == QEvent::Leave || type == QEvent::HoverLeave) hover_controls->recheck_pin_hovered();
    }
    return QWidget::eventFilter(watched, event);
}

void PinWindow::toggle_thumbnail_mode() { thumbnail_->toggle(); }

void PinWindow::toggle_toolbar() { hover_controls->toggle_toolbar(); }

void PinWindow::show_toolbar() {
    if (!toolbar) {
        toolbar = new PinToolbar(this, config_manager);
        if (canvas) canvas->connect_toolbar(toolbar, view);
        log_debug(T("创建工具栏，信号已由 PinCanvas 连接"), "PinWindow");
    }
    toolbar->show();
}

void PinWindow::hide_toolbar() {
    if (!toolbar) return;
    toolbar->hide_all_panels();
    // Hidden before leaving the tool: leaving calls back into sync(), which must see it hidden.
    toolbar->hide();
    if (toolbar->current_tool) {
        for (auto *button : toolbar->tool_buttons) button->setChecked(false);
        toolbar->current_tool.reset();
        emit toolbar->tool_changed("cursor");
    }
}

void PinWindow::on_translate_clicked() {
    if (!translation_helper_) return;
    if (ocr_has_result()) {
        if (text_selection()) translation_helper_->translate(text_selection()->layout());
    } else {
        if (!translation_helper_->begin_ocr_translation()) return;
        // Back to the event loop first so the translation window paints, then OCR (which
        // may load models): the same "panel first, recognize after" feel as capture translation.
        QTimer::singleShot(0, this, &PinWindow::start_ocr_for_translation);
    }
}

void PinWindow::start_ocr_for_translation() {
    if (is_closed_) return;
    if (ocr_mgr_->recognize_then([this](bool success, const QString &result) { on_ocr_translation_finished(success, result); })) {
        log_info(T("OCR 识别中，翻译将在识别完成后自动执行"), "Translate");
    } else {
        log_warning(T("无法启动钉图 OCR 识别"), "Translate");
        translation_helper_->complete_ocr_translation(false, pin_tr("OCR recognition could not be started"));
    }
}

void PinWindow::on_ocr_translation_finished(bool success, const QString &result) {
    if (translation_helper_) translation_helper_->complete_ocr_translation(success, result);
}

void PinWindow::show_context_menu(const QPoint &global_pos) {
    if (!context_menu_) return;
    PinMenuState state;
    state.toolbar_visible = toolbar && toolbar->isVisible();
    state.stay_on_top = windowFlags() & Qt::WindowStaysOnTopHint;
    state.click_through = click_through_;
    state.locked = locked_;
    state.border_enabled = border_enabled;
    state.text_selection_enabled = text_selection_enabled();
    state.thumbnail_mode = thumbnail_mode();
    state.close_shortcut_enabled = !(canvas && canvas->is_editing()) && !lock_blocks("key_close") && !click_through_;
    // An open menu counts as hover, so the toolbar is not hidden behind it and its switches stay accurate.
    hover_controls->set_menu_open(true);
    const QPointer<PinWindow> alive(this);
    context_menu_->show(global_pos, state);
    // The menu runs its own event loop, in which the pin may have been closed and deleted.
    if (alive) hover_controls->set_menu_open(false);
}

void PinWindow::toggle_stay_on_top() {
    auto flags = windowFlags();
    if (flags & Qt::WindowStaysOnTopHint) flags &= ~Qt::WindowStaysOnTopHint;
    else flags |= Qt::WindowStaysOnTopHint;
    const auto geo = geometry();
    setWindowFlags(flags);
    setGeometry(geo);
    show();
}

void PinWindow::toggle_lock() { set_locked(!locked_); }

void PinWindow::set_locked(bool value) {
    if (is_closed_ || value == locked_) return;
    locked_ = value;
    end_window_drag();
    end_border_resize();
    image_transform_->refresh_border(this);
    if (pin_setting("pin_lock_hint").toBool()) show_hint_label(value ? pin_tr("Locked, right-click to unlock") : pin_tr("Unlocked"));
}

bool PinWindow::lock_blocks(const QString &kind) const { return locked_ && pin_setting("pin_lock_block_" + kind).toBool(); }

void PinWindow::show_locked_hint() {
    if (pin_setting("pin_lock_hint").toBool()) show_hint_label(pin_tr("Locked, right-click to unlock"));
}

void PinWindow::close_unless_locked(const QString &kind) {
    if (lock_blocks(kind)) {
        show_locked_hint();
        return;
    }
    close_window();
}

void PinWindow::toggle_click_through() { set_click_through(!click_through_); }

void PinWindow::set_click_through(bool enabled) {
    if (is_closed_ || enabled == click_through_) return;
    click_through_ = enabled;
    end_window_drag();
    if (enabled) {
        opacity_before_click_through_ = win_opacity_;
        set_window_opacity(std::min(win_opacity_, click_through_opacity_));
        if (!click_through_bar_) click_through_bar_ = new PinClickThroughBar(this);
        click_through_bar_->set_opacity(win_opacity_);
        click_through_bar_->show();
    } else {
        click_through_bar_->hide();
        set_window_opacity(opacity_before_click_through_.value_or(win_opacity_));
        opacity_before_click_through_.reset();
    }
    set_window_click_through(static_cast<quintptr>(winId()), enabled);
    // Entering click-through while editing hides the toolbar here, ending the drawing tool.
    hover_controls->sync();
    show_hint_label(enabled ? pin_tr("Click-through on") : pin_tr("Click-through off"));
}

void PinWindow::set_click_through_opacity(double opacity) {
    if (!click_through_) return;
    click_through_opacity_ = opacity;
    set_window_opacity(opacity);
}

void PinWindow::set_window_opacity(double opacity) {
    win_opacity_ = opacity;
    setWindowOpacity(opacity);
}

void PinWindow::toggle_border_effect() {
    border_enabled = !border_enabled;
    image_transform_->refresh_border(this);
    update();
}

void PinWindow::toggle_text_selection() {
    if (ocr_mgr_) ocr_mgr_->toggle_text_selection();
}

void PinWindow::reset_to_original_size() {
    const auto sz = image_transform_ && image_transform_->has_transform() ? image_transform_->display_size(orig_size_) : orig_size_;
    // The top-left corner stays, as with wheel zoom.
    setGeometry(x(), y(), sz.width(), sz.height());
    scale_factor = 1.0;
    if (canvas) canvas->invalidate_cache();
    update_button_positions();
    update_view_transform();
    refresh_background_for_scale();
    if (toolbar && toolbar->isVisible()) toolbar->sync_with_pin_window();
    image_transform_->refresh_border(this);
    update();
    show_zoom_percent();
}

void PinWindow::rotate_image_cw() {
    image_transform_->rotate_cw();
    image_transform_->apply_to_window(this);
}

void PinWindow::rotate_image_ccw() {
    image_transform_->rotate_ccw();
    image_transform_->apply_to_window(this);
}

void PinWindow::flip_image_horizontal() {
    image_transform_->flip_horizontal();
    image_transform_->apply_to_window(this);
}

void PinWindow::flip_image_vertical() {
    image_transform_->flip_vertical();
    image_transform_->apply_to_window(this);
}

void PinWindow::reset_image_transform() {
    image_transform_->reset();
    image_transform_->apply_to_window(this);
}

QImage PinWindow::get_current_image() {
    const double dpr = devicePixelRatioF();
    QImage img;
    if (canvas) {
        img = canvas->get_current_image(dpr);
    } else {
        img = QImage(static_cast<int>(width() * dpr), static_cast<int>(height() * dpr), QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        img.setDevicePixelRatio(dpr);
        QPainter p(&img);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.drawPixmap(rect(), base_pixmap_);
        p.end();
    }
    // Rotation and flips.
    if (image_transform_) img = image_transform_->transform_image(img);
    return img;
}

QTransform PinWindow::from_current_image(const QSize &size) const {
    // Without rotation or flips the view transform is a plain scale.
    const PinImageTransform none;
    const auto &transform = image_transform_ ? *image_transform_ : none;
    return transform.build_view_transform(size.width(), size.height(), orig_size_.width(), orig_size_.height()).inverted();
}

void PinWindow::with_edit_paused(const std::function<void()> &func) {
    const bool was_editing = canvas && canvas->is_editing();
    std::optional<QString> active_tool_id;
    if (was_editing && canvas->tool_controller) {
        if (auto *tool = canvas->tool_controller->current_tool) active_tool_id = tool->id();
        canvas->deactivate_tool();
    }
    try {
        func();
    } catch (...) {
        if (was_editing && active_tool_id) canvas->activate_tool(*active_tool_id);
        throw;
    }
    if (was_editing && active_tool_id) canvas->activate_tool(*active_tool_id);
}

void PinWindow::save_image() {
    if (use_modern_save_dialog(config_manager)) {
        QImage image;
        with_edit_paused([this, &image] { image = get_current_image(); });
        const auto base_name = QString("pinned_%1").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
        const auto [outcome, path] = SaveImageDialog::run(this, image, base_name, config_manager);
        if (outcome == SaveImageDialog::Outcome::SAVED) log_info(T("保存成功: {file_path}", {{"file_path", path}}), "PinWindow");
        if (outcome != SaveImageDialog::Outcome::CLASSIC) return;
    }
    save_image_with_system_dialog();
}

void PinWindow::save_image_with_system_dialog() {
    const auto fmt = (config_manager ? config_manager->get_screenshot_format() : QString("PNG")).toLower();
    const auto default_name = QString("pinned_%1.%2").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"), fmt);
    const SaveService save_service(config_manager ? *config_manager : *get_tool_settings_manager());
    auto [file_path, selected_filter] = get_save_file_name(this, pin_tr("Save Pin Image"), save_service.suggest_path(default_name), "PNG (*.png);;JPG (*.jpg);;BMP (*.bmp);;WebP (*.webp);;PDF (*.pdf)");
    if (file_path.isEmpty()) return;
    const auto ext = QFileInfo(file_path).suffix();
    QString image_format;
    if (!ext.isEmpty()) {
        image_format = ext.toUpper();
    } else {
        static const QRegularExpression pattern(R"(\*\.(\w+))");
        const auto match = pattern.match(selected_filter);
        image_format = match.hasMatch() ? match.captured(1).toUpper() : QString("PNG");
        file_path = file_path + "." + image_format.toLower();
    }
    with_edit_paused([this, file_path, image_format] {
        const auto image = get_current_image();
        if (SaveService::save_qimage_to_path(image, file_path, image_format)) log_info(T("保存成功: {file_path}", {{"file_path", file_path}}), "PinWindow");
        else log_error(T("保存失败: {file_path}", {{"file_path", file_path}}), "PinWindow");
    });
}

void PinWindow::copy_to_clipboard() {
    with_edit_paused([this] { deliver_image_async(get_current_image()); });
}

void PinWindow::copy_all_text() {
    if (ocr_has_result() && text_selection() && text_selection()->has_text()) {
        copy_ocr_text(true, text_selection()->layout().all_text("\n"));
        return;
    }
    if (!ocr_mgr_->recognize_then([this](bool success, const QString &result) { copy_ocr_text(success, result); }))
        show_hint_label(pin_tr("OCR recognition could not be started"));
}

void PinWindow::copy_ocr_text(bool success, const QString &result) {
    // A failure reason is already logged by recognize_then.
    if (!success || result.trimmed().isEmpty()) {
        show_hint_label(pin_tr("No text was recognized"));
        return;
    }
    QApplication::clipboard()->setText(result);
    log_info(T("已复制钉图文字: {count} 字符", {{"count", static_cast<int>(result.toUcs4().size())}}), "PinWindow");
    show_hint_label(pin_tr("Text copied"));
}

void PinWindow::close_window() {
    if (is_closed_) return;
    log_debug(T("开始关闭"), "PinWindow");
    is_closed_ = true;
    cleanup();
    emit closed();
    close();
}

void PinWindow::cleanup() {
    log_debug(T("清理资源..."), "PinWindow");
    PinShortcutController::instance()->unregister_pin(this);
    if (hover_controls) hover_controls->stop();
    if (scale_timer_) scale_timer_->stop();
    if (zoom_hide_timer_) zoom_hide_timer_->stop();
    if (click_through_bar_) {
        click_through_bar_->close();
        click_through_bar_->deleteLater();
        click_through_bar_ = nullptr;
    }
    if (toolbar) {
        // The panels and the "..." popup are top-level windows of their own and do not go with the toolbar.
        const auto close_panel = [](auto *&panel) {
            if (panel) {
                panel->close();
                panel->deleteLater();
                panel = nullptr;
            }
        };
        close_panel(toolbar->paint_panel);
        close_panel(toolbar->shape_panel);
        close_panel(toolbar->arrow_panel);
        close_panel(toolbar->number_panel);
        close_panel(toolbar->text_panel);
        close_panel(toolbar->mosaic_panel);
        close_panel(toolbar->watermark_panel);
        if (auto *popup = toolbar->more_popup()) {
            popup->close();
            popup->deleteLater();
        }
        toolbar->close();
        toolbar->deleteLater();
        toolbar = nullptr;
    }
    if (ocr_mgr_) ocr_mgr_->cleanup();
    if (view) {
        view->viewport()->removeEventFilter(this);
        view->deleteLater();
        view = nullptr;
    }
    if (canvas) {
        canvas->cleanup();
        canvas = nullptr;
    }
    base_pixmap_ = QPixmap();
    log_info(T("资源清理完成"), "PinWindow");
}

void PinWindow::closeEvent(QCloseEvent *event) {
    if (!is_closed_) {
        is_closed_ = true;
        cleanup();
        emit closed();
    }
    QWidget::closeEvent(event);
}
}
