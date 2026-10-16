#pragma once
#include <QPropertyAnimation>
#include <QWidget>

namespace jietuba {
// Animated on/off switch.
class SwitchButton : public QWidget {
    Q_OBJECT
    Q_PROPERTY(double progress READ progress WRITE set_progress)
public:
    explicit SwitchButton(QWidget *parent = nullptr);
    QSize sizeHint() const override { return QSize(46, 26); }
    double progress() const { return progress_; }
    void set_progress(double value);
    bool isChecked() const { return checked_; }
    void setChecked(bool checked);
    void setOnText(const QString &text) { on_text_ = text; }
    void setOffText(const QString &text) { off_text_ = text; }
signals:
    void checkedChanged(bool checked);
protected:
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
private:
    void animate();
    bool checked_ = false;
    QString on_text_, off_text_;
    double progress_ = 0.0;
    QPropertyAnimation *animation_;
};
}
