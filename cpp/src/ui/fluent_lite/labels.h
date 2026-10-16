#pragma once
#include <QLabel>

namespace jietuba {
class BodyLabel : public QLabel {
    Q_OBJECT
public:
    explicit BodyLabel(const QString &text = {}, QWidget *parent = nullptr);
    explicit BodyLabel(QWidget *parent) : BodyLabel(QString(), parent) {}
    Q_INVOKABLE void _apply_theme();
};

class CaptionLabel : public QLabel {
    Q_OBJECT
public:
    explicit CaptionLabel(const QString &text = {}, QWidget *parent = nullptr);
    explicit CaptionLabel(QWidget *parent) : CaptionLabel(QString(), parent) {}
    Q_INVOKABLE void _apply_theme();
};
}
