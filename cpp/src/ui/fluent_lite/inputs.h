#pragma once
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QSlider>
#include <QSpinBox>
#include <QTextEdit>

namespace jietuba {
QString input_qss(const QWidget *widget = nullptr);

class ComboBox : public QComboBox {
    Q_OBJECT
public:
    // Room for the popup arrow plus a readable entry.
    static constexpr int BASE_MIN_WIDTH = 96;
    explicit ComboBox(QWidget *parent = nullptr);
    Q_INVOKABLE void _apply_theme();
    // One input height whatever the style reports, so a combo lines up with the buttons beside it.
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    // Off for a combo too narrow for its longest entry beside the chevron: the entry is then centred over the whole box.
    void setChevronVisible(bool visible);
    bool chevronVisible() const { return chevron_; }
protected:
    // The current entry is left-aligned with a chevron on the right.
    void paintEvent(QPaintEvent *event) override;
private:
    bool chevron_ = true;
};

class LineEdit : public QLineEdit {
    Q_OBJECT
public:
    explicit LineEdit(QWidget *parent = nullptr, bool use_default_style = true);
    Q_INVOKABLE void _apply_theme();
protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
private:
    bool themed_;
};

// QTextEdit with the shared context menu; the caller owns its body style.
class TextEdit : public QTextEdit {
    Q_OBJECT
public:
    using QTextEdit::QTextEdit;
protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
};

class SpinBox : public QSpinBox {
    Q_OBJECT
public:
    explicit SpinBox(QWidget *parent = nullptr);
    Q_INVOKABLE void _apply_theme();
};

// A horizontal slider with its value beside it, for a setting card's control column.
class ValueSlider : public QWidget {
    Q_OBJECT
public:
    explicit ValueSlider(QWidget *parent = nullptr);
    int value() const { return slider_->value(); }
    void setValue(int value) { slider_->setValue(value); }
    void setRange(int minimum, int maximum);
    QSlider *slider() const { return slider_; }
    QLabel *valueLabel() const { return value_label_; }
    Q_INVOKABLE void _apply_theme();
signals:
    void valueChanged(int value);
private:
    QSlider *slider_;
    QLabel *value_label_;
};

class DoubleSpinBox : public QDoubleSpinBox {
    Q_OBJECT
public:
    explicit DoubleSpinBox(QWidget *parent = nullptr);
    Q_INVOKABLE void _apply_theme();
};
}
