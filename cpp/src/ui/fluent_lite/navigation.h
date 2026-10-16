#pragma once
#include "ui/fluent_lite/theme.h"
#include <QButtonGroup>
#include <QMap>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>
#include <functional>

namespace jietuba {
enum class NavigationItemPosition { TOP = 0, BOTTOM = 1 };

// Route-based sidebar navigation.
class NavigationInterface : public QWidget {
    Q_OBJECT
public:
    explicit NavigationInterface(QWidget *parent = nullptr);
    // A group separator at the current end of the top list.
    void addSeparator();
    QPushButton *addItem(const QString &route_key, const IconSource &icon, const QString &text, std::function<void()> on_click,
                         NavigationItemPosition position = NavigationItemPosition::TOP, const QString &tooltip = {}, const QString &tone = {});
    void setCurrentItem(const QString &route_key);
    void clearCurrentItem();
    void setExpandWidth(int width) { expand_width_ = width; }
    void setMinimumExpandWidth(int width) { minimum_expand_width_ = width; }
    void expand() { if (expand_width_) setMinimumWidth(expand_width_); }
    QPushButton *widget(const QString &route_key) const { return items_.value(route_key); }
    Q_INVOKABLE void _apply_theme();
private:
    void style_button(QPushButton *button);
    // The checked item's icon takes the accent with its text; others use their feature hue.
    void apply_icon(const QString &route_key);
    QMap<QString, QPushButton *> items_;
    QMap<QString, IconSource> icons_;
    QMap<QString, QString> tones_;
    QString current_;
    QButtonGroup *group_;
    QVBoxLayout *layout_, *top_, *bottom_;
    int expand_width_ = 0, minimum_expand_width_ = 0;
};
}
