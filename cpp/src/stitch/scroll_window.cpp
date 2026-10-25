#include "stitch/scroll_window.h"
#include "capture/capture_service.h"
#include "core/clipboard_utils.h"
#include "core/i18n.h"
#include "core/input_hub.h"
#include "core/logger.h"
#include "core/platform_utils.h"
#include "core/save.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "pin/pin_manager.h"
#include "settings/tool_settings.h"
#include "stitch/auto_scroll.h"
#include "stitch/change_watch.h"
#include "stitch/jietuba_long_stitch_unified.h"
#include "stitch/scroll_toolbar.h"
#include "text_recognition/result_window.h"
#include "tools/action.h"
#include <QApplication>
#include <QDateTime>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QScreen>
#include <QVBoxLayout>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace jietuba {
namespace {
const QString MODULE_TAG = "LongStitch";
QString tr_text(const char *text) { return i18n_tr(text, "ScrollCaptureWindow"); }

// INFO when forced (always kept), DEBUG otherwise (filtered by the global level).
void log_stitch(const LogMsg &message, bool force = false) {
    if (force) log_info(message, MODULE_TAG);
    else log_debug(message, MODULE_TAG);
}

// Loads the long screenshot parameters (only hash_rust remains, so only its parameters)
// and configures the stitch engine; once, like the reference's module import.
void load_long_stitch_config() {
    static bool loaded = false;
    if (loaded) return;
    loaded = true;
    auto *config_mgr = get_tool_settings_manager();
    const auto raw_engine = config_mgr->get_long_stitch_engine();
    const auto engine = long_stitch::normalize_engine_value(raw_engine);
    if (engine != raw_engine) {
        config_mgr->set_long_stitch_engine(engine);
        log_stitch(T("📖 检测到长截图引擎旧值 {raw_engine}，已自动转换为 {engine}", {{"raw_engine", raw_engine}, {"engine", engine}}));
    }
    const int ignore_top_pixels = static_cast<int>(config_mgr->get_long_stitch_ignore_top_pixels());
    log_stitch(T("📖 从配置加载长截图参数: 引擎={engine}, 顶部忽略={ignore_top_pixels}px", {{"engine", engine}, {"ignore_top_pixels", ignore_top_pixels}}));
    // verbose goes to the stitch engine and controls output on the Rust side.
    long_stitch::configure(engine, false, std::nullopt, ignore_top_pixels);
}

const QString INPUT_WATCHER = "stitch";
// Two grabs this far apart that match mean the picture has settled.
constexpr int STILL_CHECK_MS = 40;
constexpr int STILL_CHECK_TRIES = 6;
// The can't-join hint appears this late: an isolated odd frame (a white flash, mid
// animation) is usually followed at once by a good one, and the hint should not flash.
constexpr int WARNING_DELAY_MS = 600;
// Left and right Shift.
const std::vector<quint32> SHIFT_KEYS = {0xA0, 0xA1};
// The preview panel's fixed side; thumbnails are generated for it.
constexpr int PREVIEW_SIDE = 190;
const QColor BOX_COLOR(82, 196, 26);
constexpr int BASE_FADE = 28;
constexpr int BASE_MARGIN = 24;

// No change along the stitch direction (a white screen, mid repaint): nothing to stitch,
// and two matching grabs of it do not count as settled. Vertically every row is the
// same; horizontally columns are stitched, so every row is one colour.
bool blank(const GrabbedFrame &frame, bool horizontal) {
    const qsizetype stride = static_cast<qsizetype>(frame.width) * 4;
    const char *data = frame.bgra.constData();
    if (horizontal) {
        for (int row = 0; row < frame.height; ++row) {
            const char *line = data + row * stride;
            for (int x = 1; x < frame.width; ++x)
                if (std::memcmp(line + x * 4, line, 4) != 0) return false;
        }
        return true;
    }
    for (int row = 1; row < frame.height; ++row)
        if (std::memcmp(data + row * stride, data, static_cast<size_t>(stride)) != 0) return false;
    return true;
}

void make_click_through(QWidget *widget) {
    const auto hwnd = reinterpret_cast<HWND>(widget->winId());
    const LONG ex_style = GetWindowLongW(hwnd, GWL_EXSTYLE);
    SetWindowLongW(hwnd, GWL_EXSTYLE, ex_style | WS_EX_TRANSPARENT | WS_EX_LAYERED);
}

QString screen_repr(const QScreen *screen) { return screen ? screen->name() : QString("None"); }
}

PreviewPanel::PreviewPanel(QWidget *parent) : QWidget(parent) {
    setWindowFlags(Qt::WindowStaysOnTopHint | Qt::FramelessWindowHint | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground);
    fixed_side_ = scaled(PREVIEW_SIDE);
    // A square placeholder at first.
    panel_rect_ = QRect(0, 0, fixed_side_, fixed_side_);
    resize(fixed_side_, fixed_side_);
    build_ui();
    setup_mouse_transparent();
}

void PreviewPanel::setup_mouse_transparent() {
    make_click_through(this);
    log_stitch(T("[OK] PreviewPanel 已设置为鼠标穿透模式"));
    capture_excluded_ = false;
}

void PreviewPanel::build_ui() {
    // Stitch failures are written at the top: the panel is click-through, so tooltips never show.
    warning_label = new QLabel(this);
    warning_label->setAlignment(Qt::AlignCenter);
    warning_label->setWordWrap(true);
    set_own_style(warning_label, QString("background: rgba(255, 77, 79, 0.92);color: white;border-radius: %1px;padding: %2px %3px;font-size: %4pt;font-weight: 600;")
                                     .arg(scaled(6))
                                     .arg(scaled(4))
                                     .arg(scaled(8))
                                     .arg(scaled(9)));
    warning_label->hide();
    place_warning();
}

void PreviewPanel::place_warning() {
    const int margin = scaled(8);
    warning_label->setFixedWidth(std::max(scaled(60), panel_rect_.width() - 2 * margin));
    warning_label->adjustSize();
    warning_label->move(panel_rect_.topLeft() + QPoint(margin, margin));
}

