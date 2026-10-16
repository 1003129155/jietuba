#include "gif/cursor_overlay.h"
#include "core/resource_manager.h"
#include "core/theme.h"
#include <QPainter>
#include <QPainterPath>
#include <QSvgRenderer>
#include <cmath>
#include <functional>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace jietuba {
namespace {
constexpr double PI = 3.14159265358979323846;
double radians(double degrees) { return degrees * PI / 180.0; }

// Draws with draw(painter, w, h) into tightly packed premultiplied RGBA32.
QByteArray rasterize_qimage(int w, int h, const std::function<void(QPainter &, int, int)> &draw) {
    QImage image(w, h, QImage::Format_RGBA8888_Premultiplied);
    image.fill(0);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    draw(p, w, h);
    p.end();
    QByteArray bytes;
    bytes.reserve(static_cast<qsizetype>(w) * h * 4);
    for (int row = 0; row < h; ++row) bytes.append(reinterpret_cast<const char *>(image.constScanLine(row)), w * 4);
    return bytes;
}
}

QSvgRenderer *CursorOverlay::svg_renderer() {
    static QSvgRenderer *renderer = new QSvgRenderer(ResourceManager::get_icon_path(QString::fromUtf8("鼠标.svg")));
    return renderer;
}

CursorOverlay::CursorOverlay(QWidget *parent) : QWidget(parent), burst_timer_(this) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    // WS_EX_TRANSPARENT: mouse events pass straight through.
    const auto hwnd = reinterpret_cast<HWND>(winId());
    SetWindowLongW(hwnd, GWL_EXSTYLE, GetWindowLongW(hwnd, GWL_EXSTYLE) | WS_EX_TRANSPARENT);
    svg_renderer();
    // About 40 ms per frame (a 25 fps animation).
    burst_timer_.setInterval(40);
    connect(&burst_timer_, &QTimer::timeout, this, &CursorOverlay::on_burst_tick);
}

void CursorOverlay::set_cursor_visible(bool visible) {
    visible_flag_ = visible;
    update();
}

void CursorOverlay::set_frame_cursor(const std::optional<CursorSnapshot> &cursor) {
    const int prev_press = cursor_ ? cursor_->press : 0;
    cursor_ = cursor;
    const int cur_press = cursor ? cursor->press : 0;
    // A press (0 to non-zero) starts the animation; holding stays on frame 1. On release
    // a running animation finishes and the timer stops itself.
    if (cur_press != 0 && prev_press == 0) {
        burst_side_ = cur_press;
        burst_frame_ = 1;
        burst_timer_.start();
    }
    update();
}

void CursorOverlay::on_burst_tick() {
    // Still held: stays on frame 1.
    if (cursor_ && cursor_->press != 0) return;
    burst_frame_ += 1;
    if (burst_frame_ > BURST_FRAMES) {
        burst_frame_ = 0;
        burst_timer_.stop();
    }
    update();
}

void CursorOverlay::paintEvent(QPaintEvent *) {
    if (!visible_flag_ || !cursor_ || !cursor_->visible) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const int x = cursor_->x, y = cursor_->y;
    if (burst_frame_ > 0) draw_burst(p, x, y, burst_side_, burst_frame_);
    if (cursor_->scroll != 0) draw_scroll_arrows(p, x, y, cursor_->scroll);
    // The cursor icon, hot spot at its top-left.
    const int s = CURSOR_SIZE;
    auto *renderer = svg_renderer();
    if (renderer && renderer->isValid()) {
        renderer->render(&p, QRectF(x, y, s, s));
    } else {
        p.setPen(QColor(0, 0, 0));
        p.drawLine(x - 6, y, x + 6, y);
        p.drawLine(x, y - 6, x, y + 6);
    }
    p.end();
}

void CursorOverlay::draw_burst(QPainter &p, int x, int y, int side, int frame) const {
    const int cx = x + DOT_R, cy = y + DOT_R;
    const auto theme = get_theme().theme_color();
    // Fades with each frame.
    const int alpha = std::max(60, 240 - (frame - 1) * 80);
    QColor dot_color(theme);
    dot_color.setAlpha(alpha);
    p.setPen(Qt::NoPen);
    p.setBrush(QBrush(dot_color));
    p.drawEllipse(QPoint(cx, cy), DOT_R, DOT_R);
    // The rays spread with each frame.
    const int r_outer = BURST_R_START + (frame - 1) * BURST_R_STEP;
    const int r_inner = DOT_R + 3;
    QColor line_color(theme);
    line_color.setAlpha(alpha);
    p.setPen(QPen(line_color, 2.5, Qt::SolidLine, Qt::RoundCap));
    // Split along the 135-degree diagonal: left button centred on 225, right on 45.
    const std::vector<int> angles = side == 1 ? std::vector<int>{135, 180, 225, 270, 315} : std::vector<int>{-45, 0, 45, 90, 135};
    for (const int deg : angles) {
        const double angle = radians(deg);
        p.drawLine(QPointF(cx + r_inner * std::cos(angle), cy + r_inner * std::sin(angle)), QPointF(cx + r_outer * std::cos(angle), cy + r_outer * std::sin(angle)));
    }
}

