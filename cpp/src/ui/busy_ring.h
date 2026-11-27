#pragma once
#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>
#include <optional>

class QAbstractButton;

namespace jietuba {
// A thin ring around the icon of the button it sits on, while something the button started is
// still working: an arc turning, or, once the size of the work is known, an arc growing clockwise
// from the top as it gets done. It covers the button and lets every click through to it.
class BusyRing : public QWidget {
public:
    // A wait shorter than this shows nothing.
    static constexpr int SHOW_DELAY_MS = 200;
    static constexpr int TURN_MS = 900;
    explicit BusyRing(QAbstractButton *button);
    void set_busy(bool busy);
    // done out of total; a total of 0, as before it is known, turns the arc instead.
    void set_progress(int done, int total);
    // The share of the work done, nullopt while the arc turns.
    std::optional<double> progress() const { return progress_; }
    // The ring's circle in its own (the button's) coordinates.
    QRectF circle() const;

protected:
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QAbstractButton *button_;
    QTimer delay_, frames_;
    QElapsedTimer clock_;
    std::optional<double> progress_;
    // The share drawn, which catches up with progress_ over a few frames.
    double drawn_ = 0;
};
}
