#pragma once
#include <QLabel>
#include <QTimer>

class QPaintEvent;
class QScreen;
class QShowEvent;

namespace jietuba {
class ScreenshotCountdown : public QLabel {
    Q_OBJECT
public:
    explicit ScreenshotCountdown(QScreen *screen);

signals:
    void finished();

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    QTimer timer_;
    int remaining_ = 3;
};
}
