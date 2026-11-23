#include "core/resource_manager.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "settings/tool_settings.h"
#include "translation/languages.h"
#include "translation/registry.h"
#include "translation/service.h"
#include "translation/speech_service.h"
#include "translation/worker.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/labels.h"
#include "ui/fluent_lite/segmented.h"
#include "ui/fluent_lite/switch.h"
#include "ui/fluent_lite/text_context_menu.h"
#include "ui/settings_ui/components.h"
#include "ui/settings_ui/dialog.h"
#include "ui/settings_ui/pages.h"
#include "ui/settings_ui/search.h"
#include <QAbstractButton>
#include <QButtonGroup>
#include <QDate>
#include <QDesktopServices>
#include <QFrame>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QThread>
#include <QToolTip>
#include <QUrl>
#include <algorithm>
#include <set>

// The translation settings page: a General tab (engine, options, read aloud) and an API tab with
// one card per provider. No provider is named here: each provider's credentials, own switches,
// hints, help link and which general options it uses all come from its own declaration
// (ProviderField / ProviderMetadata); the page only renders them, so a provider cannot be missing
// from either the form or the save.
namespace jietuba {
namespace {
// Label column width. Providers differ in field count and label length; a fixed width
// keeps inputs from jumping sideways when switching providers.
constexpr int LABEL_WIDTH = 135;
// Left room of a provider card's rows at 100%; the right room is card_right_margin().
constexpr int CARD_PADDING = 16;
constexpr int TAB_HEIGHT = 44;
constexpr int TAB_GAP = 14;
constexpr int CHIP_HEIGHT = 34;
constexpr int CHIP_COLUMNS = 4;
const char *const GENERAL_TAB = "general";
const char *const API_TAB = "api";
const char *const PROVIDER_ID = "provider_id";

QFont ui_font(int pixel_size, bool bold = false) {
    QFont font;
    font.setFamilies(FONT_FAMILIES());
    font.setPixelSize(dialog_scaled(pixel_size));
    font.setWeight(bold ? QFont::DemiBold : QFont::Normal);
    return font;
}

QString rgba(const QString &color, double alpha) {
    const QColor c = css_color(color);
    return QString("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(alpha);
}

// The line above every row of a provider card but the first, from the text column to the edge.
class RowLine : public QFrame {
public:
    explicit RowLine(QWidget *parent) : QFrame(parent) {
        setFixedHeight(dialog_scaled(1));
        apply_theme();
        QObject::connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { apply_theme(); });
    }
private:
    void apply_theme() { set_own_style(this, QString("background: %1; border: none; margin-left: %2px;").arg(ui_tokens(this).separator).arg(dialog_scaled(CARD_PADDING))); }
};

void add_row(QVBoxLayout *layout, QWidget *row) {
    layout->addWidget(new RowLine(row->parentWidget()));
    layout->addWidget(row);
}

// The page's scroll area with the tab row fixed above the scrolling content.
class TabbedScrollArea : public QScrollArea {
public:
    TabbedScrollArea(QWidget *parent, QWidget *header, int right_inset) : QScrollArea(parent), header_(header), right_inset_(right_inset) {
        header->setParent(this);
        setViewportMargins(0, header->height() + dialog_scaled(TAB_GAP), 0, 0);
    }
protected:
    void resizeEvent(QResizeEvent *event) override {
        QScrollArea::resizeEvent(event);
        header_->setGeometry(viewport()->x(), 0, std::max(0, viewport()->width() - right_inset_), header_->height());
    }
private:
    QWidget *header_;
    int right_inset_;
};

// One page shown at a time. Unlike a stack, the hidden pages take no room, so a short page does
// not leave the height of the tallest one below it.
class PageSwitcher : public QWidget {
public:
    explicit PageSwitcher(QWidget *parent) : QWidget(parent) {
        layout_ = new QVBoxLayout(this);
        layout_->setContentsMargins(0, 0, 0, 0);
        layout_->setSpacing(0);
    }
    void add_page(QWidget *page) {
        page->setParent(this);
        layout_->addWidget(page);
        pages_.append(page);
        page->setVisible(pages_.size() - 1 == current_);
    }
    void set_current(int index) {
        current_ = index;
        for (int i = 0; i < pages_.size(); ++i)
            if (pages_[i]) pages_[i]->setVisible(i == index);
    }
    QList<QWidget *> pages() const {
        QList<QWidget *> pages;
        for (const auto &page : pages_)
            if (page) pages.append(page);
        return pages;
    }
private:
    QVBoxLayout *layout_;
    QList<QPointer<QWidget>> pages_;
    int current_ = 0;
};

// The rounded frame around a provider's rows, drawn like a card group's body.
class ProviderCard : public QFrame {
public:
    explicit ProviderCard(QWidget *parent) : QFrame(parent) {
        setFrameShape(QFrame::NoFrame);
        QObject::connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { update(); });
    }
protected:
    void paintEvent(QPaintEvent *) override {
        const auto &t = ui_tokens(this);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(css_color(t.border));
        painter.setBrush(css_color(t.surface));
        const int radius = dialog_scaled(15);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    }
};

// This month's share of a free quota.
class QuotaBar : public QWidget {
public:
    explicit QuotaBar(QWidget *parent) : QWidget(parent) {
        setFixedHeight(dialog_scaled(4));
        QObject::connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { update(); });
    }
    void set_fraction(double fraction) {
        fraction_ = std::clamp(fraction, 0.0, 1.0);
        update();
    }
protected:
    void paintEvent(QPaintEvent *) override {
        const auto &t = ui_tokens(this);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        const qreal radius = height() / 2.0;
        painter.setBrush(css_color(t.border));
        painter.drawRoundedRect(QRectF(rect()), radius, radius);
        if (fraction_ <= 0) return;
        painter.setBrush(css_color(fraction_ >= 1 ? t.danger : t.accent));
        painter.drawRoundedRect(QRectF(0, 0, std::max<qreal>(height(), width() * fraction_), height()), radius, radius);
    }
private:
    double fraction_ = 0;
};

// Folds a provider's rarely changed settings: a chevron, its title and, while folded, what is inside.
class DisclosureRow : public QAbstractButton {
public:
    DisclosureRow(const QString &text, const QString &summary, QWidget *parent) : QAbstractButton(parent), summary_(summary) {
        setText(text);
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::TabFocus);
        setAttribute(Qt::WA_Hover);
        setFixedHeight(dialog_scaled(44));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        QObject::connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { update(); });
    }
    QSize sizeHint() const override { return {dialog_scaled(200), dialog_scaled(44)}; }
protected:
    void paintEvent(QPaintEvent *) override {
        const auto &t = ui_tokens(this);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (underMouse() || hasFocus()) painter.fillRect(rect(), css_color(t.surface_subtle));
        const qreal size = dialog_scaled(8);
        const qreal left = dialog_scaled(CARD_PADDING);
        const QPointF c(left + size / 2, height() / 2.0);
        QPen pen(css_color(t.text_muted), std::max(1.2, dialog_scaled(15) / 10.0));
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        const QPointF down[] = {{c.x() - size / 2, c.y() - size / 4}, {c.x(), c.y() + size / 4}, {c.x() + size / 2, c.y() - size / 4}};
        const QPointF right[] = {{c.x() - size / 4, c.y() - size / 2}, {c.x() + size / 4, c.y()}, {c.x() - size / 4, c.y() + size / 2}};
        painter.drawPolyline(isChecked() ? down : right, 3);

        const QFont title_font = ui_font(13);
        const int text_left = qRound(left + size) + dialog_scaled(10);
        const int title_width = QFontMetrics(title_font).horizontalAdvance(text()) + 1;
        painter.setFont(title_font);
        painter.setPen(css_color(t.text));
        painter.drawText(QRect(text_left, 0, title_width, height()), Qt::AlignLeft | Qt::AlignVCenter, text());
        if (isChecked() || summary_.isEmpty()) return;
        const QFont small = ui_font(12);
        const int x = text_left + title_width + dialog_scaled(10);
        const int room = width() - card_right_margin() - x;
        if (room <= 0) return;
        painter.setFont(small);
        painter.setPen(css_color(t.text_muted));
        painter.drawText(QRect(x, 0, room, height()), Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(small).elidedText(summary_, Qt::ElideRight, room));
    }
