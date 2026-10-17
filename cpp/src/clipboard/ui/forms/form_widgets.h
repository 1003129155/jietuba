#pragma once
#include <QAbstractButton>
#include <QLabel>

// Small widgets shared by the manage window's forms.
namespace jietuba::clipboard {
// A field name above its input.
QLabel *field_label(const QString &text, bool top_gap = true);
QLabel *field_hint(const QString &text);

// A checkable card with a title and a one-line description, used as a radio button in a QButtonGroup.
class OptionCard : public QAbstractButton {
    Q_OBJECT
public:
    OptionCard(const QString &title, const QString &description, QWidget *parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QString title_;
    QString description_;
};
}
