#pragma once
#include <QAbstractButton>
#include <QPropertyAnimation>
#include <QWidget>

namespace jietuba {
// Animated on/off switch; 46x26 at 100%, following the window scale.
class SwitchButton : public QWidget {
    Q_OBJECT
    Q_PROPERTY(double progress READ progress WRITE set_progress)
public:
    explicit SwitchButton(QWidget *parent = nullptr);
    QSize sizeHint() const override { return size(); }
    // Called by configure_dialog_controls once the window scale is known.
    Q_INVOKABLE void _apply_theme();
    double progress() const { return progress_; }
    void set_progress(double value);
    bool isChecked() const { return checked_; }
    void setChecked(bool checked);
    void setOnText(const QString &text) { on_text_ = text; }
    void setOffText(const QString &text) { off_text_ = text; }
    void setCompact(bool compact);
signals:
    void checkedChanged(bool checked);
protected:
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
private:
    void animate();
    bool checked_ = false;
    bool compact_ = false;
    QString on_text_, off_text_;
    double progress_ = 0.0;
    QPropertyAnimation *animation_;
};

// A single keyboard and pointer target, with its label before a compact switch.
class LabeledSwitch : public QAbstractButton {
    Q_OBJECT
public:
    explicit LabeledSwitch(const QString &text, QWidget *parent = nullptr);
    QSize sizeHint() const override;
    Q_INVOKABLE void _apply_theme();
protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
private:
    SwitchButton *switch_;
};
}
