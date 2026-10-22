#include "annotation/annotation_toolbar.h"
#include "annotation/annotation_view.h"
#include "annotation/screen_annotation.h"
#include "core/logger.h"
#include "core/platform_utils.h"
#include "core/ui_scale.h"
#include "settings/tool_settings.h"
#include "ui/arrow_settings_panel.h"
#include "ui/mosaic_settings_panel.h"
#include "ui/number_settings_panel.h"
#include "ui/paint_settings_panel.h"
#include "ui/shape_settings_panel.h"
#include "ui/text_settings_panel.h"
#include "ui/toolbar_layout.h"
#include "ui/watermark_settings_panel.h"
#include <QCursor>
#include <QGuiApplication>
#include <QScreen>
#include <algorithm>

namespace jietuba {
AnnotationToolbar::AnnotationToolbar(ScreenAnnotation *owner) : Toolbar(nullptr, toolbar_layout::ANNOTATION), owner_(owner) {
    const int tool = BASE_BTN_WIDTH;
    pointer_btn = add_button("annotation_pointer", QString::fromUtf8("svg/穿透.svg"), "Pass through: the mouse works the windows below (Esc)", tool,
                             [this] { emit pointer_clicked(); }, true);
    board_btn = add_button("annotation_board", "svg/白板.svg", "Whiteboard: click to cover the screen with white, click again to remove", tool,
                           [this] { emit board_clicked(); }, true);
    highlight_btn = add_button("annotation_cursor", "svg/光标高亮.svg", "Cursor highlight: a light around the pointer and a ring for every click", tool,
                               [this] { emit highlight_clicked(); }, true);
    clear_btn = add_button("annotation_clear", QString::fromUtf8("svg/清空.svg"), "Clear all drawings", tool, [this] { emit clear_clicked(); });
    hide_btn = add_button("annotation_hide", QString::fromUtf8("svg/隐藏标注.svg"), "Hide / show drawings", tool, [this] { emit hide_clicked(); }, true);
    exit_btn = add_button("annotation_exit", QString::fromUtf8("svg/结束截图.svg"), "Exit screen annotation", BASE_WIDE_WIDTH, [this] { emit exit_clicked(); });
    // Clicks must leave the focus with the annotated window.
    setWindowFlag(Qt::WindowDoesNotAcceptFocus, true);
    // It starts at the bottom of a screen, so panels open above it.
    toolbar_below_selection_ = false;
    // Sizes and icons of the added buttons, then the layout with them.
    apply_scale();
}

void AnnotationToolbar::place_initially() {
    set_manual_positioned(false);
    auto *screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen) screen = QGuiApplication::primaryScreen();
    const QRect area = screen->availableGeometry();
    move(area.left() + (area.width() - width()) / 2, area.bottom() + 1 - height() - scaled(24));
}

void AnnotationToolbar::place_at_top() {
    set_manual_positioned(true);
    auto *screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen) screen = QGuiApplication::primaryScreen();
    const QRect area = screen->availableGeometry();
    move(area.left() + (area.width() - width()) / 2, area.top() + scaled(24));
}

void AnnotationToolbar::place_at_start() {
    auto *config = get_tool_settings_manager();
    const QStringList saved = config->get_screen_annotation_toolbar_pos().split(',');
    bool ok_x = false, ok_y = false;
    const QPoint pos = saved.size() == 2 ? QPoint(saved[0].toInt(&ok_x), saved[1].toInt(&ok_y)) : QPoint();
    const QString where = config->get_screen_annotation_toolbar_position();
    if (where == "top") {
        place_at_top();
        return;
    }
    if (where != "last" || !ok_x || !ok_y) {
        place_initially();
        return;
    }
    set_manual_positioned(true);
    move(pos);
    keep_on_screen();
}

void AnnotationToolbar::remember_position() const {
    get_tool_settings_manager()->set_screen_annotation_toolbar_pos(QString("%1,%2").arg(x()).arg(y()));
}

void AnnotationToolbar::edit_layout(QWidget *parent) {
    // The dialog takes its icons from a toolbar's buttons; this one is never shown.
    AnnotationToolbar toolbar(nullptr);
    toolbar.layout_dialog_parent_ = parent;
    toolbar.open_layout_dialog();
}

void AnnotationToolbar::keep_on_screen() {
    const QRect frame(pos(), size());
    auto *screen = QGuiApplication::screenAt(frame.center());
    if (!screen) {
        place_initially();
        return;
    }
    const QRect area = screen->availableGeometry();
    const int x = std::clamp(frame.x(), area.left(), std::max(area.left(), area.right() + 1 - width()));
    const int y = std::clamp(frame.y(), area.top(), std::max(area.top(), area.bottom() + 1 - height()));
    if (QPoint(x, y) != pos()) move(x, y);
}

void AnnotationToolbar::set_mode_checks(bool pass_through, bool hidden) {
    pointer_btn->setChecked(pass_through);
    hide_btn->setChecked(hidden);
}

void AnnotationToolbar::set_effect_checks(bool board, bool highlight) {
    board_btn->setChecked(board);
    highlight_btn->setChecked(highlight);
}

QList<QWidget *> AnnotationToolbar::floating_windows() const {
    QList<QWidget *> windows;
    for (QWidget *window : std::initializer_list<QWidget *>{paint_panel, shape_panel, arrow_panel, number_panel, text_panel, mosaic_panel, watermark_panel, more_popup()})
        if (window) windows.append(window);
    return windows;
}

void AnnotationToolbar::prepare_floating_windows() {
    auto windows = floating_windows();
    windows.prepend(this);
    const bool hidden = get_tool_settings_manager()->get_screen_annotation_hide_toolbar_in_captures();
    for (auto *window : windows) {
        // Qt shows tooltips only for the active window otherwise.
        window->setAttribute(Qt::WA_AlwaysShowToolTips);
        if (!set_window_exclude_from_capture(static_cast<quintptr>(window->winId()), hidden))
            log_debug(T("屏幕标注工具栏未能排除出截图: {error_code}", {{"error_code", get_last_error()}}), "Annotation");
    }
}

void AnnotationToolbar::close_with_floating_windows() {
    for (auto *window : floating_windows()) {
        window->close();
        window->deleteLater();
    }
    paint_panel = nullptr;
    shape_panel = nullptr;
    arrow_panel = nullptr;
    number_panel = nullptr;
    text_panel = nullptr;
    mosaic_panel = nullptr;
    watermark_panel = nullptr;
    close();
    deleteLater();
}

QWidget *AnnotationToolbar::host_window() const {
    if (layout_dialog_parent_) return layout_dialog_parent_;
    return owner_ ? owner_->active_view() : nullptr;
}

void AnnotationToolbar::reposition_self() {
    if (isVisible()) keep_on_screen();
}

void AnnotationToolbar::reset_auto_position() { place_initially(); }

void AnnotationToolbar::showEvent(QShowEvent *event) {
    Toolbar::showEvent(event);
    prepare_floating_windows();
}
}
