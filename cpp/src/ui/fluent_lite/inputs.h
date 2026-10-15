#pragma once
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
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
protected:
    // The current entry is centred in the whole box: settings pages make every
    // combo as wide as the widest one, and short entries would hug the left edge.
    void paintEvent(QPaintEvent *event) override;
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

class DoubleSpinBox : public QDoubleSpinBox {
    Q_OBJECT
public:
    explicit DoubleSpinBox(QWidget *parent = nullptr);
    Q_INVOKABLE void _apply_theme();
};
}