void PreviewPanel::set_capture_excluded(bool exclude) {
    if (exclude == capture_excluded_) return;
    if (set_window_exclude_from_capture(static_cast<quintptr>(winId()), exclude)) capture_excluded_ = exclude;
}

void PreviewPanel::closeEvent(QCloseEvent *event) {
    // Restores capture on close.
    if (capture_excluded_) set_capture_excluded(false);
    QWidget::closeEvent(event);
}

void PreviewPanel::update_preview(const QImage &image, const QString &scroll_direction, const std::optional<std::pair<int, int>> &box) {
    vertical_ = scroll_direction != "horizontal";
    if (image.isNull()) {
        image_ = QImage();
        box_.reset();
        offset_ = 0;
        update(panel_rect_);
        return;
    }
    image_ = image;
    if (box) box_ = box;
    const int side = fixed_side_;
    const int total = vertical_ ? image.height() : image.width();
    const int length = std::min(std::max(total, side), max_length());
    const QRect old = panel_rect_;
    panel_rect_.setSize(vertical_ ? QSize(side, length) : QSize(length, side));
    offset_ = follow(total, length);
    place_warning();
    update(old.united(panel_rect_));
}

void PreviewPanel::place(const QPoint &top_left, const QRect &screen) {
    const QRect strip = vertical_ ? QRect(top_left.x(), screen.top(), fixed_side_, screen.height()) : QRect(screen.left(), top_left.y(), screen.width(), fixed_side_);
    if (geometry() != strip) setGeometry(strip);
    const QRect old = panel_rect_;
    panel_rect_.moveTopLeft(top_left - strip.topLeft());
    place_warning();
    update(old.united(panel_rect_));
}

int PreviewPanel::max_length() const {
    QScreen *screen = QApplication::primaryScreen();
    if (parentWidget() && parentWidget()->screen()) screen = parentWidget()->screen();
    if (!screen) return 800;
    const auto geometry = screen->geometry();
    return (vertical_ ? geometry.height() : geometry.width()) - 60;
}

int PreviewPanel::follow(int total, int length) const {
    if (total <= length) return 0;
    int offset = std::min(offset_, total - length);
    if (box_) {
        const auto [start, end] = *box_;
        const int margin = std::min(scaled(BASE_MARGIN), std::max(0, python_floordiv(length - (end - start), 2)));
        if (start < offset + margin) offset = start - margin;
        else if (end > offset + length - margin) offset = end + margin - length;
    }
    return std::max(0, std::min(offset, total - length));
}

