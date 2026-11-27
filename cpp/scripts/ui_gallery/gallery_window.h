#pragma once
// Development-only design gallery: every shared control, size, colour and dialog of the application in one
// window. Texts are in Chinese on purpose; only the controls under test follow the interface language.
#include "ui/fluent_lite/frameless.h"
#include <QList>
#include <QString>

class QApplication;
class QScrollArea;
class QStackedWidget;

namespace jietuba {
class NavigationInterface;
class ToolSettingsManager;
}

namespace jietuba::gallery {
// What the toolbar switches; a window is built for one state, a change builds the next window.
struct GalleryState {
    int page = 0;
    bool dark = false;
    int scale_percent = 100;
    QString language = "zh";
    bool operator==(const GalleryState &) const = default;
};

struct GalleryPage {
    QString key;
    QString title;
};

// Theme, popup fade, language and the three scale managers, over the given (temporary) settings.
void init_gallery_runtime(QApplication &application, ToolSettingsManager *config);

class GalleryWindow : public FrostedFramelessDialog {
    Q_OBJECT
public:
    // Applies the state's theme, window scale and language to the application, then builds the window.
    explicit GalleryWindow(const GalleryState &state, QWidget *parent = nullptr);
    // Sets the application-wide parts of a state: theme mode, window scale and language.
    static void apply_state(const GalleryState &state);
    static const QList<GalleryPage> &pages();
    static QStringList language_codes();

    const GalleryState &state() const { return state_; }
    int page_count() const { return pages().size(); }
    // The scroll area of page `index`, built on first use.
    QScrollArea *page_area(int index);
    void show_page(int index);
    int current_page() const { return state_.page; }
    NavigationInterface *navigation() const { return navigation_; }

    // Modal-free builders of the dialog samples, so a test can look at them without running them.
    QDialog *make_fluent_dialog();
    QDialog *make_standard_dialog();

signals:
    void state_requested(const jietuba::gallery::GalleryState &state);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    QWidget *build_page(int index);
    void apply_window_sheet();
    QWidget *build_toolbar(QWidget *parent);
    QWidget *build_navigation(QWidget *parent);

    GalleryState state_;
    QWidget *right_area_ = nullptr;
    QStackedWidget *stack_ = nullptr;
    NavigationInterface *navigation_ = nullptr;
};
}

Q_DECLARE_METATYPE(jietuba::gallery::GalleryState)