void CursorOverlay::draw_scroll_arrows(QPainter &p, int x, int y, int scroll) const {
    const auto theme = get_theme().theme_color();
    const int ax = x + CURSOR_SIZE + 10;
    const int ay = y + CURSOR_SIZE / 2;
    const int arrow_h = 9, arrow_w = 15, gap = 5;
    p.setPen(Qt::NoPen);
    for (int i = 0; i < 3; ++i) {
        const int offset = (i - 1) * (arrow_h + gap);
        const int alpha = scroll == 1 ? 80 + i * 70 : 220 - i * 70;
        QColor color(theme);
        color.setAlpha(std::max(60, alpha));
        p.setBrush(QBrush(color));
        QPainterPath path;
        const int base_y = ay + offset;
        const int tip_y = scroll == 1 ? ay + offset - arrow_h : ay + offset + arrow_h;
        path.moveTo(ax, tip_y);
        path.lineTo(ax - arrow_w / 2, base_y);
        path.lineTo(ax + arrow_w / 2, base_y);
        path.closeSubpath();
        p.drawPath(path);
    }
}

gifrecorder::CursorSprites rasterize_cursor_sprites(double scale) {
    const auto theme = get_theme().theme_color();
    // Sizes scaled to the export.
    const int cs = std::max(8, static_cast<int>(CursorOverlay::CURSOR_SIZE * scale + 0.5));
    const int dot_r = std::max(2, static_cast<int>(CursorOverlay::DOT_R * scale + 0.5));
    const int r_start = std::max(4, static_cast<int>(CursorOverlay::BURST_R_START * scale + 0.5));
    const int r_step = std::max(3, static_cast<int>(CursorOverlay::BURST_R_STEP * scale + 0.5));
    const double line_w = std::max(1.0, 2.5 * scale);
    // Burst sprites must hold the last frame's rays.
    const int max_r = r_start + (BURST_FRAMES - 1) * r_step + 4;
    const int burst_size = std::max(16, (max_r + dot_r) * 2 + 4);
    const int arrow_h = std::max(4, static_cast<int>(9 * scale));
    const int arrow_w = std::max(6, static_cast<int>(15 * scale));
    const int gap = std::max(2, static_cast<int>(5 * scale));
    const int scroll_h = 3 * arrow_h + 2 * gap + 4;
    const int scroll_w = arrow_w + 4;
    auto *renderer = CursorOverlay::svg_renderer();

    gifrecorder::CursorSprites result;
    result.cursor = {rasterize_qimage(cs, cs,
                                      [renderer](QPainter &p, int w, int h) {
                                          if (renderer && renderer->isValid()) renderer->render(&p, QRectF(0, 0, w, h));
                                      }),
                     cs, cs};

    for (const int side : {1, 2}) {
        const std::vector<int> angles = side == 1 ? std::vector<int>{135, 180, 225, 270, 315} : std::vector<int>{-45, 0, 45, 90, 135};
        for (int frame = 1; frame <= BURST_FRAMES; ++frame) {
            const int alpha = std::max(60, 240 - (frame - 1) * 80);
            const int r_outer = r_start + (frame - 1) * r_step;
            const int r_inner = dot_r + 3;
            auto draw_burst = [&](QPainter &p, int w, int h) {
                const int cx = w / 2, cy = h / 2;
                QColor color(theme);
                color.setAlpha(alpha);
                p.setPen(Qt::NoPen);
                p.setBrush(QBrush(color));
                p.drawEllipse(QPointF(cx, cy), dot_r, dot_r);
                p.setPen(QPen(color, line_w, Qt::SolidLine, Qt::RoundCap));
                for (const int deg : angles) {
                    const double a = radians(deg);
                    p.drawLine(QPointF(cx + r_inner * std::cos(a), cy + r_inner * std::sin(a)), QPointF(cx + r_outer * std::cos(a), cy + r_outer * std::sin(a)));
                }
            };
            auto &target = side == 1 ? result.burst_left[frame - 1] : result.burst_right[frame - 1];
            target = {rasterize_qimage(burst_size, burst_size, draw_burst), burst_size, burst_size};
        }
    }

    for (const int scroll_dir : {1, -1}) {
        auto draw_scroll = [&](QPainter &p, int w, int h) {
            const int ax = w / 2, ay = h / 2;
            p.setPen(Qt::NoPen);
            for (int i = 0; i < 3; ++i) {
                const int offset = (i - 1) * (arrow_h + gap);
                const int alpha_val = scroll_dir == 1 ? 80 + i * 70 : 220 - i * 70;
                QColor color(theme);
                color.setAlpha(std::max(60, alpha_val));
                p.setBrush(QBrush(color));
                QPainterPath path;
                path.moveTo(ax, scroll_dir == 1 ? ay + offset - arrow_h : ay + offset + arrow_h);
                path.lineTo(ax - arrow_w / 2, ay + offset);
                path.lineTo(ax + arrow_w / 2, ay + offset);
                path.closeSubpath();
                p.drawPath(path);
            }
        };
        auto &target = scroll_dir == 1 ? result.scroll_up : result.scroll_down;
        target = {rasterize_qimage(scroll_w, scroll_h, draw_scroll), scroll_w, scroll_h};
    }
    return result;
}

std::vector<std::pair<int, int>> compute_burst_states(const std::vector<std::optional<CursorSnapshot>> &cursors) {
    std::vector<std::pair<int, int>> states(cursors.size(), {0, 0});
    int burst_frame = 0, burst_side = 0, prev_press = 0;
    for (size_t i = 0; i < cursors.size(); ++i) {
        const auto &cur = cursors[i];
        const int cur_press = cur && cur->visible ? cur->press : 0;
        if (cur_press != 0 && prev_press == 0) {
            burst_side = cur_press;
            burst_frame = 1;
        } else if (cur_press != 0) {
            burst_frame = 1;
        } else if (burst_frame > 0) {
            burst_frame += 1;
            if (burst_frame > BURST_FRAMES) {
                burst_frame = 0;
                burst_side = 0;
            }
        }
        states[i] = {burst_frame, burst_side};
        prev_press = cur_press;
    }
    return states;
}
}
