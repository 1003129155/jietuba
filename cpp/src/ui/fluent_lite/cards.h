#pragma once
#include "ui/fluent_lite/theme.h"
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QVBoxLayout>
#include <optional>

namespace jietuba {
class BodyLabel;
class CaptionLabel;
class SwitchButton;

// Each global feature has a fixed muted hue; other rows use neutral.
// ((light background, light icon), (dark background, dark icon)).
struct ToneColors { QString light_bg, light_fg, dark_bg, dark_fg; };
const ToneColors &feature_tone(const QString &tone);

// Right inner margin of cards; custom card layouts use it so control edges line up.
int card_right_margin();
// Left edge of card titles; separators and nested rows align to it.
int card_text_left();

// The icon tile of a setting row: a tinted square for a tone, or the bare icon when tone is empty.
class IconBadge : public QWidget {
    Q_OBJECT
public:
    IconBadge(const IconSource &icon, const QString &tone = "neutral", QWidget *parent = nullptr, int size = 36, int icon_size = 20, bool muted = true);
    QString tone() const { return tone_; }
    void setTone(const QString &tone);
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    IconSource icon_;
    QString tone_;
    int icon_size_;
    bool muted_;
};

// heightForWidth that asks word-wrapped cards for their height at the real width.
class ExpandLayout : public QVBoxLayout {
public:
    using QVBoxLayout::QVBoxLayout;
    int heightForWidth(int width) const override;
};

// One setting row: icon and text on the left, a control column on the right.
// The column width is unified per group (SettingCardGroup), so control edges line up.
class SettingCard : public QFrame {
    Q_OBJECT
public:
    // Keeps one long option from squeezing out the description.
    static constexpr int CONTROL_COLUMN_MAX = 200;
    // An empty tone follows the owning group.
    SettingCard(const IconSource &icon, const QString &title, const QString &content = {}, QWidget *parent = nullptr, const QString &tone = {});
    // Controls fill the column by default; fixed-width ones pick an end with align.
    void addControl(QWidget *widget, std::optional<Qt::Alignment> align = std::nullopt);
    int controlColumnHint() const;
    void setControlColumnWidth(int width);
    void setTone(const QString &tone);
    void setTitle(const QString &title);
    void setContent(const QString &content);
    QSize minimumSizeHint() const override;
    Q_INVOKABLE void _apply_theme();
    bool has_own_tone() const { return !own_tone_.isEmpty(); }
    QHBoxLayout *hBoxLayout;
    IconBadge *iconBadge;
    BodyLabel *titleLabel;
    CaptionLabel *contentLabel;
    QWidget *controlContainer;
    QHBoxLayout *controlLayout;
private:
    QString own_tone_;
    QVBoxLayout *text_layout_;
};

class SwitchSettingCard : public SettingCard {
    Q_OBJECT
public:
    SwitchSettingCard(const IconSource &icon, const QString &title, const QString &content = {}, QWidget *parent = nullptr, const QString &tone = {});
    void setChecked(bool checked);
    bool isChecked() const;
    SwitchButton *switchButton;
signals:
    void checkedChanged(bool checked);
};

// A child switch under the previous card: no icon tile, text aligned with that card's title.
class SubSwitchSettingCard : public SwitchSettingCard {
    Q_OBJECT
public:
    explicit SubSwitchSettingCard(const QString &title, const QString &content = {}, QWidget *parent = nullptr);
};

class SettingCardGroup : public QWidget {
    Q_OBJECT
public:
    explicit SettingCardGroup(const QString &title, QWidget *parent = nullptr, const QString &tone = "neutral");
    // with_separator=false for card containers that draw their own separators.
    void addSettingCard(QWidget *card, bool with_separator = true);
    void addSettingCards(const QList<QWidget *> &cards);
    // Shows or hides a card together with the separator before it.
    void setCardVisible(QWidget *card, bool visible);
    void adjustSize();
    QSize minimumSizeHint() const override;
    Q_INVOKABLE void _apply_theme();
    QVBoxLayout *vBoxLayout;
    QLabel *titleLabel;
    ExpandLayout *cardLayout;
protected:
    void showEvent(QShowEvent *event) override;
    // Per group, not per page: a page-wide width would stretch short controls.
    void sync_control_column();
private:
    QString tone_;
    QFrame *card_container_;
    QList<QWidget *> cards_;
    QList<QFrame *> separators_;
    QHash<QWidget *, QFrame *> leading_separators_;
};

class SimpleCardWidget : public QFrame {
    Q_OBJECT
public:
    explicit SimpleCardWidget(QWidget *parent = nullptr);
    Q_INVOKABLE virtual void _apply_theme();
};
}
