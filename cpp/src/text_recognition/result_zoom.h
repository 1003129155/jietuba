#pragma once
#include <QObject>

class QAbstractScrollArea;

namespace jietuba {
// Shared display zoom for the editable recognition results; content and undo stay intact.
class ResultZoom : public QObject {
    Q_OBJECT
public:
    static constexpr int MIN_ZOOM = 50;
    static constexpr int MAX_ZOOM = 300;
    static constexpr int ZOOM_STEP = 10;
    explicit ResultZoom(QAbstractScrollArea *view);
    int percent() const { return percent_; }
    double factor() const { return percent_ / 100.0; }
    void set_percent(int percent);
signals:
    void changed();
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    int percent_ = 100;
    int wheel_delta_ = 0;
};
}
