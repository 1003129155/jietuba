#pragma once
#include <QObject>
#include <QRectF>
#include <QSizeF>

namespace jietuba {
// Direct port: main/canvas/selection_model.py; intentionally preserve signal order.
class SelectionModel : public QObject {
    Q_OBJECT
public:
    explicit SelectionModel(QObject *parent = nullptr) : QObject(parent) {}
    QSizeF min_size{8, 8};
    QRectF rect() const { return rect_; }
    void set_rect(QRectF &r);
    void start_dragging();
    void stop_dragging();
    bool is_dragging() const { return dragging_; }
    bool is_active() const { return active_ && !rect_.isNull(); }
    void activate() { active_ = true; }
    void deactivate();
    void confirm();
    bool is_confirmed() const { return confirmed_; }
    void initialize_confirmed_rect(const QRectF &rect);
    bool is_empty() const { return rect_.isNull() || rect_.isEmpty(); }
    void normalize();
signals:
    void rectChanged(QRectF rect);
    void confirmed(QRectF rect);
    void draggingChanged(bool dragging);
private:
    QRectF rect_;
    bool active_ = false, confirmed_ = false, dragging_ = false;
};
}
