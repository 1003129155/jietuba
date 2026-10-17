#include "ui/initial_selection.h"
#include "canvas/view.h"
#include "capture/msaa_element_finder.h"
#include "core/logger.h"

namespace jietuba {
InitialSelectionGate::InitialSelectionGate(QObject *parent) : QObject(parent) {
    poll_.setInterval(8);
    connect(&poll_, &QTimer::timeout, this, &InitialSelectionGate::check);
    deadline_.setSingleShot(true);
    deadline_.setTimerType(Qt::PreciseTimer);
    connect(&deadline_, &QTimer::timeout, this, [this] {
        check();
        if (is_waiting()) finish();
    });
}
void InitialSelectionGate::start(CanvasView *view) {
    cancel();
    view_ = view;
    if (view->element_finder)
        result_connection_ = connect(view->element_finder.get(), &MSAAElementFinder::cache_updated, this, &InitialSelectionGate::check);
    deadline_.start(WAIT_MS);
    poll_.start();
    check();
}
void InitialSelectionGate::check() {
    const auto view = view_;
    if (!view || checking_) return;
    checking_ = true;
    bool complete = true;
    try { complete = view->prepare_initial_selection(); }
    catch (const std::exception &e) { log_exception(e, T("智能选区初始化")); }
    checking_ = false;
    if (complete && view_ == view) finish();
}
void InitialSelectionGate::finish() {
    const auto view = view_;
    cancel();
    if (view) emit ready(view);
}
void InitialSelectionGate::cancel() {
    deadline_.stop();
    poll_.stop();
    disconnect(result_connection_);
    result_connection_ = {};
    view_.clear();
}
}