private:
    QString summary_;
};

// A provider on the API tab. A dot marks one whose required settings are filled, a check the one in
// use, and a warning sign the one in use while it still lacks them.
class ProviderChip : public QAbstractButton {
public:
    ProviderChip(const QString &provider_id, const QString &text, QWidget *parent) : QAbstractButton(parent) {
        setText(text);
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::TabFocus);
        setAttribute(Qt::WA_Hover);
        setProperty(PROVIDER_ID, provider_id);
        setFixedHeight(dialog_scaled(CHIP_HEIGHT));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        QObject::connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { update(); });
        QObject::connect(this, &QAbstractButton::toggled, this, [this] { update(); });
    }
    void set_state(bool configured, bool current) {
        setProperty("configured", configured);
        setProperty("current", current);
        const char *state = current ? (configured ? "In Use" : "Not configured yet") : configured ? "Configured" : "Not Configured";
        setToolTip(settings_tr(state));
        setAccessibleDescription(toolTip());
        update();
    }
    // The mark and the name, measured bold as when selected, plus the padding.
    QSize sizeHint() const override {
        const int name = QFontMetrics(ui_font(13, true)).horizontalAdvance(text());
        return {name + dialog_scaled(12) + dialog_scaled(6) + 2 * dialog_scaled(10), dialog_scaled(CHIP_HEIGHT)};
    }
    QSize minimumSizeHint() const override { return sizeHint(); }
protected:
    void paintEvent(QPaintEvent *) override {
        const auto &t = ui_tokens(this);
        const bool selected = isChecked();
        const bool current = property("current").toBool();
        const bool configured = property("configured").toBool();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QColor border = css_color(selected || hasFocus() ? t.accent : t.border);
        if (selected && !hasFocus()) border.setAlphaF(0.55f);
        painter.setPen(QPen(border, hasFocus() ? 2 : 1));
        painter.setBrush(css_color(selected ? t.accent_soft : underMouse() ? t.surface_hover : t.surface));
        const int radius = dialog_scaled(8);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);

        const QFont font = ui_font(13, selected);
        const QFontMetrics metrics(font);
        const int mark = current ? dialog_scaled(12) : configured ? dialog_scaled(6) : 0;
        const int gap = mark ? dialog_scaled(6) : 0;
        const int room = std::max(0, width() - 2 * dialog_scaled(10) - mark - gap);
        const QString name = metrics.elidedText(text(), Qt::ElideRight, room);
        const int name_width = metrics.horizontalAdvance(name);
        int x = (width() - mark - gap - name_width) / 2;
        const qreal cy = height() / 2.0;
        if (current && configured) {
            QPen pen(css_color(t.success), std::max(1.5, dialog_scaled(18) / 10.0));
            pen.setCapStyle(Qt::RoundCap);
            pen.setJoinStyle(Qt::RoundJoin);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            const qreal s = mark;
            const QPointF tick[] = {{x + s * 0.08, cy}, {x + s * 0.4, cy + s * 0.32}, {x + s * 0.95, cy - s * 0.35}};
            painter.drawPolyline(tick, 3);
        } else if (current) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(css_color(t.danger));
            painter.drawEllipse(QRectF(x, cy - mark / 2.0, mark, mark));
            painter.setBrush(Qt::white);
            const qreal w = mark * 0.16;
            painter.drawRoundedRect(QRectF(x + (mark - w) / 2, cy - mark * 0.3, w, mark * 0.36), w / 2, w / 2);
            painter.drawEllipse(QRectF(x + (mark - w) / 2, cy + mark * 0.16, w, w));
        } else if (configured) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(css_color(t.success));
            painter.drawEllipse(QRectF(x, cy - mark / 2.0, mark, mark));
        }
        x += mark + gap;
        painter.setFont(font);
        painter.setPen(css_color(selected ? t.accent_text : t.text));
        painter.drawText(QRect(x, 0, name_width + 1, height()), Qt::AlignLeft | Qt::AlignVCenter, name);
    }
};

// Equal columns of chips, as many as fit up to CHIP_COLUMNS, so a long name in a narrow window
// takes another row instead of widening the page. Grids sharing a cell width line up.
class ChipGrid : public QLayout {
public:
    explicit ChipGrid(QWidget *parent) : QLayout(parent) {
        setContentsMargins(0, 0, 0, 0);
        setSpacing(dialog_scaled(6));
    }
    ~ChipGrid() override {
        while (QLayoutItem *item = takeAt(0)) delete item;
    }
    void set_cell_width(int width) {
        cell_width_ = width;
        invalidate();
    }
    void addItem(QLayoutItem *item) override { items_.append(item); }
    int count() const override { return static_cast<int>(items_.size()); }
    QLayoutItem *itemAt(int index) const override { return items_.value(index); }
    QLayoutItem *takeAt(int index) override { return index >= 0 && index < items_.size() ? items_.takeAt(index) : nullptr; }
    Qt::Orientations expandingDirections() const override { return Qt::Horizontal; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return place(QRect(0, 0, width, 0), false); }
    QSize minimumSize() const override { return {cell_width_, heightForWidth(cell_width_)}; }
    QSize sizeHint() const override {
        const int width = CHIP_COLUMNS * cell_width_ + (CHIP_COLUMNS - 1) * spacing();
        return {width, heightForWidth(width)};
    }
    void setGeometry(const QRect &rect) override {
        QLayout::setGeometry(rect);
        place(rect, true);
    }
private:
    int place(const QRect &rect, bool apply) const {
        if (items_.isEmpty()) return 0;
        const int gap = spacing();
        const int columns = std::clamp((rect.width() + gap) / std::max(1, cell_width_ + gap), 1, CHIP_COLUMNS);
        const int cell = std::max(0, (rect.width() - gap * (columns - 1)) / columns);
        const int height = items_.first()->sizeHint().height();
        for (int i = 0; apply && i < items_.size(); ++i)
            items_[i]->setGeometry(QRect(rect.x() + (i % columns) * (cell + gap), rect.y() + (i / columns) * (height + gap), cell, height));
        const int rows = (static_cast<int>(items_.size()) + columns - 1) / columns;
        return rows * height + (rows - 1) * gap;
    }
    QList<QLayoutItem *> items_;
    int cell_width_ = 0;
};

// The providers in groups of chips; one is selected and its card shows below.
class ProviderPicker : public QWidget {
public:
    explicit ProviderPicker(QWidget *parent) : QWidget(parent), group_(new QButtonGroup(this)) {
        layout_ = new QVBoxLayout(this);
        layout_->setContentsMargins(0, 0, 0, 0);
        layout_->setSpacing(0);
        group_->setExclusive(true);
        QObject::connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { apply_theme(); });
    }
    void add_group(const QString &title, const std::vector<std::pair<QString, QString>> &items) {
        if (items.empty()) return;
        if (layout_->count()) layout_->addSpacing(dialog_scaled(14));
        auto *label = new QLabel(title, this);
        titles_.append(label);
        layout_->addWidget(label);
        layout_->addSpacing(dialog_scaled(6));
        auto *grid_host = new QWidget(this);
        auto *grid = new ChipGrid(grid_host);
        grids_.append(grid);
        for (const auto &[id, text] : items) {
            auto *chip = new ProviderChip(id, text, grid_host);
            grid->addWidget(chip);
            group_->addButton(chip);
            chips_[id] = chip;
            const QString key = id;
            QObject::connect(chip, &QAbstractButton::clicked, this, [this, key] { setCurrentItem(key); });
            cell_width_ = std::max(cell_width_, chip->minimumSizeHint().width());
        }
        for (auto *each : grids_) each->set_cell_width(cell_width_);
        layout_->addWidget(grid_host);
        apply_theme();
    }
    void setCurrentItem(const QString &id) {
        if (!chips_.contains(id)) return;
        chips_[id]->setChecked(true);
        if (current_ == id) return;
        current_ = id;
        if (on_select) on_select(id);
    }
    QString currentItem() const { return current_; }
    QString itemText(const QString &id) const { return chips_.contains(id) ? chips_[id]->text() : QString(); }
    ProviderChip *chip(const QString &id) const { return chips_.value(id); }
    std::function<void(const QString &)> on_select;
