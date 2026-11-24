#include "ui/translation_panel.h"
#include "core/i18n.h"
#include "core/ui_scale.h"
#include "settings/tool_settings.h"
#include "translation/languages.h"
#include "ui/segmented_toggle.h"
#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPolygonF>
#include <QSignalBlocker>
#include <algorithm>

namespace jietuba {
namespace {
QString panel_tr(const char *text) { return i18n_tr(text, "TranslationPanel"); }

void fit_pill_combo(PillComboBox *combo, const QStringList &names, int max_width) {
    // The panel font, like the toggle's, so the chosen entry grows with the panel too.
    QFont font = combo->font();
    font.setFamilies({"Microsoft YaHei", "SimSun", "Arial"});
    font.setPixelSize(scaled(12));
    combo->setFont(font);
    int longest = 0;
    for (const auto &name : names) longest = std::max(longest, combo->fontMetrics().horizontalAdvance(name));
    const int width = std::min(longest + scaled(PillComboBox::BASE_PADDING_LEFT) + scaled(PillComboBox::BASE_ARROW_COLUMN) + scaled(2), max_width);
    combo->setFixedWidth(width);
    combo->view()->setMinimumWidth(width);
}
}

// A chevron over the panel's button background: it points to where the folded part comes
// out and turns round as that part slides out.
class TranslationPanelExpandButton : public QPushButton {
public:
    using QPushButton::QPushButton;
    void set_turn(qreal turn) {
        turn_ = turn;
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        QPushButton::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.translate(QRectF(rect()).center());
        painter.rotate(180 * turn_);
        painter.setPen(QPen(CONTROL_INK, scaled_f(1.5), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const qreal half_width = scaled_f(2.25), half_height = scaled_f(4.5);
        painter.drawPolyline(QPolygonF{QPointF(-half_width, -half_height), QPointF(half_width, 0), QPointF(-half_width, half_height)});
    }

private:
    qreal turn_ = 0;
};

// Shows the right part of its content, all of it when fully open. The content keeps its own
// size and is clipped, so it comes out from behind the arrow next to it.
class TranslationPanelDrawer : public QWidget {
public:
    TranslationPanelDrawer(QWidget *content, QWidget *parent) : QWidget(parent), content_(content) {
        content_->setParent(this);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    }
    void set_open(qreal open) {
        open_ = open;
        updateGeometry();
        place();
    }
    QSize sizeHint() const override { return QSize(qRound(open_ * full_width()), content_->sizeHint().height()); }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void resizeEvent(QResizeEvent *) override { place(); }

private:
    int full_width() const { return content_->sizeHint().width(); }
    void place() { content_->setGeometry(width() - full_width(), 0, full_width(), height()); }
    QWidget *content_;
    qreal open_ = 0;
};

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
    // The folded part and the arrow touch, so the gap before the arrow belongs to the part and
    // the arrow stays put until the part starts to come out.
    drawer_content_ = new QWidget;
    auto *drawer_layout = new QHBoxLayout(drawer_content_);
    drawer_layout->addWidget(make_separator());
    service_combo = new PillComboBox;
    service_combo->setMaxVisibleItems(12);
    drawer_layout->addWidget(service_combo);
    drawer_ = new TranslationPanelDrawer(drawer_content_, this);
    expand_button_ = new TranslationPanelExpandButton;
    expand_btn = expand_button_;
    expand_btn->setFocusPolicy(Qt::NoFocus);
    auto *tail = new QHBoxLayout;
    tail->setContentsMargins(0, 0, 0, 0);
    tail->setSpacing(0);
    tail->addWidget(drawer_);
    tail->addWidget(expand_btn);
    layout->addLayout(tail);
    slide_.setDuration(SLIDE_MS);
    slide_.setEasingCurve(QEasingCurve::OutCubic);
    connect(&slide_, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) { apply_open(value.toReal()); });
    connect(view_toggle, &SegmentedToggle::current_changed, this, [this](int index) { emit original_shown_changed(index == 1); });
    connect(language_combo, &QComboBox::currentIndexChanged, this, [this](int) { emit target_language_changed(target_language()); });
    connect(service_combo, &QComboBox::currentIndexChanged, this, [this](int) {
        const QString id = service_combo->currentData().toString();
        if (id.isEmpty() || id == provider_) return;
        provider_ = id;
        emit provider_changed(id);
    });
    connect(expand_btn, &QPushButton::clicked, this, [this] { set_expanded(!expanded_); });
    auto *config = get_tool_settings_manager();
    expanded_ = config && config->get_translation_panel_expanded();
    retranslate();
    apply_scale();
}

void TranslationPanel::paintEvent(QPaintEvent *) { paint_rounded_panel(this); }

void TranslationPanel::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    emit resized();
}

