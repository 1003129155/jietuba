#include "ui/translation_panel.h"
#include "core/i18n.h"
#include "core/ui_scale.h"
#include "translation/languages.h"
#include "ui/segmented_toggle.h"
#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QSignalBlocker>
#include <algorithm>

namespace jietuba {
namespace {
QString panel_tr(const char *text) { return i18n_tr(text, "TranslationPanel"); }

// Three dots over the panel's button background: the icon set has no "more".
class MoreButton : public QPushButton {
public:
    using QPushButton::QPushButton;

protected:
    void paintEvent(QPaintEvent *event) override {
        QPushButton::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(CONTROL_INK);
        for (int dot = -1; dot <= 1; ++dot) painter.drawEllipse(QPointF(width() / 2.0 + dot * scaled_f(5), height() / 2.0), scaled_f(1.6), scaled_f(1.6));
    }
};
}

TranslationPanel::TranslationPanel(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_TranslucentBackground);
    auto *layout = new QHBoxLayout(this);
    view_toggle = new SegmentedToggle;
    layout->addWidget(view_toggle);
    layout->addWidget(make_separator());
    into_label = new QLabel;
    layout->addWidget(into_label);
    language_combo = new PillComboBox;
    for (const auto &[code, name] : translation_languages()) language_combo->addItem(name, code);
    language_combo->setMaxVisibleItems(12);
    layout->addWidget(language_combo);
    more_btn = new MoreButton;
    more_btn->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(more_btn);
    more_menu = new QMenu(this);
    connect(view_toggle, &SegmentedToggle::current_changed, this, [this](int index) { emit original_shown_changed(index == 1); });
    connect(language_combo, &QComboBox::currentIndexChanged, this, [this](int) { emit target_language_changed(target_language()); });
    connect(more_btn, &QPushButton::clicked, this, [this] { more_menu->popup(more_btn->mapToGlobal(QPoint(0, more_btn->height() + scaled(4)))); });
    retranslate();
    apply_scale();
}

void TranslationPanel::paintEvent(QPaintEvent *) { paint_rounded_panel(this); }

void TranslationPanel::apply_scale() {
    setStyleSheet(build_settings_panel_stylesheet() + combo_popup_qss());
    const int mh = scaled(BASE_MARGIN_H), mv = scaled(BASE_MARGIN_V);
    layout()->setContentsMargins(mh, mv, mh, mv);
    layout()->setSpacing(scaled(BASE_SPACING));
    view_toggle->apply_scale();
    into_label->setStyleSheet(QString("color: %1; font-size: %2px;").arg(CONTROL_SUB.name()).arg(scaled(12)));
    // The panel font, like the toggle's, so the chosen language grows with the panel too.
    QFont language_font = language_combo->font();
    language_font.setFamilies({"Microsoft YaHei", "SimSun", "Arial"});
    language_font.setPixelSize(scaled(12));
    language_combo->setFont(language_font);
    // As wide as the longest language name, which reads the same in every UI language.
    int longest = 0;
    for (const auto &[code, name] : translation_languages()) longest = std::max(longest, language_combo->fontMetrics().horizontalAdvance(name));
    const int language_width = longest + scaled(PillComboBox::BASE_PADDING_LEFT) + scaled(PillComboBox::BASE_ARROW_COLUMN) + scaled(2);
    language_combo->setFixedWidth(language_width);
    language_combo->view()->setMinimumWidth(language_width);
    more_btn->setFixedSize(scaled(BASE_BTN), scaled(BASE_BTN));
    more_menu->setStyleSheet(panel_menu_qss());
    adjustSize();
    update();
}

void TranslationPanel::retranslate() {
    view_toggle->set_labels({panel_tr("Translated"), panel_tr("Original")});
    view_toggle->setToolTip(panel_tr("Show original image"));
    into_label->setText(panel_tr("To"));
    language_combo->setToolTip(panel_tr("Target language"));
    more_btn->setToolTip(panel_tr("More"));
    rebuild_menu();
    adjustSize();
}

void TranslationPanel::rebuild_menu() {
    more_menu->clear();
    more_menu->addAction(panel_tr("Translation service"))->setEnabled(false);
    for (const auto &[id, name] : providers_) {
        auto *action = more_menu->addAction(name);
        action->setCheckable(true);
        action->setChecked(id == provider_);
        connect(action, &QAction::triggered, this, [this, id = id] {
            if (id == provider_) {
                rebuild_menu();
                return;
            }
            provider_ = id;
            rebuild_menu();
            emit provider_changed(id);
        });
    }
}

void TranslationPanel::set_original_shown(bool shown) { view_toggle->set_current(shown ? 1 : 0); }

bool TranslationPanel::original_shown() const { return view_toggle->current() == 1; }

void TranslationPanel::set_target_language(const QString &code) {
    const QSignalBlocker blocker(language_combo);
    const int index = language_combo->findData(code.toUpper());
    if (index >= 0) language_combo->setCurrentIndex(index);
}

void TranslationPanel::set_providers(const QList<QPair<QString, QString>> &providers, const QString &current) {
    providers_ = providers;
    provider_ = std::any_of(providers.cbegin(), providers.cend(), [&](const QPair<QString, QString> &entry) { return entry.first == current; }) ? current : QString();
    rebuild_menu();
}
}