private:
    // Like a card group's title.
    void apply_theme() {
        const auto &t = ui_tokens(this);
        for (auto *label : titles_)
            set_own_style(label, QString("color: %1; font: 600 %2px %3; padding: %4px %5px 0 %5px; background: transparent;")
                                     .arg(t.text).arg(dialog_scaled(12)).arg(FONT_FAMILY).arg(dialog_scaled(2)).arg(dialog_scaled(8)));
    }
    QVBoxLayout *layout_;
    QButtonGroup *group_;
    QMap<QString, ProviderChip *> chips_;
    QList<QLabel *> titles_;
    QList<ChipGrid *> grids_;
    int cell_width_ = 0;
    QString current_;
};

// A small tag beside a provider's name; good tags are green, the others muted.
QLabel *make_badge(QWidget *parent, const QString &text, bool good) {
    auto *badge = new QLabel(text, parent);
    const auto apply = [badge, good] {
        const auto &t = ui_tokens(badge);
        const QString color = good ? t.success : t.text_muted;
        set_own_style(badge, QString("color: %1; background: %2; border: none; border-radius: %3px; padding: %4px %5px; font: %6px %7;")
                                 .arg(color, rgba(color, good ? 0.14 : 0.12))
                                 .arg(dialog_scaled(4))
                                 .arg(dialog_scaled(1))
                                 .arg(dialog_scaled(7))
                                 .arg(dialog_scaled(11))
                                 .arg(FONT_FAMILY));
    };
    apply();
    QObject::connect(get_ui_theme(), &UIThemeManager::theme_changed, badge, apply);
    return badge;
}

// What the page updates as the engine, the fields and this month's usage change.
struct ProviderView {
    bool configured = false;
    QPointer<QLabel> in_use;
    QPointer<QLabel> unconfigured;
    QPointer<QPushButton> use_button;
    QPointer<QuotaBar> quota_bar;
    QPointer<QLabel> quota_label;
};

struct TranslationPage {
    SettingsDialog *dialog = nullptr;
    std::vector<ProviderMetadata> providers;
    QPointer<QComboBox> engine_combo;
    QPointer<SettingCard> engine_card;
    QPointer<SettingsCardGroup> options_group;
    QPointer<SegmentedWidget> tabs;
    QPointer<ProviderPicker> picker;
    QPointer<QScrollArea> scroll;
    QMap<QString, ProviderView> views;
    // Every switch of a general option a provider uses, by option; each provider using it has one.
    QMap<QString, QList<QPointer<SwitchButton>>> request_switches;
};

