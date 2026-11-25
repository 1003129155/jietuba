#pragma once
#include <QFrame>
#include <QVariantAnimation>
#include <functional>

namespace jietuba {
// Folds the widget it holds away in two steps, so that widget is never seen cut off. Opening, the box
// grows to the widget's height and the widget then fades in; closing, the widget fades out and the box
// then shrinks. The widget keeps its place and its full size inside the box all the while.
//
// Closed, the box is hidden. It opens and closes at once while it is not on screen or animations are off,
// and when asked to: something about to be scrolled to must have its final place already.
class FoldBox : public QFrame {
    Q_OBJECT
public:
    explicit FoldBox(QWidget *parent = nullptr);
    // What is folded; callers give it a layout and children.
    QWidget *content() const { return content_; }
    bool isOpen() const { return open_; }
    void setOpen(bool open, bool animated = true);
    // The fade is a cover in the colour behind the content, its lower corners rounded by `radius`.
    void setBackdrop(std::function<QColor()> color, int radius = 0);
    // Takes the content's height again after its rows changed.
    void refit();
    // How much of the content the cover hides, 0 to 1.
    qreal cover() const { return cover_; }
    bool settled() const { return phase_ == Phase::Idle; }
signals:
    // The box's height changed, or it was shown or hidden.
    void resized();
protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
private:
    enum class Phase { Idle, Growing, FadingIn, FadingOut, Shrinking };
    class Veil;
    int natural_height() const;
    // True when the height changed, which resized() has then reported.
    bool set_shown(int height);
    void set_cover(qreal cover);
    // Lays out every ancestor now, innermost first.
    void settle();
    void place();
    void finish();
    void next_phase();
    QWidget *content_;
    Veil *veil_;
    std::function<QColor()> backdrop_;
    int radius_ = 0;
    bool open_ = false;
    int shown_ = 0;
    qreal cover_ = 0;
    Phase phase_ = Phase::Idle;
    QVariantAnimation height_anim_, cover_anim_;
};
}
