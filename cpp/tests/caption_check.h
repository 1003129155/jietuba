#pragma once
// The caption every self-drawn window shares: application icon, then the title, both on the left.
// Windows differ only in which caption buttons they show, never in how the title is laid out.
#include "core/ui_scale.h"
#include "ui/fluent_lite/titlebar.h"
#include <QGuiApplication>
#include <QIcon>
#include <QPixmap>
#include <QString>
#include <QWidget>

namespace jietuba {
// The tests run without main_app's application icon; a window reads it when its caption is built,
// so call this before constructing the window.
inline void use_test_app_icon() {
    if (!QGuiApplication::windowIcon().isNull()) return;
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::darkCyan);
    QGuiApplication::setWindowIcon(QIcon(pixmap));
}

// Empty when the window's caption follows the shared layout, else what differs.
inline QString caption_problem(QWidget &window) {
    auto *bar = window.findChild<FluentTitleBar *>();
    if (!bar) return "no FluentTitleBar";
    // Laid out the way the frame does once the window is shown.
    bar->setGeometry(0, 0, 480, bar->height());
    bar->layout()->activate();
    if (window.windowTitle().isEmpty()) return "empty window title";
    if (bar->titleLabel->text() != window.windowTitle()) return "caption text differs from the window title";
    if (!bar->titleLabel->isVisibleTo(bar)) return "title hidden";
    if (!bar->iconLabel->isVisibleTo(bar) || bar->iconLabel->pixmap().isNull()) return "application icon missing";
    if (bar->iconLabel->x() != widget_scaled(bar, 8)) return QString("icon at x=%1, not the shared inset").arg(bar->iconLabel->x());
    if (bar->titleLabel->x() != bar->iconLabel->geometry().right() + 1) return "title does not follow the icon";
    if (bar->titleLabel->geometry().center().x() >= bar->width() / 3) return "title is not left-aligned";
    return {};
}
}

#define VERIFY_STANDARD_CAPTION(window) \
    do { \
        const QString caption_problem_text = ::jietuba::caption_problem(window); \
        QVERIFY2(caption_problem_text.isEmpty(), qPrintable(caption_problem_text)); \
    } while (false)
