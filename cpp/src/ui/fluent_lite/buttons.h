#pragma once
#include "ui/fluent_lite/control_style.h"
#include "ui/fluent_lite/theme.h"
#include <QPushButton>
#include <QVariantAnimation>
#include <QToolButton>

namespace jietuba {
// Native Qt buttons styled as one family. Icon size is kept as a base value
// that _apply_theme rescales, because the style is regenerated on construction,
// theme changes and dialog scaling; a plain setIconSize() would not survive.
//
// The four plain kinds are drawn by fluent_control_style() as their style sheets would draw
// them. A menu, another sheet's rule for the button or a subclass with its own sheet switches
// it to the sheet for good.
class PushButton : public QPushButton {
    Q_OBJECT
public:
    explicit PushButton(const QString &text = {}, QWidget *parent = nullptr, const IconSource &icon = {});
    explicit PushButton(QWidget *parent) : PushButton(QString(), parent) {}
    void setIcon(const IconSource &icon);
    void setBaseIconSize(int base);
    Q_INVOKABLE void _apply_theme();
    bool paints_itself() const { return painted_; }
    ControlBox control_box(QStyle::State state) const;
    QColor text_color(QStyle::State state) const;
    // What is drawn for the state: its colours, or on the way to them from those of the state before
    // while the pointer has just come, gone or pressed.
    ControlBox shown_box(QStyle::State state) const;
    QColor shown_text(QStyle::State state) const;
    int horizontal_padding() const;
protected:
    enum class Kind { Standard, Primary, Transparent, Hyperlink };
    bool event(QEvent *event) override;
    virtual QString style_sheet() const;
    void init_theme();
    void set_kind(Kind kind);
    int base_icon_size_ = 16;
    IconSource theme_icon_source_;
private:
    bool wants_painting() const;
    void use_style_sheet();
    // Hover, pressed and enabled, as the colours depend on them.
    QStyle::State pointer_state() const;
    Kind kind_ = Kind::Standard;
    bool painted_ = false;
    bool sheet_chosen_ = false;
    QStyle::State shown_state_ = QStyle::State_Enabled;
    ControlBox fade_from_box_;
    QColor fade_from_text_;
    double fade_ = 1;
    QVariantAnimation fade_anim_;
};

// A button whose fill is its content (theme colour, mask colour, palettes).
// Its frame follows the input metrics, since it sits in columns of combo boxes.
class ColorSwatchButton : public PushButton {
    Q_OBJECT
public:
    explicit ColorSwatchButton(QWidget *parent = nullptr);
    // A CSS background value: rgb()/#hex or qlineargradient(...).
    void setFill(const QString &fill);
    void setSelected(bool selected);
protected:
    QString style_sheet() const override;
private:
    QString fill_ = "transparent";
    bool selected_ = false;
};

class PrimaryPushButton : public PushButton {
    Q_OBJECT
public:
    explicit PrimaryPushButton(const QString &text = {}, QWidget *parent = nullptr, const IconSource &icon = {});
    explicit PrimaryPushButton(QWidget *parent) : PrimaryPushButton(QString(), parent) {}
protected:
    QString style_sheet() const override;
};

class TransparentPushButton : public PushButton {
    Q_OBJECT
public:
    explicit TransparentPushButton(const QString &text = {}, QWidget *parent = nullptr, const IconSource &icon = {});
    explicit TransparentPushButton(QWidget *parent) : TransparentPushButton(QString(), parent) {}
protected:
    QString style_sheet() const override;
};

class HyperlinkButton : public TransparentPushButton {
    Q_OBJECT
public:
    explicit HyperlinkButton(const QString &url = {}, const QString &text = {}, QWidget *parent = nullptr, const IconSource &icon = {});
    void setUrl(const QString &url) { url_ = url; }
protected:
    QString style_sheet() const override;
private:
    QString url_;
};

class TransparentToolButton : public QToolButton {
    Q_OBJECT
public:
    explicit TransparentToolButton(const IconSource &icon = {}, QWidget *parent = nullptr);
    explicit TransparentToolButton(QWidget *parent) : TransparentToolButton(IconSource{}, parent) {}
    // Both base sizes at once, so callers restyle only once.
    void setBaseMetrics(int side, int icon_size);
    void setBaseIconSize(int base);
    void setIcon(const IconSource &icon);
    Q_INVOKABLE void _apply_theme();
private:
    int base_side_ = 30;
    int base_icon_size_ = 16;
    IconSource theme_icon_source_;
};
}
