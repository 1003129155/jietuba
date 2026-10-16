#pragma once
#include "ui/fluent_lite/frameless.h"
#include <QIcon>
#include <QLabel>

namespace jietuba {
// Caption button whose glyph geometry derives from its own size, so scaling
// cannot pull the mark off centre.
class FluentCaptionButton : public TitleBarButton {
    Q_OBJECT
public:
    enum Kind { MINIMIZE, MAXIMIZE, CLOSE };
    FluentCaptionButton(Kind kind, QWidget *parent = nullptr) : TitleBarButton(parent), kind(kind) {}
    void setMaxState(bool is_maximized);
    Kind kind;
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    bool is_maximized_ = false;
};

// Always-on-top toggle sized for the caption row; colours follow the theme
// and windows with their own palette override colors().
class TitleBarPinButton : public QAbstractButton {
    Q_OBJECT
public:
    explicit TitleBarPinButton(QWidget *parent = nullptr);
    Q_INVOKABLE void _apply_theme();
    QString icon_path;
protected:
    // (icon colour, background); checked uses the accent background like a pressed caption button.
    virtual std::pair<QColor, QColor> colors() const;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
private:
    QIcon icon_;
};

class FluentTitleBar : public TitleBar {
    Q_OBJECT
public:
    explicit FluentTitleBar(QWidget *parent);
    void setTitle(const QString &title);
    void setIcon(const QIcon &icon);
    // Icon and title centred across the whole bar instead of left-aligned.
    void center_title();
    Q_INVOKABLE void _apply_theme();
    QHBoxLayout *buttonLayout;
    QLabel *iconLabel;
    QLabel *titleLabel;
protected:
    // The single overridable source of caption colours; colours applied from
    // outside would be repainted on the next theme change.
    virtual void apply_caption_colors();
    void resizeEvent(QResizeEvent *event) override;
    void update_max_state() override;
private:
    void place_centered_title();
    bool title_centered_ = false;
};
}
