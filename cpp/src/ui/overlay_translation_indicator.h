#pragma once
#include "translation/overlay_translation_session.h"
#include "ui/translation_sweep.h"
#include <QPointer>

class QGraphicsView;

namespace jietuba {
// The wait for an overlay translation, swept over the selection on the capture until the run
// ends. A widget over the view's viewport, like the selection handles, so an export never
// includes it; every click goes through it to the view.
class OverlayTranslationIndicator : public TranslationSweep {
public:
    explicit OverlayTranslationIndicator(QGraphicsView *view);
    void set_waiting(const OverlayTranslationWait &waiting);

protected:
    QRect area() const override;
    bool dark() const override { return waiting_.dark; }
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QPointer<QGraphicsView> view_;
    OverlayTranslationWait waiting_;
};
}
