#include "ui/motion.h"
#include <QApplication>
#include <QMenu>
#include <QPointer>
#include <QPropertyAnimation>
#include <QWidget>
#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#endif

namespace jietuba::motion {
namespace {
std::optional<bool> g_override;
const char *const FADE_NAME = "PopupFade";

void set_cloaked(QWidget *popup, bool cloaked) {
#ifdef Q_OS_WIN
    BOOL value = cloaked;
    DwmSetWindowAttribute(reinterpret_cast<HWND>(popup->internalWinId()), DWMWA_CLOAK, &value, sizeof(value));
#else
    Q_UNUSED(popup); Q_UNUSED(cloaked);
#endif
}

bool fades(const QWidget *window) { return qobject_cast<const QMenu *>(window) || window->inherits("QComboBoxPrivateContainer"); }

// A popup's window is shown before it is painted, so an empty frame of it can reach the screen.
// From its show event until the next pass of the event queue, by when it has been painted, the popup
// is kept out of the composition: transparent when it then fades in, cloaked when it does not.
class PopupFade : public QObject {
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() != QEvent::Show || !watched->isWidgetType()) return false;
        auto *popup = static_cast<QWidget *>(watched);
        if (!popup->isWindow() || !popup->internalWinId() || !fades(popup)) return false;
        delete popup->findChild<QPropertyAnimation *>(FADE_NAME, Qt::FindDirectChildrenOnly);
        const int length = duration(FAST_MS);
        popup->setWindowOpacity(length > 0 ? 0.0 : 1.0);
        if (length <= 0) set_cloaked(popup, true);
        QMetaObject::invokeMethod(this, [guard = QPointer<QWidget>(popup), length] {
            if (!guard) return;
            if (guard->isVisible()) guard->repaint();
            if (length <= 0) {
                set_cloaked(guard, false);
                return;
            }
            auto *fade = new QPropertyAnimation(guard, "windowOpacity", guard);
            fade->setObjectName(FADE_NAME);
            fade->setDuration(length);
            fade->setStartValue(0.0);
            fade->setEndValue(1.0);
            fade->start(QAbstractAnimation::DeleteWhenStopped);
        }, Qt::QueuedConnection);
        return false;
    }
};
}

bool enabled() {
    if (g_override) return *g_override;
#ifdef Q_OS_WIN
    BOOL on = TRUE;
    return !SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0) || on;
#else
    return true;
#endif
}

int duration(int ms) { return enabled() ? ms : 0; }

QEasingCurve decelerate() {
    QEasingCurve curve(QEasingCurve::BezierSpline);
    curve.addCubicBezierSegment(QPointF(0, 0), QPointF(0, 1), QPointF(1, 1));
    return curve;
}

void install_popup_fade(QApplication &application) {
    QApplication::setEffectEnabled(Qt::UI_AnimateCombo, false);
    QApplication::setEffectEnabled(Qt::UI_AnimateMenu, false);
    QApplication::setEffectEnabled(Qt::UI_FadeMenu, false);
    static QPointer<PopupFade> filter;
    if (filter) return;
    filter = new PopupFade(&application);
    application.installEventFilter(filter);
}

void set_enabled_override(std::optional<bool> on) { g_override = on; }
}
