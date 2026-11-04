#pragma once
#include "ui/fluent_lite/frameless.h"
#include <QFrame>
#include <QLabel>
#include <vector>

class QStackedWidget;
class QPushButton;

// The welcome wizard: product setup in steps.
namespace jietuba {
class BasePage;
class ToolSettingsManager;

// One segment of the progress bar over the settings; clicking it goes to that page.
class StepItem : public QWidget {
    Q_OBJECT
public:
    StepItem(int index, QWidget *parent = nullptr);
    // The page title, as tooltip and accessible name.
    void set_text(const QString &text);
    void set_state(bool active, bool completed);
    void apply_welcome_theme() { update(); }
signals:
    void clicked(int index);
protected:
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
private:
    int index_;
    bool hovered_ = false;
    bool active_ = false;
    bool completed_ = false;
};

class WelcomeWizard : public FrostedFramelessDialog {
    Q_OBJECT
public:
    static constexpr int WINDOW_W = 1040;
    static constexpr int WINDOW_H = 620;
    // The demo stage's column, left of the settings.
    static constexpr int STAGE_W = 440;
    static constexpr int PAGE_COUNT = 5;
    explicit WelcomeWizard(ToolSettingsManager *config, QWidget *parent = nullptr);
    // Refreshes every text after a language switch (navigation and pages).
    void retranslate_ui();
    int page_count() const { return static_cast<int>(pages_.size()); }
    int current_page() const { return current_; }
    void go_to_page(int index);
protected:
    // Finishing, Skip and the close button all save every page and mark the wizard as run.
    void closeEvent(QCloseEvent *event) override;
private:
    void setup_titlebar();
    // Detects the system language (or the saved one) and loads it before any page exists.
    void init_language();
    void build_ui();
    void apply_welcome_theme();
    void build_pages();
    // Repolishes controls built before they had a parent.
    void refresh_theme_scope();
    void go_next();
    void go_back();
    void update_nav();
    // The progress segments carry the pages' titles in the current language.
    void refresh_step_labels();
    // Saves every page once and marks the wizard as run.
    void save_all();
    void finish();

    ToolSettingsManager *config_;
    int current_ = 0;
    bool saved_ = false;
    QFrame *shell_ = nullptr;
    QWidget *right_ = nullptr;
    QLabel *page_count_ = nullptr;
    std::vector<StepItem *> step_items_;
    std::vector<BasePage *> pages_;
    // The pages' stages and the pages, switched together.
    QStackedWidget *stage_ = nullptr;
    QStackedWidget *stack_ = nullptr;
    QPushButton *btn_skip_ = nullptr, *btn_back_ = nullptr, *btn_next_ = nullptr;
};
}