void PreviewPanel::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(panel_rect_.topLeft());
    const int radius = scaled(8);
    QPainterPath outline;
    outline.addRoundedRect(QRectF(QPointF(), QSizeF(panel_rect_.size())).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    painter.fillPath(outline, QColor(0, 0, 0, 64));
    if (!image_.isNull()) {
        painter.setClipPath(outline);
        paint_content(painter);
        painter.setClipping(false);
    }
    painter.setPen(QPen(QColor(0, 0, 0, 204), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(outline);
    painter.end();
}

void PreviewPanel::paint_content(QPainter &painter) {
    const auto &image = image_;
    const bool vertical = vertical_;
    const int offset = offset_;
    const int total = vertical ? image.height() : image.width();
    const int w = panel_rect_.width(), h = panel_rect_.height();
    const int length = vertical ? h : w;
    const int visible = std::min(total, length);
    // Centred when the thumbnail is shorter than the panel.
    const int lead = python_floordiv(length - visible, 2);
    if (vertical) painter.drawImage(QRectF(0, lead, image.width(), visible), image, QRectF(0, offset, image.width(), visible));
    else painter.drawImage(QRectF(lead, 0, visible, image.height()), image, QRectF(offset, 0, visible, image.height()));

    if (box_) {
        const int pen_width = scaled(2);
        const double inset = pen_width / 2.0;
        const int start = box_->first - offset + lead, end = box_->second - offset + lead;
        painter.setPen(QPen(BOX_COLOR, pen_width));
        painter.setBrush(Qt::NoBrush);
        if (vertical) painter.drawRect(QRectF(inset, start + inset, w - pen_width, end - start - pen_width));
        else painter.drawRect(QRectF(start + inset, inset, end - start - pen_width, h - pen_width));
    }
    if (offset > 0) paint_clipped_edge(painter, true);
    if (offset + length < total) paint_clipped_edge(painter, false);
}

void PreviewPanel::paint_clipped_edge(QPainter &painter, bool at_start) {
    const int w = panel_rect_.width(), h = panel_rect_.height(), fade = scaled(BASE_FADE);
    const double arm = scaled(6), gap = scaled(10);
    QLinearGradient gradient;
    QRectF area;
    QPolygonF points;
    if (vertical_) {
        const int edge = at_start ? 0 : h, inner = at_start ? fade : h - fade;
        gradient = QLinearGradient(0, edge, 0, inner);
        area = QRectF(0, std::min(edge, inner), w, fade);
        const double cx = w / 2.0, cy = at_start ? gap : h - gap;
        const double tip = at_start ? -arm / 2 : arm / 2;
        points << QPointF(cx - arm, cy - tip) << QPointF(cx, cy + tip) << QPointF(cx + arm, cy - tip);
    } else {
        const int edge = at_start ? 0 : w, inner = at_start ? fade : w - fade;
        gradient = QLinearGradient(edge, 0, inner, 0);
        area = QRectF(std::min(edge, inner), 0, fade, h);
        const double cx = at_start ? gap : w - gap, cy = h / 2.0;
        const double tip = at_start ? -arm / 2 : arm / 2;
        points << QPointF(cx - tip, cy - arm) << QPointF(cx + tip, cy) << QPointF(cx - tip, cy + arm);
    }
    gradient.setColorAt(0, QColor(0, 0, 0, 150));
    gradient.setColorAt(1, QColor(0, 0, 0, 0));
    painter.fillRect(area, gradient);
    QPen pen(QColor(255, 255, 255, 230), scaled(2));
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.drawPolyline(points);
}

void PreviewPanel::show_warning(const QString &message) {
    warning_label->setText(message);
    place_warning();
    warning_label->raise();
    warning_label->show();
}

void PreviewPanel::clear_warning() { warning_label->hide(); }

ScrollCaptureWindow::ScrollCaptureWindow(const QRect &capture_rect, QWidget *parent, ToolSettingsManager *config_manager)
    : QWidget(parent), capture_rect(capture_rect), config_manager(config_manager), warning_timer_(this), still_timer_(this) {
    load_long_stitch_config();
    warning_timer_.setSingleShot(true);
    connect(&warning_timer_, &QTimer::timeout, this, &ScrollCaptureWindow::show_pending_warning);
    const auto &stitch_config = long_stitch::config();
    // 0 has always meant the library default of 20.
    stitcher_ = new IncrementalStitcher(scaled(PREVIEW_SIDE), stitch_config.ignore_right_pixels ? stitch_config.ignore_right_pixels : 20,
                                        stitch_config.ignore_top_pixels, this);
    connect(stitcher_, &IncrementalStitcher::frame_done, this, &ScrollCaptureWindow::on_frame_stitched, Qt::QueuedConnection);
    connect(stitcher_, &IncrementalStitcher::crop_done, this, &ScrollCaptureWindow::on_cropped, Qt::QueuedConnection);
    auto_scroller_ = new AutoScroller([this](AutoScroller::Done done) { capture_when_still(std::move(done)); }, inject_wheel, {}, this);
    connect(auto_scroller_, &AutoScroller::stopped, this, &ScrollCaptureWindow::on_auto_scroll_stopped);
    change_watch_ = new ChangeWatch(capture_rect, {}, this);
    connect(change_watch_, &ChangeWatch::changed, this, &ScrollCaptureWindow::on_screen_changed, Qt::QueuedConnection);

    use_hdr_ = uses_hdr_engine();

    still_timer_.setSingleShot(true);
    connect(&still_timer_, &QTimer::timeout, this, &ScrollCaptureWindow::check_still);

    setup_window();
    setup_ui();
    setup_input();
    setup_floating_toolbar();
    setup_preview_panel();

    // Checks and fixes the window position once more after 200 ms.
    position_fix_timer_.setSingleShot(true);
    connect(&position_fix_timer_, &QTimer::timeout, this, &ScrollCaptureWindow::force_fix_window_position);
    position_fix_timer_.start(200);
}

QPoint ScrollCaptureWindow::correct_window_position(int border_width) const { return QPoint(capture_rect.x() - border_width, capture_rect.y() - border_width); }

void ScrollCaptureWindow::setup_window() {
    setWindowFlags(Qt::WindowStaysOnTopHint | Qt::FramelessWindowHint | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    // Room for the border; the capture region itself excludes it. The toolbar is separate.
    const int border_width = 3;
    const auto position = correct_window_position(border_width);
    setGeometry(position.x(), position.y(), capture_rect.width() + border_width * 2, capture_rect.height() + border_width * 2);
}

void ScrollCaptureWindow::setup_ui() {
    // Only the transparent bordered area remains.
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(3, 3, 3, 3);
    layout->setSpacing(0);
    transparent_area = new QWidget;
    transparent_area->setFixedSize(capture_rect.width(), capture_rect.height());
    layout->addWidget(transparent_area);
}

void ScrollCaptureWindow::setup_floating_toolbar() {
    toolbar = new FloatingToolbar(this);
    toolbar->reposition = [this] { position_floating_toolbar(); };
    connect(toolbar, &FloatingToolbar::direction_changed, this, &ScrollCaptureWindow::toggle_direction);
    connect(toolbar, &FloatingToolbar::auto_scroll_clicked, this, &ScrollCaptureWindow::toggle_auto_scroll);
    connect(toolbar, &FloatingToolbar::crop_requested, this, &ScrollCaptureWindow::crop);
    connect(toolbar, &FloatingToolbar::save_clicked, this, &ScrollCaptureWindow::on_save);
    connect(toolbar, &FloatingToolbar::ocr_clicked, this, &ScrollCaptureWindow::on_text_recognize);
    connect(toolbar, &FloatingToolbar::pin_clicked, this, &ScrollCaptureWindow::on_pin);
    connect(toolbar, &FloatingToolbar::finish_clicked, this, &ScrollCaptureWindow::on_finish);
    connect(toolbar, &FloatingToolbar::cancel_clicked, this, &ScrollCaptureWindow::on_cancel);
    toolbar->set_result_size(capture_rect.width(), capture_rect.height());
    position_floating_toolbar();
    toolbar->show();
}

void ScrollCaptureWindow::position_floating_toolbar() {
    if (!toolbar) return;
    const int margin = scaled(10);
    QScreen *screen_obj = screen();
    if (!screen_obj) screen_obj = QApplication::primaryScreen();
    const auto screen_geometry = screen_obj->geometry();
    const int tw = toolbar->width(), th = toolbar->height();

    // Horizontally centred x for the above/below options.
    int x_center = x() + python_floordiv(width() - tw, 2);
    x_center = std::max(screen_geometry.left() + margin, std::min(x_center, screen_geometry.right() - margin - tw));

    // 1: above the region.
    const int y_above = y() - th - margin;
    if (y_above >= screen_geometry.top() + margin) {
        toolbar->move(x_center, y_above);
        return;
    }
    // 2: below the region.
    const int y_below = y() + height() + margin;
    if (y_below + th <= screen_geometry.bottom() - margin) {
        toolbar->move(x_center, y_below);
        return;
    }
    // 3/4: left or right, when the region fills the height.
    int y_mid = y() + python_floordiv(height() - th, 2);
    y_mid = std::max(screen_geometry.top() + margin, std::min(y_mid, screen_geometry.bottom() - margin - th));
    if (x() - tw - margin >= screen_geometry.left() + margin) {
        toolbar->move(x() - tw - margin, y_mid);
    } else {
        int x_right = std::min(x() + width() + margin, screen_geometry.right() - margin - tw);
        x_right = std::max(screen_geometry.left() + margin, x_right);
        toolbar->move(x_right, y_mid);
    }
}

void ScrollCaptureWindow::setup_preview_panel() {
    preview_panel = new PreviewPanel(this);
    position_preview_panel();
    preview_panel->show();
    refresh_preview_panel();
}

void ScrollCaptureWindow::position_preview_panel() {
    if (!preview_panel) return;
    auto *panel = preview_panel.data();
    const int margin = 14;
    QScreen *screen_obj = screen();
    if (!screen_obj) screen_obj = QApplication::primaryScreen();
    const auto screen_geometry = screen_obj->geometry();
    const int screen_left = screen_geometry.x(), screen_top = screen_geometry.y();
    const int screen_right = screen_geometry.x() + screen_geometry.width(), screen_bottom = screen_geometry.y() + screen_geometry.height();
    const int capture_left = x(), capture_right = x() + width();
    const int capture_top = y(), capture_bottom = y() + height();

    // The toolbar's position, to stay clear of it.
    std::optional<QRect> toolbar_rect;
    if (toolbar) toolbar_rect = QRect(toolbar->x(), toolbar->y(), toolbar->width(), toolbar->height());
    const QSize size = panel->panel_size();
    const auto overlaps_toolbar = [&](int px, int py) { return toolbar_rect && toolbar_rect->intersects(QRect(px, py, size.width(), size.height())); };
    const auto put = [&](int px, int py) { panel->place(QPoint(px, py), screen_geometry); };

    if (preview_direction_ == "vertical") {
        // Vertical capture: left or right first, bottom edges aligned.
        // 1: right, bottom aligned.
        const int x_right = capture_right + margin;
        if (x_right + size.width() <= screen_right - margin) {
            int py = capture_bottom - size.height();
            py = std::max(screen_top + margin, std::min(py, screen_bottom - size.height() - margin));
            if (!overlaps_toolbar(x_right, py)) {
                put(x_right, py);
                return;
            }
        }
        // 2: left, bottom aligned.
        const int x_left = capture_left - size.width() - margin;
        if (x_left >= screen_left + margin) {
            int py = capture_bottom - size.height();
            py = std::max(screen_top + margin, std::min(py, screen_bottom - size.height() - margin));
            if (!overlaps_toolbar(x_left, py)) {
                put(x_left, py);
                return;
            }
        }
        // 3: above, centred.
        int y_top = capture_top - size.height() - margin;
        if (toolbar_rect && toolbar_rect->bottom() >= y_top - margin) y_top = toolbar_rect->y() - size.height() - margin;
        if (y_top >= screen_top + margin) {
            int px = capture_left + python_floordiv(width() - size.width(), 2);
            px = std::max(screen_left + margin, std::min(px, screen_right - size.width() - margin));
            if (!overlaps_toolbar(px, y_top)) {
                put(px, y_top);
                return;
            }
        }
        // 4: below, centred.
        int y_bottom = capture_bottom + margin;
        if (toolbar_rect && toolbar_rect->top() <= y_bottom + size.height() + margin) y_bottom = toolbar_rect->bottom() + margin;
        if (y_bottom + size.height() <= screen_bottom - margin) {
            int px = capture_left + python_floordiv(width() - size.width(), 2);
            px = std::max(screen_left + margin, std::min(px, screen_right - size.width() - margin));
            if (!overlaps_toolbar(px, y_bottom)) {
                put(px, y_bottom);
                return;
            }
        }
    } else {
        // Horizontal capture: above or below first, right edges aligned.
        // 1: above.
        const int y_top = capture_top - size.height() - margin;
        if (y_top >= screen_top + margin) {
            int px = capture_right - size.width();
            px = std::max(screen_left + margin, std::min(px, screen_right - size.width() - margin));
            if (!overlaps_toolbar(px, y_top)) {
                put(px, y_top);
                return;
            }
        }
        // 2: below (further down when the toolbar is below too).
        int y_bottom = capture_bottom + margin;
        if (toolbar_rect) {
            const int tb_bottom = toolbar_rect->y() + toolbar_rect->height();
            // The toolbar is below the region: the panel goes under it.
            if (toolbar_rect->y() >= capture_bottom) y_bottom = std::max(y_bottom, tb_bottom + margin);
        }
        if (y_bottom + size.height() <= screen_bottom - margin) {
            int px = capture_right - size.width();
            px = std::max(screen_left + margin, std::min(px, screen_right - size.width() - margin));
            put(px, y_bottom);
            return;
        }
        // 3: right.
        const int x_right = capture_right + margin;
        if (x_right + size.width() <= screen_right - margin) {
            int py = capture_top + python_floordiv(height() - size.height(), 2);
            py = std::max(screen_top + margin, std::min(py, screen_bottom - size.height() - margin));
            if (!overlaps_toolbar(x_right, py)) {
                put(x_right, py);
                return;
            }
        }
        // 4: left.
        const int x_left = capture_left - size.width() - margin;
        if (x_left >= screen_left + margin) {
            int py = capture_top + python_floordiv(height() - size.height(), 2);
            py = std::max(screen_top + margin, std::min(py, screen_bottom - size.height() - margin));
            if (!overlaps_toolbar(x_left, py)) {
                put(x_left, py);
                return;
            }
        }
    }
    // Fallback: the screen's top-right corner, clear of the region and the toolbar.
    int px = screen_right - size.width() - margin;
    int py = screen_top + margin;
    if (overlaps_toolbar(px, py) && toolbar_rect) {
        py = toolbar_rect->bottom() + margin;
        if (py + size.height() > screen_bottom - margin) {
            px = screen_left + margin;
            py = screen_top + margin;
        }
    }
    put(px, py);
}

void ScrollCaptureWindow::refresh_preview_panel() {
    if (!preview_panel) return;
    preview_panel->update_preview(latest_preview_, preview_direction_, latest_box_);
    // The panel may have resized; place it clear of the region again.
    position_preview_panel();
}

void ScrollCaptureWindow::show_preview_warning(QString message) {
    warning_timer_.stop();
    pending_warning_.reset();
    preview_warning_active = true;
    if (preview_panel) preview_panel->show_warning(message);
}

void ScrollCaptureWindow::show_pending_warning() {
    if (pending_warning_) show_preview_warning(*pending_warning_);
}

void ScrollCaptureWindow::clear_preview_warning() {
    warning_timer_.stop();
    pending_warning_.reset();
    if (!preview_warning_active) return;
    preview_warning_active = false;
    if (preview_panel) preview_panel->clear_warning();
}

void ScrollCaptureWindow::handle_stitch_failure(const QString &hint, bool immediate) {
    stitch_failing_ = true;
    if (screenshots > 0) screenshots -= 1;
    if (immediate || preview_warning_active) {
        show_preview_warning(hint);
    } else {
        pending_warning_ = hint;
        if (!warning_timer_.isActive()) warning_timer_.start(WARNING_DELAY_MS);
    }
}

void ScrollCaptureWindow::setup_input() {
    // Set on the GUI thread through the Windows API.
    make_click_through(transparent_area);
    log_stitch(T("[OK] 窗口已设置为鼠标穿透模式"));
    try {
        auto *hub = input_hub();
        key_connection_ = connect(hub, &InputHub::key, this, &ScrollCaptureWindow::on_key, Qt::QueuedConnection);
        input_hub_ = hub;
    } catch (const std::exception &e) {
        log_stitch(T("[ERROR] 接入全局按键失败: {e}", {{"e", QString::fromUtf8(e.what())}}), true);
    }
}

void ScrollCaptureWindow::on_screen_changed() {
    if (auto_scroller_->running()) return;
    capture_when_still();
}

void ScrollCaptureWindow::toggle_auto_scroll() {
    if (auto_scroller_->running()) {
        auto_scroller_->stop();
        return;
    }
    const auto center = capture_rect.center();
    // With the wheel going to the focused window, focus is on this program after the
    // toolbar click, so the captured window is brought to the foreground first.
    if (wheel_follows_focus()) activate_window_at(center.x(), center.y());
    auto_scroller_->start(center, scroll_direction == "horizontal");
    toolbar->set_auto_scrolling(true);
    update();
    log_stitch(T("▶️ 开始自动滚动"));
}

void ScrollCaptureWindow::capture_pending_now() {
    auto_scroller_->stop();
    auto_scroller_->flush();
    if (still_timer_.isActive()) flush_still();
    else do_capture();
}

void ScrollCaptureWindow::on_auto_scroll_stopped(const QString &reason) {
    // Auto scrolling stopped because of it, so the reason shows at once.
    if (reason == "failed") show_pending_warning();
    if (toolbar) toolbar->set_auto_scrolling(false);
    update();
    log_stitch(T("⏹️ 自动滚动已停止: {reason}", {{"reason", reason}}));
}

void ScrollCaptureWindow::toggle_direction() {
    auto_scroller_->stop();
    if (scroll_direction == "vertical") {
        scroll_direction = "horizontal";
        toolbar->update_direction("horizontal");
        log_stitch(T("🔄 切换到横向截图模式"));
    } else {
        scroll_direction = "vertical";
        toolbar->update_direction("vertical");
        log_stitch(T("🔄 切换到竖向截图模式"));
    }
    reconfigure_stitch_engine();
    // A long image is stitched in one direction: switching starts a new one from the current view.
    stitcher_->restart();
    auto_scroller_->reset();
    screenshots = 0;
    do_capture();
    refresh_preview_panel();
    if (scroll_direction == "horizontal") start_keyboard_listener();
    else stop_keyboard_listener();
}

void ScrollCaptureWindow::send_horizontal_scroll() {
    // MOUSEEVENTF_HWHEEL, one WHEEL_DELTA to the right.
    const int amount = 1;
    mouse_event(MOUSEEVENTF_HWHEEL, 0, 0, static_cast<DWORD>(amount * 120), 0);
    log_stitch(T("[OK] 发送横向滚动指令: 向右滚动 {amount} 格", {{"amount", amount}}));
}

void ScrollCaptureWindow::start_keyboard_listener() {
    if (!input_hub_) return;
    horizontal_scroll_key_pressed = false;
    input_hub_->native->watch_keys(INPUT_WATCHER, SHIFT_KEYS);
    log_stitch(T("[OK] 键盘监听器已启动（横向模式，按Shift触发）"));
}

void ScrollCaptureWindow::stop_keyboard_listener() {
    if (input_hub_) {
        input_hub_->native->unwatch_keys(INPUT_WATCHER);
        log_stitch(T("[OK] 键盘监听器已停止"));
    }
}

void ScrollCaptureWindow::on_key(const QString &watcher, int vk, bool pressed) {
    if (watcher != INPUT_WATCHER || std::find(SHIFT_KEYS.begin(), SHIFT_KEYS.end(), static_cast<quint32>(vk)) == SHIFT_KEYS.end() ||
        scroll_direction != "horizontal")
        return;
    if (!pressed) {
        horizontal_scroll_key_pressed = false;
        return;
    }
    if (horizontal_scroll_key_pressed) return;
    horizontal_scroll_key_pressed = true;
    log_stitch(T("⌨️ 检测到Shift按下，往右滚一格"));
    send_horizontal_scroll();
}

void ScrollCaptureWindow::reconfigure_stitch_engine() {
    const auto &config = long_stitch::config();
    long_stitch::configure(config.engine, true, std::nullopt, config.ignore_top_pixels);
    if (scroll_direction == "horizontal") log_stitch(T("[OK] 拼接引擎已重新配置: 横向截图（图片旋转90度+竖向拼接）"));
    else log_stitch(T("[OK] 拼接引擎已重新配置: 竖向截图（竖向拼接）"));
    refresh_preview_panel();
}

void ScrollCaptureWindow::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    verify_window_position();
    // One event loop later every floating window goes to the top of TOPMOST, or the
    // taskbar (also HWND_TOPMOST) may cover them on first show.
    QTimer::singleShot(0, this, &ScrollCaptureWindow::raise_all_topmost);
    QTimer::singleShot(0, this, &ScrollCaptureWindow::drop_system_corners);
    // Delayed so the window is fully shown before the first capture.
    QTimer::singleShot(100, this, &ScrollCaptureWindow::capture_initial_screenshot);
}

void ScrollCaptureWindow::drop_system_corners() {
    set_window_rounded_corners(static_cast<quintptr>(winId()), false);
    if (preview_panel) set_window_rounded_corners(static_cast<quintptr>(preview_panel->winId()), false);
}

void ScrollCaptureWindow::raise_all_topmost() {
    raise();
    if (toolbar) toolbar->raise();
    if (preview_panel) preview_panel->raise();
}

void ScrollCaptureWindow::verify_window_position() {
    const int window_x = x(), window_y = y();
    const QPoint window_center(window_x + python_floordiv(width(), 2), window_y + python_floordiv(height(), 2));
    QScreen *current_screen = QApplication::screenAt(window_center);
    if (!current_screen) current_screen = QApplication::primaryScreen();
    const auto screen_geometry = current_screen->geometry();

    log_stitch(T("窗口位置验证:"));
    log_stitch(T("   窗口位置: x={window_x}, y={window_y}", {{"window_x", window_x}, {"window_y", window_y}}));
    log_stitch(T("   窗口中心: x={cx}, y={cy}", {{"cx", window_center.x()}, {"cy", window_center.y()}}));
    log_stitch(T("   所在显示器: {current_screen}", {{"current_screen", screen_repr(current_screen)}}));
    log_stitch(T("   显示器范围: x={x_min}-{x_max}, y={y_min}-{y_max}",
                 {{"x_min", screen_geometry.x()},
                  {"x_max", screen_geometry.x() + screen_geometry.width()},
                  {"y_min", screen_geometry.y()},
                  {"y_max", screen_geometry.y() + screen_geometry.height()}}));

    // The screen holding the capture region's centre.
    const int capture_center_x = capture_rect.x() + python_floordiv(capture_rect.width(), 2);
    const int capture_center_y = capture_rect.y() + python_floordiv(capture_rect.height(), 2);
    const QPoint capture_center(capture_center_x, capture_center_y);
    QScreen *expected_screen = QApplication::screenAt(capture_center);
    log_stitch(T("   截图区域中心: x={capture_center_x}, y={capture_center_y}", {{"capture_center_x", capture_center_x}, {"capture_center_y", capture_center_y}}));
    log_stitch(T("   期望显示器: {expected_screen}", {{"expected_screen", screen_repr(expected_screen)}}));

    if (expected_screen && current_screen != expected_screen) {
        log_stitch(T("[WARN] 警告: 窗口显示在显示器 {screen_name}，但截图区域在不同的显示器", {{"screen_name", current_screen->name()}}));
        QScreen *target_screen = QApplication::screenAt(capture_center);
        if (!target_screen) target_screen = QApplication::primaryScreen();
        const auto target_geometry = target_screen->geometry();
        // border_width = 3
        const int relative_x = capture_rect.x() - 3, relative_y = capture_rect.y() - 3;
        if (relative_x >= target_geometry.x() && relative_y >= target_geometry.y() && relative_x + width() <= target_geometry.x() + target_geometry.width() &&
            relative_y + height() <= target_geometry.y() + target_geometry.height()) {
            log_stitch(T("[FIX] 尝试移动窗口到正确位置: x={relative_x}, y={relative_y}", {{"relative_x", relative_x}, {"relative_y", relative_y}}));
            move(relative_x, relative_y);
            raise();
            activateWindow();
        } else {
            log_stitch(T("[WARN] 无法移动窗口到目标位置，可能会超出显示器边界"));
        }
    } else {
        log_stitch(T("[OK] 窗口位置正确"));
    }
}

void ScrollCaptureWindow::force_fix_window_position() {
    if (!isVisible()) {
        log_stitch(T("[WARN] 检测到窗口不可见，强制显示"));
        show();
        raise();
        activateWindow();
        return;
    }
    const auto window_rect = geometry();
    bool visible_on_any_screen = false;
    for (auto *screen_obj : QApplication::screens())
        if (screen_obj->geometry().intersects(window_rect)) {
            visible_on_any_screen = true;
            break;
        }
    if (!visible_on_any_screen) {
        log_stitch(T("🚨 检测到窗口在所有显示器外，执行强制修复..."));
        const QPoint capture_center(capture_rect.x() + python_floordiv(capture_rect.width(), 2), capture_rect.y() + python_floordiv(capture_rect.height(), 2));
        QScreen *target_screen = QApplication::screenAt(capture_center);
        if (!target_screen) {
            target_screen = QApplication::primaryScreen();
            log_stitch(T("[WARN] 截图区域不在任何显示器内，使用主显示器"));
        }
        const auto target_geometry = target_screen->geometry();
        // Moves the window to the centre of the target screen.
        const int new_x = target_geometry.x() + python_floordiv(target_geometry.width() - width(), 2);
        const int new_y = target_geometry.y() + python_floordiv(target_geometry.height() - height(), 2);
        log_stitch(T("[FIX] 强制移动窗口到显示器 {target_screen} 中央: x={new_x}, y={new_y}",
                     {{"target_screen", screen_repr(target_screen)}, {"new_x", new_x}, {"new_y", new_y}}));
        move(new_x, new_y);
        raise();
        activateWindow();
        // The title tells the user what happened.
        setWindowTitle(tr_text("Long screenshot - window position corrected"));
    } else {
        log_stitch(T("[OK] 窗口位置验证通过"));
    }
}

void ScrollCaptureWindow::capture_initial_screenshot() {
    log_stitch(T("🎬 截取初始截图（第1张）..."));
    do_capture();
    log_stitch(T("   初始截图完成，当前共 {count} 张", {{"count", screenshots}}));
    // Not started when cancelled within 100 ms of showing.
    if (stitcher_) change_watch_->start();
}

void ScrollCaptureWindow::exclude_overlapping_ui(bool exclude) {
    // Only the panel counts: the rest of the preview strip is transparent and never shows in a capture.
    const std::pair<QWidget *, QRect> windows[] = {
        {toolbar, toolbar ? toolbar->geometry() : QRect()},
        {preview_panel.data(), preview_panel ? preview_panel->panel_geometry() : QRect()},
    };
    for (const auto &[widget, rect] : windows)
        if (widget && widget->isVisible() && rect.intersects(capture_rect)) set_window_exclude_from_capture(static_cast<quintptr>(widget->winId()), exclude);
}

QImage ScrollCaptureWindow::grab_capture_rect() {
    if (use_hdr_) {
        try {
            return grab_region_hdr(capture_rect);
        } catch (const std::exception &e) {
            use_hdr_ = false;
            log_stitch(T("HDR 截图失败，本次长截图改用 GDI: {error}", {{"error", QString::fromUtf8(e.what())}}), true);
        }
    }
    // The screen holding the capture region.
    const QPoint center_point(capture_rect.x() + python_floordiv(capture_rect.width(), 2), capture_rect.y() + python_floordiv(capture_rect.height(), 2));
    QScreen *screen_obj = QGuiApplication::screenAt(center_point);
    if (!screen_obj) {
        log_stitch(T("[WARN] 截图区域不在任何显示器范围内，使用主显示器"), true);
        screen_obj = QGuiApplication::primaryScreen();
    }
    const auto screen_geometry = screen_obj->geometry();
    // Virtual-desktop coordinates to coordinates relative to the target screen.
    const int relative_x = capture_rect.x() - screen_geometry.x();
    const int relative_y = capture_rect.y() - screen_geometry.y();
    const auto pixmap = screen_obj->grabWindow(0, relative_x, relative_y, capture_rect.width(), capture_rect.height());
    if (pixmap.isNull()) return {};
    return pixmap.toImage();
}

std::optional<GrabbedFrame> ScrollCaptureWindow::grab_frame() {
    // Before grabbing: exclude UI windows overlapping the region.
    exclude_overlapping_ui(true);
    std::optional<GrabbedFrame> result;
    try {
        const auto qimage = grab_capture_rect();
        if (qimage.isNull()) {
            log_stitch(T("[ERROR] 截图失败"), true);
        } else {
            const auto image = qimage.convertToFormat(QImage::Format_RGB32);
            GrabbedFrame frame{QByteArray(), image.width(), image.height()};
            frame.bgra.reserve(static_cast<qsizetype>(frame.width) * frame.height * 4);
            for (int y = 0; y < frame.height; ++y) frame.bgra.append(reinterpret_cast<const char *>(image.constScanLine(y)), frame.width * 4);
            result = std::move(frame);
        }
    } catch (const std::exception &e) {
        log_stitch(T("[ERROR] 截图时出错: {e}", {{"e", QString::fromUtf8(e.what())}}), true);
    }
    // Done: UI windows may be captured again.
    exclude_overlapping_ui(false);
    return result;
}

std::optional<int> ScrollCaptureWindow::submit_frame(const std::optional<GrabbedFrame> &frame) {
    if (!stitcher_ || !frame) return std::nullopt;
    screenshots += 1;
    // The scroll direction before this frame is unknown; stitching follows the latest movement.
    StitchFrame stitch_frame;
    stitch_frame.bgra = frame->bgra;
    stitch_frame.width = frame->width;
    stitch_frame.height = frame->height;
    stitch_frame.index = screenshots;
    stitch_frame.scroll_direction = scroll_direction;
    stitcher_->submit(stitch_frame);
    return screenshots;
}

std::optional<int> ScrollCaptureWindow::do_capture() {
    if (!stitcher_) return std::nullopt;
    return submit_frame(grab_frame());
}

void ScrollCaptureWindow::capture_when_still(std::function<void(std::optional<int>)> done) {
    if (done) still_callbacks_.push_back(std::move(done));
    if (still_timer_.isActive()) return;
    still_frame_ = stitcher_ ? grab_frame() : std::nullopt;
    if (!still_frame_) {
        finish_still(std::nullopt);
        return;
    }
    still_tries_ = 1;
    still_timer_.start(STILL_CHECK_MS);
}

void ScrollCaptureWindow::check_still() {
    auto frame = grab_frame();
    if (!frame) {
        still_frame_.reset();
        finish_still(std::nullopt);
        return;
    }
    still_tries_ += 1;
    const bool still = still_frame_ && frame->bgra == still_frame_->bgra && !blank(*frame, scroll_direction == "horizontal");
    if (still || still_tries_ >= STILL_CHECK_TRIES) {
        still_frame_.reset();
        finish_still(submit_frame(frame));
    } else {
        still_frame_ = std::move(frame);
        still_timer_.start(STILL_CHECK_MS);
    }
}

void ScrollCaptureWindow::finish_still(std::optional<int> index) {
    auto callbacks = std::move(still_callbacks_);
    still_callbacks_.clear();
    for (auto &done : callbacks) done(index);
}

void ScrollCaptureWindow::flush_still() {
    if (still_timer_.isActive()) {
        still_timer_.stop();
        still_frame_.reset();
        finish_still(do_capture());
    }
}

void ScrollCaptureWindow::on_frame_stitched(const FrameResult &result) {
    // A stitched frame, already on the GUI thread.
    if (!stitcher_) return;
    if (result.ok) {
        toolbar->set_result_size(result.size->first, result.size->second);
        log_stitch(T("📸 第 {screenshot_count} 张 → 拼接结果: {w}x{h}", {{"screenshot_count", result.index}, {"w", result.width}, {"h", result.height}}));
        stitch_failing_ = false;
        clear_preview_warning();
    } else if (result.error) {
        log_stitch(T("[WARN] 第 {screenshot_count} 张拼接出错: {e}", {{"screenshot_count", result.index}, {"e", *result.error}}), true);
        handle_stitch_failure(tr_text("Stitching failed"), true);
    } else {
        if (!stitch_failing_) log_stitch(T("[WARN] 第 {screenshot_count} 张拼接失败，未找到重叠区域", {{"screenshot_count", result.index}}));
        handle_stitch_failure(tr_text("Couldn't join. Scroll back"));
    }
    if (!result.preview.isNull()) {
        latest_preview_ = result.preview;
        latest_box_ = result.box;
        preview_direction_ = result.scroll_direction;
    }
    refresh_preview_panel();
    auto_scroller_->on_frame(result);
}

void ScrollCaptureWindow::crop(const QString &side) {
    if (!stitcher_) return;
    capture_pending_now();
    stitcher_->crop(side);
}

void ScrollCaptureWindow::on_cropped(const FrameResult &result) {
    // Cropped in the background, already on the GUI thread.
    if (!stitcher_) return;
    if (!result.ok) {
        log_stitch(T("[WARN] 裁剪出错: {e}", {{"e", result.error.value_or(QString("None"))}}), true);
        return;
    }
    log_stitch(T("✂️ 裁剪后: {w}x{h}", {{"w", result.width}, {"h", result.height}}));
    toolbar->set_result_size(result.size->first, result.size->second);
    latest_preview_ = result.preview;
    latest_box_ = result.box;
    preview_direction_ = result.scroll_direction;
    refresh_preview_panel();
    auto_scroller_->on_frame(result);
}

QImage ScrollCaptureWindow::export_result() {
    // Waits for submitted frames, then takes the whole image with orientation restored.
    if (!stitcher_) return stitched_result;
    stitched_result = stitcher_->export_image();
    return stitched_result;
}

void ScrollCaptureWindow::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    // A translucent border at the window edge, outside the capture region; green while auto scrolling.
    const QColor color = auto_scroller_ && auto_scroller_->running() ? QColor(82, 196, 26) : QColor(0, 120, 215);
    painter.setPen(QPen(color, 3));
    // The window is capture_rect plus a 3 px border on each side.
    painter.drawRect(QRect(1, 1, width() - 2, height() - 2));
    painter.end();
}

void ScrollCaptureWindow::moveEvent(QMoveEvent *event) {
    QWidget::moveEvent(event);
    position_preview_panel();
    position_floating_toolbar();
}

void ScrollCaptureWindow::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    position_preview_panel();
    position_floating_toolbar();
}

