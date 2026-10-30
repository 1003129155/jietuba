#pragma once
#include "ui/fluent_lite/cards.h"
#include "ui/fluent_lite/switch.h"
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

// Shared widgets of the settings window.
namespace jietuba {
class SettingsDialog;

// The outermost scroll area of a settings page, created under the page stack at once:
// content built in a detached scroll area and attached later would be styled twice.
// The vertical scroll bar is always on, so cards keep their width when switching pages;
// the dialog style sheet hides the handle when everything fits.
QScrollArea *page_scroll_area(SettingsDialog *dialog);
// Settings pages scroll with the pointer often over a combo or spin box; the wheel must not change their values.
void disable_wheel_on_value_controls(QWidget *root);

// One of two values for the effective application theme.
QString theme_color(const QString &light, const QString &dark);
QString theme_surface_color();
QString theme_border_color();
QString theme_input_background();
QString theme_popup_background();
QString theme_popup_hover_background();
QString theme_text_style(int font_size = 14, bool bold = false, const QString &extra = {});
QString theme_caption_style(int font_size = 13, const QString &extra = {});
// Applies a semantic text style and registers it for refresh on theme changes.
void apply_theme_text_style(QWidget *widget, int font_size = 14, bool bold = false, const QString &extra = {}, bool caption = false);
// Re-applies the registered text styles below a top-level widget.
void refresh_theme_widget_styles(QWidget *root);
QString theme_menu_style();

// SettingCardGroup with a height that follows the visible cards.
class SettingsCardGroup : public SettingCardGroup {
    Q_OBJECT
public:
    explicit SettingsCardGroup(const QString &title, QWidget *parent = nullptr, const QString &tone = "neutral");
    // Recomputes the height from the visible cards. Computing it once in showEvent is not
    // enough: after cards are shown or hidden (another provider's credentials on the
    // translation page), a stale height leaves a gap at the top and cuts off cards below.
    void refreshHeight();
protected:
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
};

// Sizes a button for its current text and icon.
void adjust_button_width(QPushButton *button, int min_width = 0, int horizontal_padding = 28);

// A custom-layout row inside a group, as tall as its content. No background or border:
// the group paints them, and another layer would show a nested frame in dark mode.
class WhiteCard : public QFrame {
    Q_OBJECT
public:
    explicit WhiteCard(QWidget *parent = nullptr);
    QSize minimumSizeHint() const override;
};

// SwitchButton with a toggled signal, for the setup wizard.
class ToggleSwitch : public SwitchButton {
    Q_OBJECT
public:
    explicit ToggleSwitch(QWidget *parent = nullptr);
signals:
    void toggled(bool checked);
};

// A settings section with its title inside the card: icon, title and a short caption,
// then rows. A plain layout, so the height follows the content.
class SectionCard : public QFrame {
    Q_OBJECT
public:
    SectionCard(const IconSource &icon, const QString &title, const QString &caption = {}, QWidget *parent = nullptr);
    // A row with a separator above it.
    void addRow(QWidget *widget);
    // Content without a separator (tab switchers and the like).
    void addWidget(QWidget *widget);
    QVBoxLayout *bodyLayout;
    QLabel *titleLabel;
    QLabel *captionLabel = nullptr;
protected:
    void paintEvent(QPaintEvent *event) override;
};

// Adds a row to a vertical layout with a theme-coloured separator above it.
void add_separated_row(QVBoxLayout *layout, QWidget *widget);
// A file name under svg/ as a full path; FluentIcon passes through.
IconSource icon_ref(const IconSource &ref);
// A settings row title; parent is the row, found by settings search through it.
QLabel *row_label(QWidget *parent, const QString &text);
}