// Request threads are not children of the settings window: the window may close while a
// thread still waits on the network, and a destroyed parent would take the running QThread with it.
void keep_until_finished(QThread *thread) {
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

class ModelListThread : public QThread {
    Q_OBJECT
public:
    explicit ModelListThread(std::unique_ptr<TranslationProvider> provider) : provider_(std::move(provider)) {}
signals:
    // (success, models / error message).
    void finished_signal(bool ok, const QStringList &models, const QString &error);
protected:
    void run() override {
        try {
            emit finished_signal(true, provider_->list_models(), {});
        } catch (const std::exception &exc) {
            emit finished_signal(false, {}, QString::fromUtf8(exc.what()));
        }
    }
private:
    std::unique_ptr<TranslationProvider> provider_;
};

std::shared_ptr<TranslationConfig> pending_provider_config(SettingsDialog *dialog, const QString &provider_id) {
    const auto fields = provider_fields::provider_fields(*dialog->translation_registry, provider_id);
    return provider_fields::pending_config(dialog->config_manager, fields, *dialog->provider_field_widgets);
}

TranslationService pending_service(SettingsDialog *dialog, const QString &provider_id) { return TranslationService(dialog->translation_registry, pending_provider_config(dialog, provider_id)); }

// Whether the form's current, possibly unsaved values let the provider translate.
bool pending_configured(SettingsDialog *dialog, const QString &provider_id) {
    try {
        const auto config = pending_provider_config(dialog, provider_id);
        return dialog->translation_registry->create(provider_id, config->get_translation_provider_config(provider_id))->is_configured();
    } catch (const std::exception &) {
        return false;
    }
}

void show_status(SettingsDialog *dialog, const QString &provider_id, const QString &text) {
    if (auto *label = dialog->provider_status_labels.value(provider_id).data()) label->setText(text);
}

void test_connection(SettingsDialog *dialog, const QString &provider_id, QPushButton *button) {
    QString target;
    if (auto *combo = dialog->attr<QComboBox>("translation_target_combo")) target = combo->currentData().toString();
    const TranslationRequest request("Hello, world!", !target.isEmpty() ? target : "ZH");
    auto *worker = new TranslationWorker(pending_service(dialog, provider_id), request, provider_id);
    button->setEnabled(false);
    show_status(dialog, provider_id, dialog->tr_text("Testing..."));
    QPointer<QPushButton> guarded_button(button);
    QPointer<SettingsDialog> guarded_dialog(dialog);
    QObject::connect(worker, &TranslationWorker::finished_signal, worker, [guarded_button, guarded_dialog, provider_id](const TranslationResult &result) {
        if (!guarded_button || !guarded_dialog) return;
        guarded_button->setEnabled(true);
        show_status(guarded_dialog, provider_id, result.success ? QString::fromUtf8("✓ ") + result.translated_text : QString::fromUtf8("✗ ") + result.error_message);
    });
    keep_until_finished(worker);
}

// The status sits at the bottom of the provider's card, several rows away from the model
// row, so a failure is repeated next to the button. No owning widget is passed:
// the button's style sheet would then style the tooltip with dark text on black.
void show_near(QPushButton *button, const QString &text) { QToolTip::showText(button->mapToGlobal(QPoint(0, button->height())), text, nullptr, QRect(), 8000); }

void fetch_models(SettingsDialog *dialog, const QString &provider_id, QLineEdit *edit, QPushButton *button) {
    std::unique_ptr<TranslationProvider> provider;
    try {
        provider = pending_service(dialog, provider_id).provider(provider_id);
    } catch (const std::invalid_argument &exc) {
        show_status(dialog, provider_id, QString::fromUtf8("✗ ") + QString::fromUtf8(exc.what()));
        return;
    }
    auto *thread = new ModelListThread(std::move(provider));
    button->setEnabled(false);
    show_status(dialog, provider_id, dialog->tr_text("Fetching models..."));
    QPointer<QPushButton> guarded_button(button);
    QPointer<QLineEdit> guarded_edit(edit);
    QPointer<SettingsDialog> guarded_dialog(dialog);
    QObject::connect(thread, &ModelListThread::finished_signal, thread,
                     [guarded_button, guarded_edit, guarded_dialog, provider_id](bool ok, const QStringList &models, const QString &error) {
                         if (!guarded_button || !guarded_dialog) return;
                         guarded_button->setEnabled(true);
                         if (!ok || models.isEmpty()) {
                             const QString text = !ok ? QString::fromUtf8("✗ ") + error : guarded_dialog->tr_text("No models returned");
                             show_status(guarded_dialog, provider_id, text);
                             show_near(guarded_button, text);
                             return;
                         }
                         show_status(guarded_dialog, provider_id, "");
                         if (!guarded_edit) return;
                         QMenu menu(guarded_edit);
                         style_popup_menu(&menu, ui_tokens(guarded_edit));
                         for (const auto &model : models) {
                             QLineEdit *target = guarded_edit;
                             QObject::connect(menu.addAction(model), &QAction::triggered, target, [target, model] { target->setText(model); });
                         }
                         menu.exec(guarded_edit->mapToGlobal(QPoint(0, guarded_edit->height())));
                     });
    keep_until_finished(thread);
}

void toggle_secret(SettingsDialog *dialog, QLineEdit *edit, QPushButton *button) {
    if (edit->echoMode() == QLineEdit::Password) {
        edit->setEchoMode(QLineEdit::Normal);
        button->setText(dialog->tr_text("Hide"));
    } else {
        edit->setEchoMode(QLineEdit::Password);
        button->setText(dialog->tr_text("Show"));
    }
    adjust_button_width(button, 60);
}

// A switch with its title and description, in a provider card.
std::pair<WhiteCard *, SwitchButton *> toggle_row(QWidget *parent, const QString &title, const QString &description, bool checked) {
    auto *card = new WhiteCard(parent);
    auto *row = new QHBoxLayout(card);
    row->setContentsMargins(dialog_scaled(CARD_PADDING), dialog_scaled(10), card_right_margin(), dialog_scaled(10));
    row->setSpacing(dialog_scaled(10));
    auto *text = new QVBoxLayout;
    text->setSpacing(dialog_scaled(2));
    auto *title_label = new QLabel(title, card);
    apply_theme_text_style(title_label, 13);
    text->addWidget(title_label);
    QLabel *description_label = nullptr;
    if (!description.isEmpty()) {
        description_label = new QLabel(description, card);
        description_label->setWordWrap(true);
        apply_theme_text_style(description_label, 12, false, {}, true);
        text->addWidget(description_label);
    }
    mark_search_row(card, title_label, description_label);
    row->addLayout(text, 1);
    auto *switch_button = new SwitchButton(card);
    switch_button->setChecked(checked);
    row->addWidget(switch_button, 0, Qt::AlignVCenter);
    card->setMinimumHeight(dialog_scaled(58));
    return {card, switch_button};
}

// One row per field, by the field's kind; returns the row and the control holding the value.
std::pair<WhiteCard *, QWidget *> build_field(SettingsDialog *dialog, QWidget *parent, const ProviderField &field, const QString &provider_id) {
    if (field.kind == ProviderField::Toggle) {
        const auto row = toggle_row(parent, dialog->tr_text(field.label.toUtf8().constData()),
                                    field.description.isEmpty() ? QString() : dialog->tr_text(field.description.toUtf8().constData()),
                                    provider_fields::read_config(dialog->config_manager, field).toBool());
        return {row.first, row.second};
    }
    auto *card = new WhiteCard(parent);
    auto *row = new QHBoxLayout(card);
    row->setContentsMargins(dialog_scaled(CARD_PADDING), dialog_scaled(12), card_right_margin(), dialog_scaled(12));
    row->setSpacing(dialog_scaled(10));
    auto *title = new QLabel(dialog->tr_text(field.label.toUtf8().constData()), card);
    apply_theme_text_style(title, 13);
    title->setFixedWidth(dialog_scaled(LABEL_WIDTH));
    title->setWordWrap(true);
    row->addWidget(title);
    mark_search_row(card, title);
    auto *edit = new LineEdit(card);
    edit->setText(provider_fields::read_config(dialog->config_manager, field).toString());
    if (!field.placeholder.isEmpty()) edit->setPlaceholderText(dialog->tr_text(field.placeholder.toUtf8().constData()));
    if (field.secret) edit->setEchoMode(QLineEdit::Password);
    row->addWidget(edit, 1);
    // Every secret field has a show / hide button, so each can be checked by eye.
    if (field.secret) {
        auto *button = new PushButton(dialog->tr_text("Show"), card);
        button->setFixedHeight(dialog_scaled(32));
        adjust_button_width(button, 60);
        QObject::connect(button, &QPushButton::clicked, dialog, [dialog, edit, button] { toggle_secret(dialog, edit, button); });
        row->addWidget(button);
    }
    if (field.kind == ProviderField::Model) {
        auto *button = new PushButton(dialog->tr_text("Fetch Models"), card);
        button->setFixedHeight(dialog_scaled(32));
        adjust_button_width(button, 60);
        QObject::connect(button, &QPushButton::clicked, dialog, [dialog, provider_id, edit, button] { fetch_models(dialog, provider_id, edit, button); });
        row->addWidget(button);
    }
    card->setMinimumHeight(dialog_scaled(58));
    return {card, edit};
}

// The provider's hint. URLs never wrap inside: word joiners (U+2060) between their characters keep
// a narrow card from breaking "http://" from the rest; <nobr> only covers spaces.
QString notice_text(SettingsDialog *dialog, const ProviderMetadata &metadata) {
    if (metadata.notice.isEmpty()) return {};
    static const QRegularExpression url(R"(https?://[\w.:/%?#=&-]+)", QRegularExpression::NoPatternOption);
    const QString text = dialog->tr_text(metadata.notice.toUtf8().constData());
    QString result;
    qsizetype last = 0;
    for (auto it = url.globalMatch(text); it.hasNext();) {
        const auto match = it.next();
        result += text.mid(last, match.capturedStart() - last);
        const QString found = match.captured(0);
        QString joined;
        for (int i = 0; i < found.size(); ++i) {
            if (i) joined += QChar(0x2060);
            joined += found[i];
        }
        result += joined;
        last = match.capturedEnd();
    }
    return result + text.mid(last);
}

// The hint followed by the help link, which some hints lead into ("Get API key at").
QString notice_html(SettingsDialog *dialog, const ProviderMetadata &metadata) {
    QStringList parts;
    if (const QString text = notice_text(dialog, metadata); !text.isEmpty()) parts << text.toHtmlEscaped();
    if (!metadata.help_url.isEmpty()) {
        const QString label = !metadata.help_label.isEmpty() ? dialog->tr_text(metadata.help_label.toUtf8().constData()) : metadata.help_url;
        parts << QString("<a href=\"%1\" style=\"color: %2;\">%3</a>").arg(metadata.help_url, ACCENT, label.toHtmlEscaped());
    }
    return parts.join(" ");
}

struct QuotaUse {
    qint64 used = 0;
    qint64 limit = 0;
    bool guarded = false;
};

std::optional<QuotaUse> quota_use(SettingsDialog *dialog, const QString &provider_id, std::optional<qint64> usage) {
    if (provider_id.isEmpty() || !dialog->translation_registry || !dialog->config_manager) return std::nullopt;
    std::unique_ptr<TranslationProvider> provider;
    try {
        provider = dialog->translation_registry->create(provider_id, {});
    } catch (const std::invalid_argument &) {
        return std::nullopt;
    }
    if (provider->quota_limit_chars <= 0) return std::nullopt;
    const auto custom = dialog->config_manager->get_translation_quota_limit(provider_id);
    QuotaUse use;
    use.limit = custom > 0 ? custom : provider->quota_limit_chars;
    use.used = usage ? *usage : dialog->config_manager->get_translation_usage_chars(provider_id);
    use.guarded = dialog->config_manager->get_translation_quota_enabled(provider_id);
    return use;
}

// Until the first background read returns, usage comes from the settings rather than as 0.
std::optional<qint64> read_usage(SettingsDialog *dialog, const QString &provider_id) {
    const auto read = dialog->translation_usage.constFind(provider_id);
    return read == dialog->translation_usage.cend() ? std::nullopt : std::optional<qint64>(*read);
}

const ProviderMetadata *find_provider(const TranslationPage &page, const QString &provider_id) {
    for (const auto &metadata : page.providers)
        if (metadata.provider_id == provider_id) return &metadata;
    return nullptr;
}

QString current_engine(const TranslationPage &page) { return page.engine_combo ? page.engine_combo->currentData().toString() : QString(); }

// The model a language model provider uses: the field's text, or the default it shows.
QString model_name(SettingsDialog *dialog, const ProviderMetadata &metadata) {
    for (const auto &field : metadata.credentials) {
        if (field.kind != ProviderField::Model) continue;
        auto *edit = qobject_cast<QLineEdit *>(dialog->provider_field_widgets->value(field.config_key).data());
        if (!edit) return {};
        return !edit->text().trimmed().isEmpty() ? edit->text().trimmed() : edit->placeholderText();
    }
    return {};
}

// Whether the engine in use is ready, under its name on the General tab; a link to its card.
void apply_engine_status(TranslationPage &page) {
    if (!page.engine_card) return;
    SettingsDialog *dialog = page.dialog;
    const auto &t = ui_tokens(page.engine_card);
    const QString id = current_engine(page);
    const ProviderMetadata *metadata = find_provider(page, id);
    const ProviderView view = page.views.value(id);
    QString text;
    QString color = t.text_muted;
    if (metadata && !view.configured) {
        text = dialog->tr_text("Not configured yet");
        color = t.danger;
    } else if (metadata) {
        QString detail;
        if (const auto use = quota_use(dialog, id, read_usage(dialog, id)); use && use->limit > 0) {
            const double percent = 100.0 * use->used / use->limit;
            const QString shown = use->used <= 0 ? "0" : percent < 1 ? "<1" : QString::number(qRound(percent));
            detail = dialog->tr_text("%1% used this month").arg(shown);
        } else {
            detail = model_name(dialog, *metadata);
        }
        text = dialog->tr_text("Configured") + (detail.isEmpty() ? QString() : QString::fromUtf8(" · ") + detail);
    }
    const QString dot_color = view.configured ? t.success : t.danger;
    page.engine_card->setContent(text.isEmpty() ? QString()
                                                : QString("<a href=\"configure\" style=\"color: %1; text-decoration: none;\"><span style=\"color: %2; font-size: %3px;\">"
                                                          "&#9679;</span>&nbsp;%4</a>")
                                                      .arg(color, dot_color)
                                                      .arg(dialog_scaled(9))
                                                      .arg(text.toHtmlEscaped()));
    if (page.options_group) page.options_group->refreshHeight();
}

// A provider's chip, tags, "Use This" button and quota line from the cached state.
void apply_view(TranslationPage &page, const QString &provider_id) {
    const ProviderView view = page.views.value(provider_id);
    const bool current = provider_id == current_engine(page);
    if (page.picker)
        if (auto *chip = page.picker->chip(provider_id)) chip->set_state(view.configured, current);
    if (view.in_use) view.in_use->setVisible(current);
    if (view.unconfigured) view.unconfigured->setVisible(!view.configured);
    if (view.use_button) view.use_button->setVisible(!current && view.configured);
    if (view.quota_label) {
        if (const auto use = quota_use(page.dialog, provider_id, read_usage(page.dialog, provider_id))) {
            view.quota_label->setText(quota_usage_text(page.dialog, provider_id, use->used));
            if (view.quota_bar) view.quota_bar->set_fraction(use->limit > 0 ? double(use->used) / use->limit : 0);
        }
    }
}

void apply_all(TranslationPage &page) {
    for (const auto &metadata : page.providers) apply_view(page, metadata.provider_id);
    apply_engine_status(page);
}

void update_provider(TranslationPage &page, const QString &provider_id) {
    page.views[provider_id].configured = pending_configured(page.dialog, provider_id);
    apply_view(page, provider_id);
    if (provider_id == current_engine(page)) apply_engine_status(page);
}

void open_provider(TranslationPage &page, const QString &provider_id) {
    if (page.tabs) page.tabs->setCurrentItem(API_TAB);
    if (page.picker) page.picker->setCurrentItem(provider_id);
    if (page.scroll) page.scroll->verticalScrollBar()->setValue(0);
}

// The switch of a general option a provider uses. All switches of one option show the same value,
// and the first is the one saved.
void add_request_switch(const std::shared_ptr<TranslationPage> &page, QVBoxLayout *layout, QWidget *parent, const QString &option) {
    SettingsDialog *dialog = page->dialog;
    const bool split = option == "split_sentences";
    if (!split && option != "preserve_formatting") return;
    const auto row = toggle_row(parent, dialog->tr_text(split ? "Ignore Line Breaks" : "Preserve Formatting"),
                                dialog->tr_text(split ? "Merge multi-line text for better translation" : "Keep original text formatting"),
                                split ? dialog->config_manager->get_translation_split_sentences() : dialog->config_manager->get_translation_preserve_formatting());
    add_row(layout, row.first);
    SwitchButton *button = row.second;
    auto &switches = page->request_switches[option];
    if (switches.isEmpty()) dialog->set_attr(split ? "split_sentences_toggle" : "preserve_formatting_toggle", button);
    switches.append(button);
    const std::weak_ptr<TranslationPage> weak = page;
    QObject::connect(button, &SwitchButton::checkedChanged, button, [weak, option, button](bool checked) {
        const auto page = weak.lock();
        if (!page) return;
        for (const auto &other : page->request_switches.value(option)) {
            if (!other || other == button || other->isChecked() == checked) continue;
            const QSignalBlocker blocker(other.data());
            other->setChecked(checked);
        }
    });
}

QWidget *build_provider_card(const std::shared_ptr<TranslationPage> &page, const ProviderMetadata &metadata, QWidget *parent) {
    SettingsDialog *dialog = page->dialog;
    const QString id = metadata.provider_id;
    ProviderView &view = page->views[id];
    auto *card = new ProviderCard(parent);
    card->setProperty(PROVIDER_ID, id);
    auto *layout = new QVBoxLayout(card);
    // Inside the border, so a highlighted row does not paint over it.
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);

    // Name, tags and "Use This"; then the provider's hint and this month's quota.
    auto *header = new QWidget(card);
    auto *header_layout = new QVBoxLayout(header);
    header_layout->setContentsMargins(dialog_scaled(CARD_PADDING), dialog_scaled(12), card_right_margin(), dialog_scaled(13));
    header_layout->setSpacing(dialog_scaled(4));
    auto *title_row = new QWidget(header);
    // As tall as the button, so the header keeps its height when the button hides.
    title_row->setMinimumHeight(dialog_scaled(34));
    auto *title_layout = new QHBoxLayout(title_row);
    title_layout->setContentsMargins(0, 0, 0, 0);
    title_layout->setSpacing(dialog_scaled(8));
    auto *name = new QLabel(dialog->tr_text(metadata.display_name.toUtf8().constData()), title_row);
    apply_theme_text_style(name, 15, true);
    title_layout->addWidget(name, 0, Qt::AlignVCenter);
    view.in_use = make_badge(title_row, dialog->tr_text("In Use"), true);
    title_layout->addWidget(view.in_use, 0, Qt::AlignVCenter);
    view.unconfigured = make_badge(title_row, dialog->tr_text("Not Configured"), false);
    title_layout->addWidget(view.unconfigured, 0, Qt::AlignVCenter);
    title_layout->addStretch(1);
    auto *use_button = new PushButton(dialog->tr_text("Use This"), title_row);
    use_button->setFixedHeight(dialog_scaled(32));
    adjust_button_width(use_button, 60);
    view.use_button = use_button;
    const std::weak_ptr<TranslationPage> weak = page;
    QObject::connect(use_button, &QPushButton::clicked, use_button, [weak, id] {
        const auto page = weak.lock();
        if (page && page->engine_combo) page->engine_combo->setCurrentIndex(page->engine_combo->findData(id));
    });
    title_layout->addWidget(use_button);
    header_layout->addWidget(title_row);
    QLabel *notice = nullptr;
    if (const QString html = notice_html(dialog, metadata); !html.isEmpty()) {
        notice = new QLabel(html, header);
        notice->setTextFormat(Qt::RichText);
        notice->setWordWrap(true);
        notice->setOpenExternalLinks(true);
        apply_theme_text_style(notice, 12, false, {}, true);
        header_layout->addWidget(notice);
    }
    // The provider's name finds its card.
    mark_search_row(header, name, notice);
    if (quota_use(dialog, id, std::nullopt)) {
        header_layout->addSpacing(dialog_scaled(5));
        view.quota_bar = new QuotaBar(header);
        header_layout->addWidget(view.quota_bar);
        auto *quota = new QLabel(header);
        quota->setObjectName("ProviderQuota");
        quota->setWordWrap(true);
        apply_theme_text_style(quota, 12, false, {}, true);
        view.quota_label = quota;
        header_layout->addWidget(quota);
    }
    layout->addWidget(header);

    // Settings needed to work, the provider's own switches and the general options it uses.
    std::vector<ProviderField> folded;
    for (const auto &field : metadata.all_fields()) {
        if (field.advanced) {
            folded.push_back(field);
            continue;
        }
        const auto [row, control] = build_field(dialog, card, field, id);
        (*dialog->provider_field_widgets)[field.config_key] = control;
        add_row(layout, row);
    }
    for (const char *option : {"split_sentences", "preserve_formatting"})
        if (metadata.supported_request_options.contains(option)) add_request_switch(page, layout, card, option);

    // The rest, folded.
    if (!folded.empty()) {
        QStringList labels;
        for (const auto &field : folded) labels << dialog->tr_text(field.label.toUtf8().constData());
        auto *disclosure = new DisclosureRow(dialog->tr_text("Advanced"), labels.join(QString::fromUtf8(" · ")), card);
        add_row(layout, disclosure);
        auto *box = new QWidget(card);
        auto *box_layout = new QVBoxLayout(box);
        box_layout->setContentsMargins(0, 0, 0, 0);
        box_layout->setSpacing(0);
        for (const auto &field : folded) {
            const auto [row, control] = build_field(dialog, box, field, id);
            (*dialog->provider_field_widgets)[field.config_key] = control;
            add_row(box_layout, row);
        }
        box->setVisible(false);
        layout->addWidget(box);
        QObject::connect(disclosure, &QAbstractButton::toggled, box, [box](bool open) { box->setVisible(open); });
        mark_search_revealable(box, [disclosure] { disclosure->setChecked(true); });
    }

    // The last test or model list result, and the test.
    auto *footer = new WhiteCard(card);
    auto *footer_layout = new QHBoxLayout(footer);
    footer_layout->setContentsMargins(dialog_scaled(CARD_PADDING), dialog_scaled(12), card_right_margin(), dialog_scaled(12));
    footer_layout->setSpacing(dialog_scaled(10));
    auto *status = new QLabel(footer);
    status->setWordWrap(true);
    status->setTextInteractionFlags(status->textInteractionFlags() | Qt::TextSelectableByMouse);
    apply_theme_text_style(status, 13, false, {}, true);
    footer_layout->addWidget(status, 1);
    dialog->provider_status_labels[id] = status;
    auto *test_button = new PushButton(dialog->tr_text("Test Connection"), footer);
    test_button->setFixedHeight(dialog_scaled(32));
    adjust_button_width(test_button, 60);
    QObject::connect(test_button, &QPushButton::clicked, dialog, [dialog, id, test_button] { test_connection(dialog, id, test_button); });
    footer_layout->addWidget(test_button);
    footer->setMinimumHeight(dialog_scaled(58));
    add_row(layout, footer);

    // Typing a key can make the provider usable.
    for (const auto &field : metadata.all_fields()) {
        QWidget *control = dialog->provider_field_widgets->value(field.config_key);
        if (auto *edit = qobject_cast<QLineEdit *>(control))
            QObject::connect(edit, &QLineEdit::textChanged, card, [weak, id] {
                if (const auto page = weak.lock()) update_provider(*page, id);
            });
        else if (auto *button = qobject_cast<SwitchButton *>(control))
            QObject::connect(button, &SwitchButton::checkedChanged, card, [weak, id] {
                if (const auto page = weak.lock()) update_provider(*page, id);
            });
    }
    return card;
}