void ScrollCaptureWindow::on_finish() {
    log_stitch(T("[OK] 完成长截图，共 {count} 张图片", {{"count", screenshots}}), true);
    capture_pending_now();
    const auto image = result_qimage();
    // Writing the clipboard turns the long image into a bitmap and PNG, which can take a
    // second or two, so every long screenshot window closes first.
    cleanup();
    hide();
    if (image.isNull()) log_stitch(T("[WARN] 没有拼接结果，跳过保存"));
    else deliver_screenshot(image, config_manager, true, "capture", file_prefix());
    emit finished();
    close();
}

QString ScrollCaptureWindow::file_prefix() const {
    const auto prefix = tr_text("Long Screenshot");
    const auto direction = scroll_direction == "horizontal" ? tr_text("Horizontal") : tr_text("Vertical");
    return prefix + "_" + direction;
}

QImage ScrollCaptureWindow::result_qimage() {
    const auto result = export_result();
    if (result.isNull()) return {};
    // A very long image may run out of memory; the window still has to finish.
    const auto rgb = result.convertToFormat(QImage::Format_RGB888);
    if (rgb.isNull()) {
        log_stitch(T("[ERROR] 复制到剪贴板失败: {e}", {{"e", QString("MemoryError")}}));
        return {};
    }
    return rgb;
}

