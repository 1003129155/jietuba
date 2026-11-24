#pragma once
#include "ui/base_settings_panel.h"
#include <QList>
#include <QPair>
#include <QPushButton>
#include <QVariantAnimation>
#include <QWidget>

class QLabel;

namespace jietuba {
class SegmentedToggle;
class TranslationPanelExpandButton;
class TranslationPanelDrawer;

// Below the capture toolbar while a translation is laid over the selection: the translation
// or the original picture and the language translated into. The arrow at the end slides out
// the service, and each panel opens the way the last one was left. The language list shows
// names that are not translated, so it is as wide as the longest of them whatever the UI language.
class TranslationPanel : public QWidget {
    Q_OBJECT
public:
    static constexpr int BASE_MARGIN_H = 12;
    static constexpr int BASE_MARGIN_V = 6;
    static constexpr int BASE_SPACING = 10;
    static constexpr int BASE_BTN = 28;
    // A custom model's name can be anything; longer ones are cut at this width.
    static constexpr int BASE_SERVICE_MAX_WIDTH = 160;
    static constexpr int SLIDE_MS = 160;
    explicit TranslationPanel(QWidget *parent = nullptr);
    void apply_scale();
    void retranslate();
    // Setters for the host; they do not emit.
    void set_original_shown(bool shown);
    void set_target_language(const QString &code);
    // (provider id, name) in the order to list them, with current selected.
    void set_providers(const QList<QPair<QString, QString>> &providers, const QString &current);
    bool original_shown() const;
    QString target_language() const { return language_combo->currentData().toString(); }
    QString provider() const { return provider_; }
    // Shows or folds the part behind the arrow, sliding while the panel is on screen, and
    // remembers it for the next panel.
    void set_expanded(bool expanded);
    bool expanded() const { return expanded_; }
    bool sliding() const { return slide_.state() == QAbstractAnimation::Running; }

    // Segment 0 the translation, 1 the original.
    SegmentedToggle *view_toggle = nullptr;
    QLabel *into_label = nullptr;
    PillComboBox *language_combo = nullptr;
    // Behind the arrow.
    PillComboBox *service_combo = nullptr;
    QPushButton *expand_btn = nullptr;
signals:
    void original_shown_changed(bool shown);
    void target_language_changed(const QString &code);
    void provider_changed(const QString &provider_id);
    // The width changes as the part behind the arrow slides.
    void resized();
protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
private:
    // 0 folded to 1 shown.
    void apply_open(qreal open);
    void fit_service_combo();
    TranslationPanelExpandButton *expand_button_ = nullptr;
    TranslationPanelDrawer *drawer_ = nullptr;
    QWidget *drawer_content_ = nullptr;
    QVariantAnimation slide_;
    qreal open_ = 0;
    bool expanded_ = false;
    QList<QPair<QString, QString>> providers_;
    QString provider_;
};
}
