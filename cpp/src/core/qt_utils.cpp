#include "core/qt_utils.h"
#include <QApplication>
#include <QLayout>
#include <QWidget>
#include <QWindow>

namespace jietuba {
QList<QWidget *> layout_widgets(QLayout *layout) {
    QList<QWidget *> widgets;
    for (int index = 0; index < layout->count(); ++index)
        if (auto *widget = layout->itemAt(index)->widget()) widgets.append(widget);
    return widgets;
}

namespace {
QList<QWindow *> window_chain(QWindow *window) {
    QList<QWindow *> chain;
    while (window) {
        chain.append(window);
        window = window->parent(QWindow::IncludeTransients);
    }
    return chain;
}
}

QWidget *blocking_modal(QWidget *widget) {
    // Visible top-levels rather than activeModalWidget(): a modal's Show event
    // arrives before Qt registers it as modal.
    const auto target = widget ? window_chain(widget->window()->windowHandle()) : QList<QWindow *>{};
    for (auto *candidate : QApplication::topLevelWidgets()) {
        if (!(candidate->isVisible() && candidate->isModal())) continue;
        auto *handle = candidate->windowHandle();
        if (!handle || target.contains(handle)) continue;
        if (candidate->windowModality() == Qt::ApplicationModal) return candidate;
        for (auto *window : window_chain(handle))
            if (target.contains(window)) return candidate;
    }
    return nullptr;
}

void close_active_popups() {
    auto *popup = QApplication::activePopupWidget();
    while (popup) {
        popup->close();
        auto *remaining = QApplication::activePopupWidget();
        if (remaining == popup) return;  // a popup that refuses to close is not retried
        popup = remaining;
    }
}
}