void ScrollCaptureWindow::on_pin() {
    log_stitch(T("钉图长截图结果..."));
    capture_pending_now();
    if (!config_manager) {
        log_stitch(T("[ERROR] config_manager 未设置，无法创建钉图"));
        return;
    }
    const auto image = result_qimage();
    if (image.isNull()) {
        log_stitch(T("[WARN] 没有拼接结果，无法钉图"));
        return;
    }
    // The pin is centred on the long screenshot region.
    const auto center = capture_rect.center();
    const QPoint position(center.x() - python_floordiv(image.width(), 2), center.y() - python_floordiv(image.height(), 2));
    try {
        PinManager::instance()->create_pin(image, position, config_manager);
    } catch (const std::exception &e) {
        log_stitch(T("[ERROR] 创建钉图失败: {e}", {{"e", QString::fromUtf8(e.what())}}));
        return;
    }
    log_debug(T("钉图已创建，位置: ({pin_x}, {pin_y})", {{"pin_x", position.x()}, {"pin_y", position.y()}}), MODULE_TAG);
    cleanup();
    hide();
    deliver_screenshot(image, config_manager, false, "pin", file_prefix());
    emit finished();
    close();
}

void ScrollCaptureWindow::on_text_recognize() {
    capture_pending_now();
    const auto image = result_qimage();
    if (image.isNull()) {
        log_stitch(T("[WARN] 没有拼接结果，无法识别文字"));
        return;
    }
    const auto anchor = capture_rect;
    const bool copy_directly = config_manager && config_manager->get_ocr_copy_directly_enabled();
    cleanup();
    hide();
    if (copy_directly) copy_text_recognition(image);
    else show_text_recognition(image, anchor);
    emit finished();
    close();
}