// SAPI rates for the labelled speeds: each rate step is about 12% faster.
const std::pair<const char *, int> SPEECH_RATES[] = {{"0.5×", -6}, {"0.75×", -3}, {"1×", 0}, {"1.25×", 2}, {"1.5×", 4}, {"2×", 6}};
const int SPEECH_VOLUMES[] = {100, 80, 60, 40, 20};

QString speech_sample(const QLocale &locale) {
    const QString key = voice_language_key(locale);
    if (key == "zh") return QString::fromUtf8("你好，这是朗读音色的试听。");
    if (key == "ja") return QString::fromUtf8("こんにちは。読み上げ音声のテストです。");
    if (key == "ko") return QString::fromUtf8("안녕하세요. 읽기 음성 미리 듣기입니다.");
    if (key == "en") return "Hello, this is a preview of the reading voice.";
    return locale.nativeLanguageName();
}

// "Microsoft Huihui - Chinese (Simplified, PRC)" shows as "Huihui": the language is already the
// row's title, and the shared vendor prefix would leave the narrow combo no room for the name.
QString voice_label(const VoiceInfo &voice) {
    QString label = voice.name.section(" - ", 0, 0).trimmed();
    if (label.startsWith("Microsoft ", Qt::CaseInsensitive) && label.size() > 10) label = label.mid(10);
    return label;
}

