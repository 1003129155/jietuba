#pragma once
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QMap>
#include <QPushButton>
#include <QWidget>
#include <functional>

namespace jietuba {
// Compact segmented selector addressed by route keys.
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
    // Laid over other content, such as an image: an opaque backing keeps the labels readable.
    void setFloating(bool floating);
    void setIndicatorColor(const QString &light, const QString &dark = {}) { indicator_ = light; Q_UNUSED(dark); }
    Q_INVOKABLE void _apply_theme();
signals:
    void currentItemChanged(const QString &route_key);
private:
    QString style() const;
    void refresh_items();
    QMap<QString, QPushButton *> items_;
    QStringList order_;
    QString current_;
    QString indicator_;
    bool large_ = false;
    bool floating_ = false;
    QButtonGroup *group_;
    QHBoxLayout *layout_;
};
}
