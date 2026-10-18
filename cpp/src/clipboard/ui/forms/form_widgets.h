#pragma once
#include <QAbstractButton>
#include <QLabel>
#include <QList>

// Small widgets shared by the manage window's forms.
namespace jietuba::clipboard {
// A field name above its input.
QLabel *field_label(const QString &text, bool top_gap = true);
QLabel *field_hint(const QString &text);

// A checkable card with a title and a one-line description, used as a radio button in a QButtonGroup.
// With a kind (group_kind_style's KIND_*) it also has a tinted icon tile, and when checked it is outlined
// in that kind's colour.
class OptionCard : public QAbstractButton {
    Q_OBJECT
public:
    OptionCard(const QString &title, const QString &description, const QString &kind = QString(), QWidget *parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    // The width at which the title and the description both show in full.
    int required_width() const;

protected:
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    int card_height() const;
    QString title_;
    QString description_;
    QString kind_;
};

// Cards in one row when every description fits, otherwise two per row; hidden cards take no cell.
// No layout with height-for-width: inside a scroll area, the content gets wider than the viewport once the
// scroll bar appears. The height is fixed in resizeEvent for the current width; going from four columns to
// two only makes it taller, so it cannot flip back and forth.
class CardGrid : public QWidget {
    Q_OBJECT
public:
    static constexpr int WIDE_COLUMNS = 4;
    static constexpr int NARROW_COLUMNS = 2;

    explicit CardGrid(const QList<OptionCard *> &cards, QWidget *parent = nullptr);
    int columns(int width) const;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    QList<OptionCard *> shown() const;
    int required_width() const;
    int card_height() const;
    int height_for(int columns) const;
    QList<OptionCard *> cards_;
    int spacing_;
};
}