ComboBox *speech_style_combo(SettingsDialog *dialog, SettingCard *card, const QString &key, const std::vector<std::pair<QString, int>> &items) {
    auto *combo = new ComboBox(card);
    for (const auto &[label, value] : items) combo->addItem(label, value);
    int index = combo->findData(dialog->config_manager->get_app_setting(key));
    if (index < 0) index = combo->findData(dialog->config_manager->app_defaults().value(key));
    combo->setCurrentIndex(std::max(0, index));
    dialog->behavior_controls.emplace_back(key, combo);
    card->addControl(combo);
    return combo;
}

SpeechStyle pending_speech_style(SettingsDialog *dialog) {
    SpeechStyle style;
    if (auto *combo = qobject_cast<QComboBox *>(dialog->behavior_control("speech_rate"))) style.rate = combo->currentData().toInt();
    if (auto *combo = qobject_cast<QComboBox *>(dialog->behavior_control("speech_volume"))) style.volume = combo->currentData().toInt();
    return style;
}

// Reads a sample with the row's current voice and the page's current speed and volume, so a
// choice can be heard before the autosave writes it; pressed again it stops. An icon button,
// since a captioned one would leave the voice combo too little of the control column.
PushButton *speech_preview_button(SettingsDialog *dialog, SettingCard *card, QComboBox *combo, const QLocale &locale) {
    const QString preview_text = dialog->tr_text("Preview"), stop_text = dialog->tr_text("Stop");
    const IconSource preview_icon = ResourceManager::get_icon_path(QString::fromUtf8("录制声音.svg"));
    const IconSource stop_icon = ResourceManager::get_icon_path(QString::fromUtf8("结束录制.svg"));
    auto *button = new PushButton({}, card, preview_icon);
    button->setFixedSize(dialog_scaled(32), dialog_scaled(32));
    button->setIconSize(QSize(dialog_scaled(16), dialog_scaled(16)));
    button->setToolTip(preview_text);
    button->setAccessibleName(preview_text);
    const quint64 owner = SpeechService::new_owner();
    QPointer<SpeechService> service = SpeechService::instance();
    QObject::connect(button, &QPushButton::clicked, button, [dialog, combo, locale, service, owner] {
        if (!service) return;
        if (service->is_speaking(owner)) service->stop(owner);
        else service->preview(owner, speech_sample(locale), locale.name(), combo->currentData().toString(), pending_speech_style(dialog));
    });
    QObject::connect(service, &SpeechService::state_changed, button, [button, service, owner, preview_text, stop_text, preview_icon, stop_icon] {
        const bool speaking = service && service->is_speaking(owner);
        button->setIcon(speaking ? stop_icon : preview_icon);
        button->setToolTip(speaking ? stop_text : preview_text);
        button->setAccessibleName(speaking ? stop_text : preview_text);
    });
    QObject::connect(button, &QObject::destroyed, service, [service, owner] {
        if (service) service->stop(owner);
    });
    return button;
}

QStringList voice_tokens(const std::vector<VoiceInfo> &voices) {
    QStringList tokens;
    for (const auto &voice : voices) tokens << voice.token.toLower() + "|" + voice.name + "|" + voice.locale.name();
    tokens.sort();
    return tokens;
}

