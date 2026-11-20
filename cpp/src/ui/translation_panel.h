#pragma once
#include "ui/base_settings_panel.h"
#include <QList>
#include <QPair>
#include <QPushButton>
#include <QWidget>

class QLabel;
class QMenu;

namespace jietuba {
class SegmentedToggle;

// Below the capture toolbar while a translation is laid over the selection: the translation
// or the original picture, the language translated into, and behind "..." the service. The
// language list shows names that are not translated, so it is as wide as the longest of them
// whatever the UI language.
class TranslationPanel : public QWidget {
    Q_OBJECT
public:
    static constexpr int BASE_MARGIN_H = 12;
    static constexpr int BASE_MARGIN_V = 6;
    static constexpr int BASE_SPACING = 10;
    static constexpr int BASE_BTN = 28;
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

    // Segment 0 the translation, 1 the original.
    SegmentedToggle *view_toggle = nullptr;
    QLabel *into_label = nullptr;
    PillComboBox *language_combo = nullptr;
    QPushButton *more_btn = nullptr;
    QMenu *more_menu = nullptr;
signals:
    void original_shown_changed(bool shown);
    void target_language_changed(const QString &code);
    void provider_changed(const QString &provider_id);
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    void rebuild_menu();
    QList<QPair<QString, QString>> providers_;
    QString provider_;
};
}