void TranslationPanel::apply_scale() {
    setStyleSheet(build_settings_panel_stylesheet() + combo_popup_qss());
    const int mh = scaled(BASE_MARGIN_H), mv = scaled(BASE_MARGIN_V), spacing = scaled(BASE_SPACING);
    layout()->setContentsMargins(mh, mv, mh, mv);
    layout()->setSpacing(spacing);
    drawer_content_->layout()->setContentsMargins(0, 0, spacing, 0);
    drawer_content_->layout()->setSpacing(spacing);
    view_toggle->apply_scale();
    into_label->setStyleSheet(QString("color: %1; font-size: %2px;").arg(CONTROL_SUB.name()).arg(scaled(12)));
    // As wide as the longest language name, which reads the same in every UI language.
    QStringList languages;
    for (const auto &[code, name] : translation_languages()) languages << name;
    fit_pill_combo(language_combo, languages, QWIDGETSIZE_MAX);
    fit_service_combo();
    expand_btn->setFixedSize(scaled(BASE_BTN), scaled(BASE_BTN));
    slide_.stop();
    apply_open(expanded_ ? 1 : 0);
    update();
}

void TranslationPanel::fit_service_combo() {
    QStringList names;
    for (const auto &[id, name] : providers_) names << name;
    fit_pill_combo(service_combo, names, scaled(BASE_SERVICE_MAX_WIDTH));
}

void TranslationPanel::apply_open(qreal open) {
    open_ = open;
    // Folded, nothing behind the arrow takes clicks.
    drawer_->setVisible(open > 0);
    drawer_->set_open(open);
    expand_button_->set_turn(open);
    layout()->activate();
    adjustSize();
}

void TranslationPanel::set_expanded(bool expanded) {
    if (auto *config = get_tool_settings_manager()) config->set_translation_panel_expanded(expanded);
    if (expanded_ == expanded) return;
    expanded_ = expanded;
    expand_btn->setToolTip(panel_tr(expanded_ ? "Collapse" : "Expand"));
    slide_.stop();
    if (!isVisible()) {
        apply_open(expanded_ ? 1 : 0);
        return;
    }
    slide_.setStartValue(open_);
    slide_.setEndValue(expanded_ ? 1.0 : 0.0);
    slide_.start();
}

void TranslationPanel::retranslate() {
    view_toggle->set_labels({panel_tr("Translated"), panel_tr("Original")});
    view_toggle->setToolTip(panel_tr("Show original image"));
    into_label->setText(panel_tr("To"));
    language_combo->setToolTip(panel_tr("Target language"));
    service_combo->setToolTip(panel_tr("Translation service"));
    expand_btn->setToolTip(panel_tr(expanded_ ? "Collapse" : "Expand"));
    adjustSize();
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
    {
        const QSignalBlocker blocker(service_combo);
        service_combo->clear();
        for (const auto &[id, name] : providers_) service_combo->addItem(name, id);
        service_combo->setCurrentIndex(service_combo->findData(provider_));
    }
    fit_service_combo();
    if (!sliding()) apply_open(open_);
}
}
