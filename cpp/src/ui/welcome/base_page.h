#pragma once
#include "ui/fluent_lite/switch.h"
#include <QFrame>
#include <QLabel>
#include <QPixmap>
#include <QVBoxLayout>
#include <QWidget>
#include <optional>
#include <tuple>

// The shared page frame and visual rules of the welcome wizard.
namespace jietuba {
class ToolSettingsManager;

inline const QString PRODUCT_NAME = "Jietuba";
constexpr int WELCOME_RADIUS = 16;

QString welcome_tr(const char *text);

// The wizard's semantic colours, following the global light / dark theme.
struct WelcomeTheme {
    QString page, panel, panel_subtle, illustration, sidebar, border, border_strong, separator;
    QString text, text_muted, text_soft, accent, accent_hover, accent_soft, toolbar, canvas;
    bool is_dark;
};
WelcomeTheme welcome_theme();

// Replaces historical product names in translations with the current brand spelling.
QString brand_text(const QString &text);

// Marks a label's semantic style; BasePage refreshes it on theme changes.
QLabel *set_welcome_label_style(QLabel *label, const QString &role = "primary", int font_size = 13, int weight = 400, const QString &extra = {});
void apply_welcome_label_style(QLabel *label);

// The shared SwitchButton under its older name.
class WelcomeToggleSwitch : public SwitchButton {
    Q_OBJECT
public:
    explicit WelcomeToggleSwitch(QWidget *parent = nullptr);
signals:
    void toggled(bool checked);
};

// The illustration on the page's left. Subclasses fill it in build_content(); the base is
// a plain background.
class IllustrationArea : public QFrame {
    Q_OBJECT
public:
    explicit IllustrationArea(QWidget *parent = nullptr);
    // Called by the most derived constructor, so overrides apply.
    void init();
    virtual void apply_welcome_theme();
    // Language changed.
    virtual void retranslate() {}
    // Shows a picture in the middle.
    void set_pixmap(const QPixmap &pixmap, std::optional<QSize> max_size = std::nullopt);
protected:
    virtual void build_content() {}
    QVBoxLayout *layout_;
};

// Base of every wizard page: the header explains the step, the body shows a feature
// preview beside the settings panel. Pages only provide the illustration and controls.
class BasePage : public QWidget {
    Q_OBJECT
public:
    BasePage(const QString &title = {}, const QString &subtitle = {}, QWidget *parent = nullptr);
    // Builds the body; called by the most derived constructor, so overrides apply.
    void init();
    void apply_welcome_theme();
    // Language changed.
    virtual void retranslate() {}
    // Persists the page's settings when the wizard ends.
    virtual void save() {}
    QLabel *title_label = nullptr;
    QLabel *subtitle_label = nullptr;
    IllustrationArea *illus_area = nullptr;
protected:
    virtual IllustrationArea *create_illustration() { return new IllustrationArea(this); }
    virtual void build_controls(QVBoxLayout *) {}
    // A "text left, control right" row with an optional description: (container, label, description or null).
    static std::tuple<QWidget *, QLabel *, QLabel *> make_setting_row_with_refs(const QString &label_text, QWidget *widget, const QString &description = {});
    QVBoxLayout *content_layout = nullptr;
private:
    QVBoxLayout *root_ = nullptr;
    QFrame *content_widget_ = nullptr;
};
}
