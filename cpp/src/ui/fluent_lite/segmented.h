#pragma once
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QMap>
#include <QPushButton>
#include <QVariantAnimation>
#include <QWidget>
#include <functional>

namespace jietuba {
// Compact segmented selector addressed by route keys. The backing of the checked button is drawn
// here, under the buttons, so that on screen it can slide from the old choice to the new one.
class SegmentedWidget : public QWidget {
    Q_OBJECT
public:
    explicit SegmentedWidget(QWidget *parent = nullptr);
    void addItem(const QString &route_key, const QString &text, std::function<void()> on_click = {});
    void insertItem(int index, const QString &route_key, const QString &text, std::function<void()> on_click = {});
    void setCurrentItem(const QString &route_key);
    QString currentItem() const { return current_; }
    QString itemText(const QString &route_key) const;
    // Roomier buttons for a selector that is the main control of its view.
    void setLarge(bool large);
    // Shorter buttons and track for a selector in a title bar.
    void setCompact(bool compact, int horizontal_padding = 12);
    // Laid over other content, such as an image: an opaque backing keeps the labels readable.
    void setFloating(bool floating);
    void setIndicatorColor(const QString &light, const QString &dark = {}) { indicator_ = light; Q_UNUSED(dark); }
    Q_INVOKABLE void _apply_theme();
    // Where the checked backing is drawn now.
    QRectF thumbRect() const;
signals:
    void currentItemChanged(const QString &route_key);
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    QString style() const;
    void refresh_items();
    QMap<QString, QPushButton *> items_;
    QStringList order_;
    QString current_;
    QString indicator_;
    bool large_ = false;
    bool compact_ = false;
    int compact_padding_ = 12;
    bool floating_ = false;
    QButtonGroup *group_;
    QHBoxLayout *layout_;
    // The backing travels from here to the checked button, wherever the layout has that button by then.
    QRectF slide_from_;
    double slide_ = 1;
    QVariantAnimation slide_anim_;
};
}
