#pragma once
#include <QObject>
#include <QTimer>
#include <functional>

namespace jietuba {
// The cycling dots after "Translating...", shared by both translation entry points
// (the shortcut popup and the dialog). The hosts write their text in different
// places, so the text goes to a callback.
class EllipsisAnimator : public QObject {
    Q_OBJECT
public:
    static constexpr int INTERVAL_MS = 320;
    // 0-3 dots
    static constexpr int STEPS = 4;
    explicit EllipsisAnimator(QObject *parent = nullptr, std::function<void(const QString &)> on_tick = {});
    void start();
    void stop();
    bool is_running() const { return timer_.isActive(); }
    // Frame step's text. The translated label's own trailing dots are removed first
    // because languages use different ellipses.
    static QString text_for(int step);
private:
    void advance();
    std::function<void(const QString &)> on_tick_;
    int step_ = 0;
    QTimer timer_;
};
}