// One row per language with a voice installed, in the order of the translation languages.
SettingsCardGroup *build_speech_group(SettingsDialog *dialog, QWidget *page, std::vector<VoiceInfo> voices) {
    auto *group = new SettingsCardGroup(dialog->tr_text("Read Aloud"), page, "translate");
    // What the rows were built from, so a later visit can tell whether voices came or went.
    group->setProperty("voice_tokens", voice_tokens(voices));
    dialog->set_attr("speech_group", group);
    std::stable_sort(voices.begin(), voices.end(), [](const VoiceInfo &a, const VoiceInfo &b) { return voice_label(a).compare(voice_label(b), Qt::CaseInsensitive) < 0; });
    QStringList keys;
    QMap<QString, QString> titles;
    for (const auto &[code, name] : translation_languages()) {
        const QString key = voice_language_key(code);
        if (!titles.contains(key)) titles[key] = name;
    }
    for (const auto &voice : voices) {
        const QString key = voice_language_key(voice.locale);
        if (!keys.contains(key)) keys << key;
        if (!titles.contains(key)) titles[key] = voice.locale.nativeLanguageName();
    }
    const auto order = [](const QString &key) {
        const auto &languages = translation_languages();
        const auto it = std::find_if(languages.begin(), languages.end(), [&key](const auto &entry) { return voice_language_key(entry.first) == key; });
        return static_cast<int>(it - languages.begin());
    };
    std::stable_sort(keys.begin(), keys.end(), [&order](const QString &a, const QString &b) { return order(a) != order(b) ? order(a) < order(b) : a < b; });

    dialog->speech_voice_combos.clear();
    for (const auto &key : keys) {
        auto *card = new SettingCard(FluentIcon::Language, titles.value(key), {}, group);
        auto *combo = new ComboBox(card);
        combo->addItem(dialog->tr_text("Auto"), QString());
        for (const auto &voice : voices)
            if (voice_language_key(voice.locale) == key) combo->addItem(voice_label(voice), voice.token);
        const auto select_voice = [dialog, combo, key] {
            auto &preferences = dialog->speech_voice_preferences;
            preferences.removeIf([&key](const QString &entry) { return entry.section('=', 0, 0) == key; });
            const QString token = combo->currentData().toString();
            if (!token.isEmpty()) preferences << key + "=" + token;
        };
        QObject::connect(combo, &QComboBox::currentIndexChanged, dialog, select_voice);
        QObject::connect(combo, &QComboBox::activated, dialog, [dialog, select_voice] {
            // Auto can already be displayed for an unavailable preference.
            select_voice();
            dialog->schedule_save();
        });
        card->addControl(combo);
        card->addControl(speech_preview_button(dialog, card, combo, QLocale(key)), Qt::AlignRight);
        group->addSettingCard(card);
        dialog->speech_voice_combos.emplace_back(key, combo);
    }
    select_speech_voices(dialog, dialog->config_manager->get_speech_voices());

    auto *rate_card = new SettingCard(FluentIcon::StopWatch, dialog->tr_text("Speech Rate"), {}, group);
    std::vector<std::pair<QString, int>> rates;
    for (const auto &[label, rate] : SPEECH_RATES) rates.emplace_back(QString::fromUtf8(label), rate);
    speech_style_combo(dialog, rate_card, "speech_rate", rates);
    group->addSettingCard(rate_card);
    auto *volume_card = new SettingCard(FluentIcon::FontSize, dialog->tr_text("Volume"), {}, group);
    std::vector<std::pair<QString, int>> volumes;
    for (const int volume : SPEECH_VOLUMES) volumes.emplace_back(QString("%1%").arg(volume), volume);
    speech_style_combo(dialog, volume_card, "speech_volume", volumes);
    group->addSettingCard(volume_card);

    auto *add_card = new SettingCard(FluentIcon::Download, dialog->tr_text("Add Voices"), dialog->tr_text("Install voices for more languages in Windows Settings."),
                                     group);
    auto *open_button = new PushButton(dialog->tr_text("Open Settings"), add_card);
    open_button->setFixedHeight(dialog_scaled(32));
    adjust_button_width(open_button, 60);
    QObject::connect(open_button, &QPushButton::clicked, dialog, [] { QDesktopServices::openUrl(QUrl("ms-settings:speech")); });
    add_card->addControl(open_button, Qt::AlignRight);
    group->addSettingCard(add_card);
    return group;
}

// The engine, the options every provider shares, and read aloud.
QWidget *build_general_tab(const std::shared_ptr<TranslationPage> &page, const QString &current_provider) {
    SettingsDialog *dialog = page->dialog;
    auto *config = dialog->config_manager;
    auto *general = new QWidget;
    set_own_style(general, "background: transparent;");
    auto *layout = new QVBoxLayout(general);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(dialog_scaled(14));

    auto *group = new SettingsCardGroup(dialog->tr_text("Translation Options"), general, "translate");
    page->options_group = group;
    dialog->set_attr("translation_options_group", group);
    // The description tells whether the engine is ready and leads to its card, as does the button.
    auto *engine_card = new SettingCard(FluentIcon::Application, dialog->tr_text("Translation Engine"), {}, group);
    engine_card->contentLabel->setTextFormat(Qt::RichText);
    page->engine_card = engine_card;
    dialog->set_attr("translation_engine_card", engine_card);
    auto *provider_combo = new ComboBox(engine_card);
    for (const auto &metadata : page->providers)
        // Brand names have no translation entry and come back unchanged; only names such as "Custom" change.
        provider_combo->addItem(dialog->tr_text(metadata.display_name.toUtf8().constData()), metadata.provider_id);
    provider_combo->setCurrentIndex(std::max(0, provider_combo->findData(current_provider)));
    page->engine_combo = provider_combo;
    dialog->set_attr("translation_provider_combo", provider_combo);
    engine_card->addControl(provider_combo);
    const QString configure_text = dialog->tr_text("Configure API");
    auto *configure = new PushButton({}, engine_card, FluentIcon::Setting);
    configure->setFixedSize(dialog_scaled(32), dialog_scaled(32));
    configure->setIconSize(QSize(dialog_scaled(16), dialog_scaled(16)));
    configure->setToolTip(configure_text);
    configure->setAccessibleName(configure_text);
    dialog->set_attr("translation_configure_button", configure);
    engine_card->addControl(configure, Qt::AlignRight);
    const std::weak_ptr<TranslationPage> weak = page;
    const auto open_current = [weak] {
        if (const auto page = weak.lock()) open_provider(*page, current_engine(*page));
    };
    QObject::connect(configure, &QPushButton::clicked, configure, open_current);
    QObject::connect(engine_card->contentLabel, &QLabel::linkActivated, engine_card, open_current);
    group->addSettingCard(engine_card);

    auto *lang_card = new SettingCard(FluentIcon::Language, dialog->tr_text("Target Language"), {}, group);
    auto *target_combo = new ComboBox(lang_card);
    std::vector<std::pair<QString, QString>> lang_options{{"", dialog->tr_text("Auto (System)")}};
    lang_options.insert(lang_options.end(), translation_languages().begin(), translation_languages().end());
    const QString current_lang = config->get_saved_translation_target_lang();
    int current_index = 0;
    for (int i = 0; i < static_cast<int>(lang_options.size()); ++i) {
        target_combo->addItem(lang_options[i].second, lang_options[i].first);
        if (lang_options[i].first == current_lang) current_index = i;
    }
    target_combo->setCurrentIndex(current_index);
    dialog->set_attr("translation_target_combo", target_combo);
    lang_card->addControl(target_combo);
    group->addSettingCard(lang_card);

    auto *mode_card = new SettingCard(FluentIcon::Camera, dialog->tr_text("Screenshot Translation"),
                                      dialog->tr_text("What the translate button on the capture and long screenshot toolbars does"), group);
    auto *mode_combo = new ComboBox(mode_card);
    // The labels follow SCREENSHOT_TRANSLATE_MODES.
    const char *mode_labels[] = {"Replace the original text", "Side-by-side window"};
    for (int i = 0; i < SCREENSHOT_TRANSLATE_MODES.size(); ++i) mode_combo->addItem(dialog->tr_text(mode_labels[i]), SCREENSHOT_TRANSLATE_MODES[i]);
    mode_combo->setCurrentIndex(SCREENSHOT_TRANSLATE_MODES.indexOf(config->get_screenshot_translate_mode()));
    dialog->set_attr("screenshot_translate_mode_combo", mode_combo);
    mode_card->addControl(mode_combo);
    group->addSettingCard(mode_card);

    auto *auto_close_card = new SwitchSettingCard(FluentIcon::Hide, dialog->tr_text("Close When Clicking Outside"),
                                                  dialog->tr_text("The translation popup opened by the shortcut closes when you click outside it or switch windows"),
                                                  group, "translate");
    auto_close_card->setChecked(config->get_translation_popup_auto_close());
    dialog->set_attr("translation_auto_close_toggle", auto_close_card);
    group->addSettingCard(auto_close_card);
    layout->addWidget(group);

    layout->addWidget(build_speech_group(dialog, general, {}));
    return general;
}

// The providers as chips, translation APIs before language models, and the selected one's card.
QWidget *build_api_tab(const std::shared_ptr<TranslationPage> &page, const QString &current_provider) {
    SettingsDialog *dialog = page->dialog;
    auto *api = new QWidget;
    set_own_style(api, "background: transparent;");
    auto *layout = new QVBoxLayout(api);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(dialog_scaled(14));

    auto *picker = new ProviderPicker(api);
    page->picker = picker;
    dialog->set_attr("translation_provider_picker", picker);
    std::vector<std::pair<QString, QString>> services, models;
    for (const auto &metadata : page->providers)
        (metadata.language_model ? models : services).emplace_back(metadata.provider_id, dialog->tr_text(metadata.display_name.toUtf8().constData()));
    picker->add_group(dialog->tr_text("Translation API"), services);
    picker->add_group(dialog->tr_text("Large Language Models"), models);
    layout->addWidget(picker);

    auto *cards = new PageSwitcher(api);
    QStringList ids;
    dialog->provider_sections.clear();
    dialog->provider_status_labels.clear();
    for (const auto &metadata : page->providers) {
        QWidget *card = build_provider_card(page, metadata, cards);
        cards->add_page(card);
        ids << metadata.provider_id;
        dialog->provider_sections[metadata.provider_id] = card;
    }
    layout->addWidget(cards);
    picker->on_select = [cards, ids](const QString &id) { cards->set_current(ids.indexOf(id)); };
    const QPointer<ProviderPicker> guarded(picker);
    link_pages(
        cards, [cards] { return cards->pages(); }, ids, [guarded](const QString &id) { return guarded ? guarded->itemText(id) : QString(); },
        [guarded](const QString &id) {
            if (guarded) guarded->setCurrentItem(id);
        });
    picker->setCurrentItem(ids.contains(current_provider) ? current_provider : ids.value(0));
    return api;
}
}

