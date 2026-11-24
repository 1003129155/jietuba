#pragma once
#include "ui/fluent_lite/theme.h"
#include <QAbstractButton>
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
// Left edge of card titles; nested rows align to it.
int card_text_left();
// Room an expander's arrow column takes at the right end of its header; nested rows keep clear of it.
int expander_gutter();
// A setting row's width besides its title: icon, gaps, margins and the widest control column, with the arrow column of a folded row.
int card_width_besides_title();

// The icon of a setting row: the bare icon in the text colour, or a tinted square when a tone is given.
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
    static constexpr int CONTROL_COLUMN_MAX = 204;
    // The tone is kept for callers; cards draw their icon in the text colour.
    SettingCard(const IconSource &icon, const QString &title, const QString &content = {}, QWidget *parent = nullptr, const QString &tone = {});
    // Controls fill the column by default; fixed-width ones pick an end with align.
    void addControl(QWidget *widget, std::optional<Qt::Alignment> align = std::nullopt);
    int controlColumnHint() const;
    void setControlColumnWidth(int width);
    void setTone(const QString &tone);
    // A row inside an expander: no card of its own, and it keeps clear of the arrow column.
    void setEmbedded(bool embedded);
    // The header of an expanded expander has square lower corners.
    void setJoinedBelow(bool joined);
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
    bool embedded_ = false;
    bool joined_below_ = false;
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

// A child switch under the previous card: no icon, text aligned with that card's title.
// The group folds it into an expander on that card.
class SubSwitchSettingCard : public SwitchSettingCard {
    Q_OBJECT
public:
    explicit SubSwitchSettingCard(const QString &title, const QString &content = {}, QWidget *parent = nullptr);
};

// A custom-layout row that can paint the card background; groups switch it on for rows they own.
class CardSurface : public QFrame {
    Q_OBJECT
public:
    explicit CardSurface(QWidget *parent = nullptr);
    void setChrome(bool chrome);
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    bool chrome_ = false;
};

// A card with rows folded under it: the header carries an arrow that shows or hides the rows.
class SettingExpander : public QWidget {
    Q_OBJECT
public:
    SettingExpander(SettingCard *header, QWidget *parent = nullptr);
    SettingCard *header() const { return header_; }
    void addItem(SettingCard *item);
    // Called after an item was shown or hidden: separators and the body follow the visible items.
    void refreshItems();
    bool isExpanded() const { return expanded_; }
    void setExpanded(bool expanded);
    QAbstractButton *arrow() const { return arrow_; }
    Q_INVOKABLE void _apply_theme();
signals:
    // The expander's height changed.
    void resized();
private:
    void sync_header();
    SettingCard *header_;
    QAbstractButton *arrow_;
    QFrame *body_;
    QVBoxLayout *body_layout_;
    QList<SettingCard *> items_;
    QList<QFrame *> separators_;
    bool expanded_ = true;
};

class SettingCardGroup : public QWidget {
    Q_OBJECT
public:
    explicit SettingCardGroup(const QString &title, QWidget *parent = nullptr, const QString &tone = "neutral");
    // A SubSwitchSettingCard joins the card before it as an expander; any other card stands alone.
    void addSettingCard(QWidget *card);
    void addSettingCards(const QList<QWidget *> &cards);
    // Shows or hides a card; an expander's header takes its rows along.
    void setCardVisible(QWidget *card, bool visible);
    void adjustSize();
    // Recomputes the height after the cards changed; derived groups pin it to their content.
    virtual void refreshHeight();
    QSize minimumSizeHint() const override;
    Q_INVOKABLE void _apply_theme();
    QVBoxLayout *vBoxLayout;
    QLabel *titleLabel;
    ExpandLayout *cardLayout;
protected:
    void showEvent(QShowEvent *event) override;
    // Per group, not per page: a page-wide width would stretch short controls.
    void sync_control_column();
    // The title, its spacing and the margins: everything but the cards.
    int header_height() const;
private:
    QString tone_;
    QWidget *card_container_;
    QList<QWidget *> cards_;
    // The last card added on its own: the header a following switch row folds under.
    QWidget *last_top_ = nullptr;
    QHash<QWidget *, SettingExpander *> expanders_;
    QHash<QWidget *, SettingExpander *> hosts_;
};

class SimpleCardWidget : public QFrame {
    Q_OBJECT
public:
    explicit SimpleCardWidget(QWidget *parent = nullptr);
    Q_INVOKABLE virtual void _apply_theme();
};
}