void ScrollCaptureWindow::on_save() {
    capture_pending_now();
    const auto image = result_qimage();
    if (image.isNull()) {
        log_stitch(T("[WARN] 没有拼接结果，跳过保存"));
        return;
    }
    const auto base_name = file_prefix() + "_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    ToolSettingsManager *config = config_manager ? config_manager : get_tool_settings_manager();
    cleanup();
    hide();
    // The save window runs once this click has returned, owning nothing of the closing window.
    QTimer::singleShot(0, qApp, [image, base_name, config] {
        ActionTools::save_image_interactively(nullptr, image, base_name, config, std::make_shared<SaveService>(*config));
    });
    emit finished();
    close();
}

void ScrollCaptureWindow::on_cancel() {
    log_stitch(T("[ERROR] 取消长截图"), true);
    screenshots = 0;
    cleanup();
    emit cancelled();
    close();
}

void ScrollCaptureWindow::cleanup() {
    if (change_watch_) change_watch_->stop();
    if (auto_scroller_) auto_scroller_->cancel();
    screenshots = 0;
    stitched_result = QImage();

    auto *stitcher = stitcher_;
    stitcher_ = nullptr;
    if (stitcher) stitcher->close();
    latest_preview_ = QImage();
    latest_box_.reset();

    if (toolbar) {
        toolbar->close();
        log_stitch(T("[OK] 浮动工具栏已关闭"));
    }
    if (preview_panel) {
        preview_panel->close();
        log_stitch(T("[OK] 预览面板已关闭"));
        preview_panel = nullptr;
    }
    still_timer_.stop();
    still_callbacks_.clear();
    warning_timer_.stop();
    position_fix_timer_.stop();

    auto hub = input_hub_;
    input_hub_ = nullptr;
    if (hub) {
        disconnect(key_connection_);
        hub->native->unwatch_keys(INPUT_WATCHER);
    }
}

void ScrollCaptureWindow::closeEvent(QCloseEvent *event) {
    cleanup();
    QWidget::closeEvent(event);
}
}
