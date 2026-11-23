#pragma once
#include <QStringList>
#include <QWidget>

namespace jietuba {
// A pill of labelled segments, one of them chosen, which sits raised on white. Clicking another
// segment chooses it. As wide as its labels, at the toolbar and panel scale.
class SegmentedToggle : public QWidget {
    Q_OBJECT
public:
    static constexpr int BASE_HEIGHT = 28;
    // Each label's room on either side, and the pill's inset around the segments.
    static constexpr int BASE_SEGMENT_PADDING = 12;
    static constexpr int BASE_INSET = 2;
    static constexpr int BASE_FONT = 12;
    explicit SegmentedToggle(QWidget *parent = nullptr);
    // Keeps the chosen segment when there are as many.
    void set_labels(const QStringList &labels);
    QStringList labels() const { return labels_; }
    int current() const { return current_; }
    // Without current_changed.
    void set_current(int index);
    QRect segment_rect(int index) const;
    // Font and size for the current scale.
    void apply_scale();
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

signals:
    // A click chose another segment.
    void current_changed(int index);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    int segment_at(const QPoint &point) const;
    QStringList labels_;
    int current_ = 0;
    int hovered_ = -1;
};
}