QString quota_usage_text(SettingsDialog *dialog, const QString &provider_id, std::optional<qint64> usage) {
    const auto use = quota_use(dialog, provider_id, usage);
    if (!use) return {};
    const auto text = use->guarded ? dialog->tr_text("Used this month: %1 / %2 characters (quota guard on)") : dialog->tr_text("Used this month: %1 / %2 characters (quota guard off)");
    // Grouped with commas whatever the interface language.
    const QLocale grouping(QLocale::English, QLocale::UnitedStates);
    return text.arg(grouping.toString(use->used), grouping.toString(use->limit));
}

void show_translation_provider(SettingsDialog *dialog, const QString &provider_id) {
    auto *tabs = dialog->attr<SegmentedWidget>("translation_tabs");
    auto *picker = dynamic_cast<ProviderPicker *>(dialog->attr_widget("translation_provider_picker"));
    if (!tabs || !picker) return;
    tabs->setCurrentItem(API_TAB);
    picker->setCurrentItem(provider_id);
    if (auto *scroll = qobject_cast<QScrollArea *>(tabs->parentWidget())) scroll->verticalScrollBar()->setValue(0);
}

QStringList speech_voice_choices(const SettingsDialog *dialog) {
    QStringList result = dialog->speech_voice_preferences;
    result.sort();
    return result;
}

SettingsCardGroup *rebuild_speech_group(SettingsDialog *dialog, std::vector<VoiceInfo> voices) {
    auto *old_group = dialog->attr<SettingsCardGroup>("speech_group");
    if (!old_group) return nullptr;
    if (voice_tokens(voices) == old_group->property("voice_tokens").toStringList()) return nullptr;
    QWidget *page = old_group->parentWidget();
    auto *layout = qobject_cast<QBoxLayout *>(page->layout());
    if (!layout) return nullptr;
    // Choices not saved yet survive the rebuild.
    const QStringList choices = speech_voice_choices(dialog);
    const SpeechStyle style = pending_speech_style(dialog);
    const int index = layout->indexOf(old_group);
    delete old_group;
    auto &controls = dialog->behavior_controls;
    controls.erase(std::remove_if(controls.begin(), controls.end(), [](const auto &entry) { return !entry.second; }), controls.end());
    auto *group = build_speech_group(dialog, page, std::move(voices));
    layout->insertWidget(index, group);
    select_speech_voices(dialog, choices);
    for (const auto &[key, value] : {std::pair<const char *, int>{"speech_rate", style.rate}, {"speech_volume", style.volume}})
        if (auto *combo = qobject_cast<QComboBox *>(dialog->behavior_control(key)); combo && combo->findData(value) >= 0) combo->setCurrentIndex(combo->findData(value));
    return group;
}

void select_speech_voices(SettingsDialog *dialog, const QStringList &saved) {
    dialog->speech_voice_preferences = saved;
    for (const auto &[key, combo] : dialog->speech_voice_combos) {
        if (!combo) continue;
        QString token;
        for (const auto &entry : saved)
            if (entry.section('=', 0, 0) == key) token = entry.section('=', 1);
        const QSignalBlocker blocker(combo);
        int index = 0;
        for (int i = 1; i < combo->count(); ++i)
            if (combo->itemData(i).toString().compare(token, Qt::CaseInsensitive) == 0) index = i;
        combo->setCurrentIndex(index);
    }
}

void reset_speech_voices(SettingsDialog *dialog) {
    auto choices = speech_voice_choices(dialog);
    for (const auto &[key, combo] : dialog->speech_voice_combos)
        choices.removeIf([&key](const QString &entry) { return entry.section('=', 0, 0) == key; });
    select_speech_voices(dialog, choices);
}

QWidget *create_translation_page(SettingsDialog *dialog) {
    auto *config = dialog->config_manager;
    const auto service = create_default_translation_service();
    dialog->translation_registry = service->registry;
    dialog->provider_field_widgets.emplace();
    auto page = std::make_shared<TranslationPage>();
    page->dialog = dialog;
    page->providers = service->registry->available_providers();
    const QString current_provider = config->get_translation_provider();

    auto *tabs = new SegmentedWidget;
    tabs->setLarge(true);
    tabs->setFixedHeight(dialog_scaled(TAB_HEIGHT));
    tabs->setIndicatorColor(ACCENT, ACCENT);
    tabs->addItem(GENERAL_TAB, dialog->tr_text("General"));
    tabs->addItem(API_TAB, dialog->tr_text("API Services"));
    page->tabs = tabs;
    dialog->set_attr("translation_tabs", tabs);

    // The right inset keeps the cards, and the tab row above them, off the scroll bar.
    const int right_inset = dialog_scaled(10);
    auto *scroll = new TabbedScrollArea(dialog->content_stack, tabs, right_inset);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet("QScrollArea { border: none; background: transparent; }");
    page->scroll = scroll;
    auto *content = new QWidget;
    set_own_style(content, "background: transparent;");
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, right_inset, 0);
    layout->setSpacing(0);

    auto *switcher = new PageSwitcher(content);
    switcher->add_page(build_general_tab(page, current_provider));
    switcher->add_page(build_api_tab(page, current_provider));
    layout->addWidget(switcher);
    layout->addStretch();
    const QStringList routes{GENERAL_TAB, API_TAB};
    // Each tab opens at its top.
    QObject::connect(tabs, &SegmentedWidget::currentItemChanged, switcher, [switcher, scroll, routes](const QString &route) {
        switcher->set_current(routes.indexOf(route));
        scroll->verticalScrollBar()->setValue(0);
    });
    const QPointer<SegmentedWidget> guarded_tabs(tabs);
    link_pages(
        switcher, [switcher] { return switcher->pages(); }, routes, [guarded_tabs](const QString &route) { return guarded_tabs ? guarded_tabs->itemText(route) : QString(); },
        [guarded_tabs](const QString &route) {
            if (guarded_tabs) guarded_tabs->setCurrentItem(route);
        });
    tabs->setCurrentItem(GENERAL_TAB);

    for (const auto &metadata : page->providers) page->views[metadata.provider_id].configured = pending_configured(dialog, metadata.provider_id);
    apply_all(*page);
    QObject::connect(page->engine_combo.data(), &QComboBox::currentIndexChanged, page->engine_combo.data(), [page] { apply_all(*page); });

    const auto categories = SpeechService::voice_categories();
    dialog->add_external_refresh([categories] { return installed_speech_voices(categories); }, [dialog](const std::vector<VoiceInfo> &voices) {
        if (auto *group = rebuild_speech_group(dialog, voices)) dialog->adopt_controls(group);
    });
    const auto file = config->settings().fileName();
    const auto format = config->settings().format();
    const auto providers = page->providers;
    const std::weak_ptr<TranslationPage> weak = page;
    dialog->add_external_refresh([file, format, providers] {
        QSettings settings(file, format);
        QMap<QString, qint64> usage;
        const auto prefix = "translation/usage/" + QDate::currentDate().toString("yyyyMM") + "/";
        for (const auto &provider : providers) usage[provider.provider_id] = settings.value(prefix + provider.provider_id, 0).toLongLong();
        return usage;
    }, [dialog, weak](const QMap<QString, qint64> &usage) {
        dialog->translation_usage = usage;
        if (const auto page = weak.lock()) apply_all(*page);
    });

    scroll->setWidget(content);
    return scroll;
}
}

#include "page_translation.moc"
