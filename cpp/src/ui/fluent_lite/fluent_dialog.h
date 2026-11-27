#pragma once
#include "ui/fluent_lite/frameless.h"
#include <QDialogButtonBox>
#include <QFrame>
#include <QList>
#include <utility>

class QHBoxLayout;
class QLabel;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace jietuba {
class BodyLabel;
class CaptionLabel;

// Paints a frameless dialog: the window background, a band across the full width in band_color with
// a hairline above and below it (a null rect paints none), and the window border.
void paint_dialog_frame(QWidget *dialog, int radius, const QRect &band, const QString &band_color);
// A muted info icon with wrapped caption text, the hint line below a dialog's cards.
QWidget *dialog_hint(QWidget *parent, const QString &text);

// An editing dialog in the settings window's look: the logo and title in the caption row, the content
// on the same lighter layer as the settings pages, and the buttons in a footer on the window background.
// Sizes follow the window scale; subclasses call finish_controls() once their content is built.
class FluentDialog : public FrostedFramelessDialog {
    Q_OBJECT
public:
    explicit FluentDialog(QWidget *parent = nullptr);
    QWidget *body() const { return body_; }
    QVBoxLayout *bodyLayout() const { return body_layout_; }
    QWidget *footer() const { return footer_; }
    // Secondary actions such as restoring defaults at the left; the buttons that close the dialog at the
    // right, all as wide as the widest of them.
    void setFooterButtons(const QList<QPushButton *> &left, const QList<QPushButton *> &right);
    // Applies the window scale to every control built so far and evens out the closing buttons.
    void finish_controls();
    // The dialog height at width that shows all of scroll's content without scrolling.
    int height_showing(QScrollArea *scroll, int width) const;
    // For a dialog opened from a topmost window.
    void setStaysOnTop();
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    QWidget *body_;
    QVBoxLayout *body_layout_;
    QWidget *footer_;
    QHBoxLayout *footer_layout_;
    QList<QPushButton *> closing_buttons_;
};

// A settings card holding any content, such as a form.
class DialogPanel : public QFrame {
    Q_OBJECT
public:
    explicit DialogPanel(QWidget *parent = nullptr);
    Q_INVOKABLE void _apply_theme();
};

// One row of a FluentDialog on a settings card: the name with an optional caption below it at the
// left, the controls at the right.
class DialogCard : public DialogPanel {
    Q_OBJECT
public:
    explicit DialogCard(const QString &title, QWidget *parent = nullptr);
    CaptionLabel *addCaption(const QString &text);
    // A caption made by the caller, such as one that elides its text.
    void setCaptionLabel(QLabel *label);
    void addControl(QWidget *control, int stretch = 0);
    BodyLabel *titleLabel;
    QVBoxLayout *textLayout;
    QHBoxLayout *controlLayout;
};

// The buttons of a message dialog, laid out in the platform's role order and sharing the row equally.
// It answers the QDialogButtonBox calls the dialog helpers use.
class DialogButtonRow : public QWidget {
    Q_OBJECT
public:
    explicit DialogButtonRow(QWidget *parent = nullptr);
    // The first accepting button is drawn as the primary one and answers Enter.
    QPushButton *addButton(const QString &text, QDialogButtonBox::ButtonRole role);
    QList<QAbstractButton *> buttons() const;
    QDialogButtonBox::ButtonRole buttonRole(QAbstractButton *button) const;
    QSize minimumSizeHint() const override;
private:
    void relayout();
    QHBoxLayout *layout_;
    QList<std::pair<QPushButton *, QDialogButtonBox::ButtonRole>> buttons_;
};
}
