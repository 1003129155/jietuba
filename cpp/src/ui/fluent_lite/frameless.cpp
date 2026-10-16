#include "ui/fluent_lite/frameless.h"
#include "ui/fluent_lite/theme.h"
#include <QEnterEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWindow>
#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#include <windowsx.h>
#endif

namespace jietuba {
void FramelessHelper::install() {
    window_->setWindowFlags(window_->windowFlags() | Qt::FramelessWindowHint);
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(window_->winId());
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    SetWindowLongW(hwnd, GWL_STYLE, style | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CAPTION | WS_THICKFRAME);
    // A one-pixel extended frame keeps the DWM shadow.
    const MARGINS margins = {0, 0, 1, 0};
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
#endif
}

bool FramelessHelper::native_event(const QByteArray &event_type, void *message, qintptr *result) {
#ifdef Q_OS_WIN
    if (event_type != "windows_generic_MSG") return false;
    auto *msg = static_cast<MSG *>(message);
    // internalWinId, not winId: messages arrive while the native window is still being
    // created, and winId would then try to create it again.
    if (!msg || !window_->internalWinId() || msg->hwnd != reinterpret_cast<HWND>(window_->internalWinId())) return false;
    if (msg->message == WM_NCCALCSIZE && msg->wParam) {
        // Client area covers the whole window; when maximised the frame would
        // spill past the monitor, so it is pulled back by the frame thickness.
        if (IsZoomed(msg->hwnd)) {
            auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(msg->lParam);
            const UINT dpi = GetDpiForWindow(msg->hwnd);
            const int frame = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            params->rgrc[0].left += frame;
            params->rgrc[0].top += frame;
            params->rgrc[0].right -= frame;
            params->rgrc[0].bottom -= frame;
        }
        *result = 0;
        return true;
    }
    if (msg->message == WM_NCHITTEST && resize_enabled && !IsZoomed(msg->hwnd)) {
        const QPoint global(GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam));
        RECT rect;
        GetWindowRect(msg->hwnd, &rect);
        const int border = static_cast<int>(BORDER_WIDTH * window_->devicePixelRatioF());
        const bool left = global.x() < rect.left + border, right = global.x() >= rect.right - border;
        const bool top = global.y() < rect.top + border, bottom = global.y() >= rect.bottom - border;
        LRESULT hit = 0;
        if (top && left) hit = HTTOPLEFT;
        else if (top && right) hit = HTTOPRIGHT;
        else if (bottom && left) hit = HTBOTTOMLEFT;
        else if (bottom && right) hit = HTBOTTOMRIGHT;
        else if (left) hit = HTLEFT;
        else if (right) hit = HTRIGHT;
        else if (top) hit = HTTOP;
        else if (bottom) hit = HTBOTTOM;
        if (hit) {
            *result = hit;
            return true;
        }
    }
#else
    Q_UNUSED(event_type); Q_UNUSED(message); Q_UNUSED(result);
#endif
    return false;
}

FramelessWindow::FramelessWindow(QWidget *parent) : QWidget(parent), helper_(this) {
    helper_.install();
    title_bar_ = new TitleBar(this);
    title_bar_->raise();
}

void FramelessWindow::setTitleBar(TitleBar *title_bar) {
    if (title_bar_) title_bar_->deleteLater();
    title_bar_ = title_bar;
    title_bar_->setParent(this);
    title_bar_->raise();
    title_bar_->resize(width(), title_bar_->height());
}

bool FramelessWindow::nativeEvent(const QByteArray &event_type, void *message, qintptr *result) {
    return helper_.native_event(event_type, message, result) || QWidget::nativeEvent(event_type, message, result);
}

void FramelessWindow::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    if (title_bar_) title_bar_->resize(width(), title_bar_->height());
}

FramelessDialog::FramelessDialog(QWidget *parent) : QDialog(parent), helper_(this) {
    helper_.install();
    title_bar_ = new TitleBar(this);
    title_bar_->minBtn->hide();
    title_bar_->maxBtn->hide();
    title_bar_->setDoubleClickEnabled(false);
    title_bar_->raise();
}

void FramelessDialog::setTitleBar(TitleBar *title_bar) {
    if (title_bar_) title_bar_->deleteLater();
    title_bar_ = title_bar;
    title_bar_->setParent(this);
    title_bar_->raise();
    title_bar_->resize(width(), title_bar_->height());
}

bool FramelessDialog::nativeEvent(const QByteArray &event_type, void *message, qintptr *result) {
    return helper_.native_event(event_type, message, result) || QDialog::nativeEvent(event_type, message, result);
}

void FramelessDialog::resizeEvent(QResizeEvent *event) {
    QDialog::resizeEvent(event);
    if (title_bar_) title_bar_->resize(width(), title_bar_->height());
}

FrostedFramelessDialog::FrostedFramelessDialog(QWidget *parent) : FramelessDialog(parent) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { update(); });
}

void FrostedFramelessDialog::paintEvent(QPaintEvent *) { paint_solid_background(this, frosted_corner_radius_); }

TitleBarButton::TitleBarButton(QWidget *parent) : QAbstractButton(parent) {
    setCursor(Qt::ArrowCursor);
    setFixedSize(46, 32);
}

std::pair<QColor, QColor> TitleBarButton::colors() const {
    if (isDown()) return {pressed_color_, pressed_bg_};
    if (underMouse()) return {hover_color_, hover_bg_};
    return {normal_color_, normal_bg_};
}

void TitleBarButton::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), colors().second);
}

void TitleBarButton::enterEvent(QEnterEvent *event) {
    update();
    QAbstractButton::enterEvent(event);
}

void TitleBarButton::leaveEvent(QEvent *event) {
    update();
    QAbstractButton::leaveEvent(event);
}

TitleBar::TitleBar(QWidget *parent) : QWidget(parent) {
    hBoxLayout = new QHBoxLayout(this);
    hBoxLayout->setSpacing(0);
    hBoxLayout->setContentsMargins(0, 0, 0, 0);
    hBoxLayout->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    hBoxLayout->addStretch(1);
    setFixedHeight(32);
    minBtn = new TitleBarButton(this);
    maxBtn = new TitleBarButton(this);
    closeBtn = new TitleBarButton(this);
    hBoxLayout->addWidget(minBtn, 0, Qt::AlignRight);
    hBoxLayout->addWidget(maxBtn, 0, Qt::AlignRight);
    hBoxLayout->addWidget(closeBtn, 0, Qt::AlignRight);
    connect(minBtn, &QAbstractButton::clicked, this, [this] { window()->showMinimized(); });
    connect(maxBtn, &QAbstractButton::clicked, this, [this] { toggle_max_state(window()); });
    connect(closeBtn, &QAbstractButton::clicked, this, [this] { window()->close(); });
    window()->installEventFilter(this);
}

bool TitleBar::is_drag_region(const QPoint &pos) const {
    auto *child = childAt(pos);
    return !child || !qobject_cast<QAbstractButton *>(child);
}

void TitleBar::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && is_drag_region(event->position().toPoint())) {
        if (auto *handle = window()->windowHandle()) handle->startSystemMove();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && double_click_enabled_ && is_drag_region(event->position().toPoint())) {
        toggle_max_state(window());
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

bool TitleBar::eventFilter(QObject *watched, QEvent *event) {
    if (watched == window() && event->type() == QEvent::WindowStateChange) update_max_state();
    return QWidget::eventFilter(watched, event);
}

void TitleBar::update_max_state() {}

void toggle_max_state(QWidget *window) {
    if (window->isMaximized()) window->showNormal();
    else window->showMaximized();
}
}
