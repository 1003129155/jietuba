#include "ui/settings_ui/search.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "ui/fluent_lite/cards.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/labels.h"
#include "ui/fluent_lite/segmented.h"
#include "ui/fluent_lite/theme.h"
#include "ui/settings_ui/components.h"
#include "ui/settings_ui/dialog.h"
#include <QApplication>
#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTimer>
#include <QToolButton>
#include <QVariantAnimation>
#include <algorithm>

namespace jietuba {
namespace {
const char *const ROW_TITLE = "_search_title";
const char *const ROW_DESCRIPTION = "_search_description";
const char *const STACK_TABS = "_search_tabs";
const char *const STACK_ROUTES = "_search_routes";

// Result list geometry at 100%.
constexpr int POPUP_WIDTH = 480;
constexpr int POPUP_MAX_HEIGHT = 440;
constexpr int ROW_HEIGHT = 56;
constexpr int SHORT_ROW_HEIGHT = 40;
constexpr int FLASH_HOLD_MS = 1100;
constexpr int FLASH_FADE_MS = 400;
constexpr int FLASH_OUTSET = 8;

enum Role { TitleRole = Qt::UserRole + 1, PathRole, DescriptionRole, ToneRole };

QString normalized(const QString &text) {
    static const QRegularExpression spaces("\\s+");
    QString value = text.normalized(QString::NormalizationForm_KC).toCaseFolded();
    value.replace(spaces, " ");
    return value.trimmed();
}

// Card descriptions may be rich text (links, line breaks).
QString plain_text(const QString &text) {
    const QString plain = Qt::mightBeRichText(text) ? QTextDocumentFragment::fromHtml(text).toPlainText() : text;
    return plain.simplified();
}

QString label_text(const QLabel *label) { return label ? plain_text(label->text()) : QString(); }

QString join_path(const QStringList &parts) {
    QStringList shown;
    for (const auto &part : parts)
        if (!part.isEmpty()) shown.append(part);
    return shown.join(" / ");
}

// exact > whole-field prefix > word prefix > anywhere.
int field_score(const QString &token, const QString &field, int exact, int prefix, int word, int contains) {
    if (field.isEmpty()) return 0;
    if (field == token) return exact;
    if (field.startsWith(token)) return prefix;
    int at = field.indexOf(token);
    if (at < 0) return 0;
    for (; at >= 0; at = field.indexOf(token, at + 1))
        if (!field.at(at - 1).isLetterOrNumber()) return word;
    return contains;
}

int kind_rank(SettingsSearchEntry::Kind kind) {
    switch (kind) {
    case SettingsSearchEntry::Kind::Page: return 0;
    case SettingsSearchEntry::Kind::Group: return 1;
    case SettingsSearchEntry::Kind::Item: return 2;
    }
    return 2;
}

// Hidden by code, not merely because an ancestor is.
bool hidden_on_purpose(const QWidget *widget) { return widget->isHidden() && widget->testAttribute(Qt::WA_WState_ExplicitShowHide); }

SegmentedWidget *stack_tabs(const QWidget *stack) { return qobject_cast<SegmentedWidget *>(stack->property(STACK_TABS).value<QObject *>()); }

void add_entry(std::vector<SettingsSearchEntry> &out, const SettingsSearchEntry &page_entry, SettingsSearchEntry::Kind kind, const QString &title,
               const QString &description, const QStringList &path, QWidget *target, const QStringList &options = {}) {
    if (title.isEmpty()) return;
    SettingsSearchEntry entry;
    entry.kind = kind;
    entry.stack_index = page_entry.stack_index;
    entry.route_key = page_entry.route_key;
    entry.tone = page_entry.tone;
    entry.title = title;
    entry.description = description;
    entry.path = join_path(path);
    entry.options = options;
    entry.target = target;
    out.push_back(std::move(entry));
}

QStringList combo_options(const QWidget *container) {
    QStringList options;
    for (auto *combo : container->findChildren<QComboBox *>())
        for (int i = 0; i < combo->count(); ++i)
            if (!combo->itemText(i).isEmpty()) options.append(combo->itemText(i));
    return options;
}

void walk(QWidget *widget, const SettingsSearchEntry &page_entry, const QString &group, const QString &tab, std::vector<SettingsSearchEntry> &out);

void visit(QWidget *child, const SettingsSearchEntry &page_entry, const QString &group, const QString &tab, std::vector<SettingsSearchEntry> &out) {
    using Kind = SettingsSearchEntry::Kind;
    if (auto *card_group = qobject_cast<SettingCardGroup *>(child)) {
        const QString title = label_text(card_group->titleLabel);
        add_entry(out, page_entry, Kind::Group, title, {}, {page_entry.title, group, tab}, card_group);
        walk(card_group, page_entry, title.isEmpty() ? group : title, tab, out);
    } else if (auto *section = qobject_cast<SectionCard *>(child)) {
        const QString title = label_text(section->titleLabel);
        add_entry(out, page_entry, Kind::Group, title, label_text(section->captionLabel), {page_entry.title, group, tab}, section);
        walk(section, page_entry, title.isEmpty() ? group : title, tab, out);
    } else if (auto *card = qobject_cast<SettingCard *>(child)) {
        add_entry(out, page_entry, Kind::Item, label_text(card->titleLabel), label_text(card->contentLabel), {page_entry.title, group, tab}, card,
                  combo_options(card->controlContainer));
    } else if (auto *title = qobject_cast<QLabel *>(child->property(ROW_TITLE).value<QObject *>())) {
        add_entry(out, page_entry, Kind::Item, label_text(title), label_text(qobject_cast<QLabel *>(child->property(ROW_DESCRIPTION).value<QObject *>())),
                  {page_entry.title, group, tab}, child);
    } else {
        walk(child, page_entry, group, tab, out);
    }
}

void walk(QWidget *widget, const SettingsSearchEntry &page_entry, const QString &group, const QString &tab, std::vector<SettingsSearchEntry> &out) {
    auto *stack = qobject_cast<QStackedWidget *>(widget);
    if (SegmentedWidget *tabs = stack ? stack_tabs(stack) : nullptr) {
        // Pages of other tabs are hidden by the stack and reachable through their tab. In tab
        // order: showing a page raises it, which reorders the children.
        const QStringList routes = stack->property(STACK_ROUTES).toStringList();
        for (int index = 0; index < stack->count() && index < routes.size(); ++index) visit(stack->widget(index), page_entry, group, tabs->itemText(routes[index]), out);
        return;
    }
    for (QObject *object : widget->children()) {
        auto *child = qobject_cast<QWidget *>(object);
        if (child && !child->isWindow() && !hidden_on_purpose(child)) visit(child, page_entry, group, tab, out);
    }
}

QFont result_font(int pixel_size, QFont::Weight weight) {
    QFont font;
    font.setFamilies(FONT_FAMILIES());
    font.setPixelSize(dialog_scaled(pixel_size));
    font.setWeight(weight);
    return font;
}
QFont title_font() { return result_font(14, QFont::DemiBold); }
QFont small_font() { return result_font(12, QFont::Normal); }

// Row geometry at 100%: the highlight's inset in the list, the text's inside the highlight.
constexpr int ROW_INSET = 2;
constexpr int ROW_PADDING = 12;
constexpr int TAG_GAP = 12;
constexpr int TAG_PADDING = 6;
constexpr int TAG_MIN = 48;

// Two lines: the title with its location tag on the right, then the description.
class ResultDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const override {
        return {0, dialog_scaled(index.data(DescriptionRole).toString().isEmpty() ? SHORT_ROW_HEIGHT : ROW_HEIGHT)};
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const auto &t = get_ui_theme()->tokens();
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const int inset = dialog_scaled(ROW_INSET);
        const QRect row = option.rect.adjusted(inset, dialog_scaled(1), -inset, -dialog_scaled(1));
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        if (selected || hovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(css_color(selected ? t.accent_soft : t.popup_hover));
            painter->drawRoundedRect(row, dialog_scaled(8), dialog_scaled(8));
        }
        const int padding = dialog_scaled(ROW_PADDING);
        const QRect content = row.adjusted(padding, dialog_scaled(8), -padding, -dialog_scaled(8));
        const QFont big = title_font(), small = small_font();
        const QFontMetrics title_metrics(big), small_metrics(small);

        const QString title = index.data(TitleRole).toString();
        const QString path = index.data(PathRole).toString();
        const QString description = index.data(DescriptionRole).toString();
        const int line_height = std::max(title_metrics.height(), small_metrics.height() + dialog_scaled(6));
        const int top = description.isEmpty() ? content.top() + (content.height() - line_height) / 2 : content.top();
        const auto widths = search_row_widths(option.rect.width(), title, path);

        painter->setFont(big);
        painter->setPen(css_color(t.text));
        painter->drawText(QRect(content.left(), top, widths.title_room, line_height), Qt::AlignLeft | Qt::AlignVCenter,
                          title_metrics.elidedText(title, Qt::ElideRight, widths.title_room));
        if (widths.tag > 0) {
            const auto &tone = feature_tone(index.data(ToneRole).toString());
            const bool dark = t.is_dark();
            const int tag_height = small_metrics.height() + dialog_scaled(4);
            const QRect tag(content.right() - widths.tag + 1, top + (line_height - tag_height) / 2, widths.tag, tag_height);
            painter->setPen(Qt::NoPen);
            painter->setBrush(css_color(dark ? tone.dark_bg : tone.light_bg));
            painter->drawRoundedRect(tag, dialog_scaled(5), dialog_scaled(5));
            painter->setFont(small);
            painter->setPen(css_color(dark ? tone.dark_fg : tone.light_fg));
            const int tag_padding = dialog_scaled(TAG_PADDING);
            painter->drawText(tag.adjusted(tag_padding, 0, -tag_padding, 0), Qt::AlignCenter, widths.tag_text);
        }
        if (!description.isEmpty()) {
            painter->setFont(small);
            painter->setPen(css_color(t.text_muted));
            const QRect line(content.left(), content.bottom() - small_metrics.height() + 1, content.width(), small_metrics.height());
            painter->drawText(line, Qt::AlignLeft | Qt::AlignVCenter, small_metrics.elidedText(description, Qt::ElideRight, content.width()));
        }
        painter->restore();
    }
};

// Outlines a row after a jump, then fades. A sibling laid over the row and widened where the
// parent leaves room on both sides, so an icon at the row's edge is not touched by the outline.
class RowFlash : public QWidget {
public:
    explicit RowFlash(QWidget *target) : QWidget(target->parentWidget()), target_(target) {
        setObjectName("SettingsSearchFlash");
        setAttribute(Qt::WA_TransparentForMouseEvents);
        place();
        target->installEventFilter(this);
        connect(target, &QObject::destroyed, this, &QObject::deleteLater);
        auto *fade = new QVariantAnimation(this);
        fade->setStartValue(1.0);
        fade->setEndValue(0.0);
        fade->setDuration(FLASH_FADE_MS);
        connect(fade, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            strength_ = value.toDouble();
            update();
        });
        connect(fade, &QVariantAnimation::finished, this, &QObject::deleteLater);
        QTimer::singleShot(FLASH_HOLD_MS, fade, [fade] { fade->start(); });
        show();
    }
    QWidget *target() const { return target_; }
protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == target_ && (event->type() == QEvent::Move || event->type() == QEvent::Resize)) place();
        return false;
    }
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QColor accent = css_color(get_ui_theme()->tokens().accent);
        QColor fill = accent;
        accent.setAlphaF(strength_);
        fill.setAlphaF(0.12 * strength_);
        const int pen = std::max(2, dialog_scaled(2));
        painter.setPen(QPen(accent, pen));
        painter.setBrush(fill);
        const qreal inset = pen / 2.0;
        painter.drawRoundedRect(QRectF(rect()).adjusted(inset, inset, -inset, -inset), dialog_scaled(8), dialog_scaled(8));
    }
private:
    void place() {
        const QRect row = target_->geometry();
        const int outset = std::max(0, std::min({dialog_scaled(FLASH_OUTSET), row.left(), parentWidget()->width() - 1 - row.right()}));
        setGeometry(row.adjusted(-outset, 0, outset, 0));
        raise();
    }
    QPointer<QWidget> target_;
    double strength_ = 1.0;
};
}

class SettingsSearchPopup : public QWidget {
public:
    explicit SettingsSearchPopup(QWidget *parent) : QWidget(parent) {
        setObjectName("SettingsSearchPopup");
        auto *layout = new QVBoxLayout(this);
        const int margin = dialog_scaled(6);
        layout->setContentsMargins(margin, margin, margin, margin);
        layout->setSpacing(0);
        list = new QListWidget(this);
        list->setObjectName("SettingsSearchResults");
        // The field keeps the keyboard; the list only takes clicks.
        list->setFocusPolicy(Qt::NoFocus);
        list->setFrameShape(QFrame::NoFrame);
        list->setMouseTracking(true);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        list->setSelectionMode(QAbstractItemView::SingleSelection);
        list->setItemDelegate(new ResultDelegate(list));
        layout->addWidget(list);
        // The room around the text sits in a layout: the style sheet resets a label's own margins.
        empty_box = new QWidget(this);
        auto *empty_layout = new QVBoxLayout(empty_box);
        const int padding = dialog_scaled(14);
        empty_layout->setContentsMargins(padding, padding, padding, padding);
        empty = new QLabel(settings_tr("No matching settings"), empty_box);
        empty->setObjectName("SettingsSearchEmpty");
        empty->setAlignment(Qt::AlignCenter);
        empty->setWordWrap(true);
        empty_layout->addWidget(empty);
        layout->addWidget(empty_box);
        apply_theme();
    }
    void apply_theme() {
        list->setStyleSheet("QListWidget#SettingsSearchResults { background: transparent; border: none; outline: none; }" + scrollbar_qss(list));
        apply_theme_text_style(empty, 13, false, {}, true);
        update();
    }
    QListWidget *list;
    QWidget *empty_box;
    QLabel *empty;
protected:
    void paintEvent(QPaintEvent *) override {
        const auto &t = get_ui_theme()->tokens();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(css_color(t.border));
        painter.setBrush(css_color(t.popup_background));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), dialog_scaled(10), dialog_scaled(10));
    }
};

std::vector<SettingsSearchEntry> rank_settings_search(const std::vector<SettingsSearchEntry> &entries, const QString &query) {
    const QString whole = normalized(query);
    const QStringList tokens = whole.split(' ', Qt::SkipEmptyParts);
    if (tokens.isEmpty()) return {};
    struct Ranked {
        int score;
        int order;
    };
    std::vector<Ranked> ranked;
    for (int order = 0; order < static_cast<int>(entries.size()); ++order) {
        const auto &entry = entries[order];
        const QString title = normalized(entry.title), path = normalized(entry.path), description = normalized(entry.description);
        QStringList options;
        for (const auto &option : entry.options) options.append(normalized(option));
        int total = title == whole ? 2000 : 0;
        bool every = true;
        for (const auto &token : tokens) {
            int score = field_score(token, title, 1000, 800, 700, 600);
            for (const auto &option : options) score = std::max(score, field_score(token, option, 500, 450, 420, 400));
            score = std::max(score, field_score(token, path, 400, 350, 320, 300));
            score = std::max(score, field_score(token, description, 150, 125, 110, 100));
            if (!score) {
                every = false;
                break;
            }
            total += score;
        }
        if (every) ranked.push_back({total, order});
    }
    std::stable_sort(ranked.begin(), ranked.end(), [&entries](const Ranked &a, const Ranked &b) {
        if (a.score != b.score) return a.score > b.score;
        return kind_rank(entries[a.order].kind) < kind_rank(entries[b.order].kind);
    });
    std::vector<SettingsSearchEntry> result;
    result.reserve(ranked.size());
    for (const auto &item : ranked) result.push_back(entries[item.order]);
    return result;
}

void collect_settings_entries(QWidget *page, const SettingsSearchEntry &page_entry, std::vector<SettingsSearchEntry> &out) {
    if (page) walk(page, page_entry, {}, {}, out);
}

void mark_search_row(QWidget *row, QLabel *title, QLabel *description) {
    row->setProperty(ROW_TITLE, QVariant::fromValue<QObject *>(title));
    if (description) row->setProperty(ROW_DESCRIPTION, QVariant::fromValue<QObject *>(description));
}

void link_tabs(SegmentedWidget *tabs, QStackedWidget *stack, const QStringList &routes) {
    QObject::connect(tabs, &SegmentedWidget::currentItemChanged, stack, [stack, routes](const QString &route) { stack->setCurrentIndex(routes.indexOf(route)); });
    stack->setProperty(STACK_TABS, QVariant::fromValue<QObject *>(tabs));
    stack->setProperty(STACK_ROUTES, routes);
}

SearchRowWidths search_row_widths(int row_width, const QString &title, const QString &tag) {
    const int content = std::max(0, row_width - 2 * dialog_scaled(ROW_INSET + ROW_PADDING));
    SearchRowWidths widths;
    widths.title_needed = QFontMetrics(title_font()).horizontalAdvance(title) + 1;
    const int padding = 2 * dialog_scaled(TAG_PADDING) + 1;
    const int room = content - std::min(content, widths.title_needed) - dialog_scaled(TAG_GAP) - padding;
    if (!tag.isEmpty() && room + padding >= dialog_scaled(TAG_MIN)) {
        // Whole parts from the end, the innermost being the most telling: "… / group / tab".
        const QFontMetrics metrics(small_font());
        const QStringList parts = tag.split(" / ");
        for (int first = 0; first < parts.size() && widths.tag_text.isEmpty(); ++first) {
            const QString text = (first ? QString::fromUtf8("… / ") : QString()) + parts.mid(first).join(" / ");
            if (metrics.horizontalAdvance(text) <= room) widths.tag_text = text;
        }
        if (widths.tag_text.isEmpty()) widths.tag_text = metrics.elidedText(parts.last(), Qt::ElideRight, room);
        widths.tag = metrics.horizontalAdvance(widths.tag_text) + padding;
    }
    widths.title_room = content - (widths.tag ? widths.tag + dialog_scaled(TAG_GAP) : 0);
    return widths;
}

void reveal_settings_row(QWidget *target, QWidget *page) {
    for (QWidget *widget = target; widget && widget != page; widget = widget->parentWidget()) {
        auto *stack = qobject_cast<QStackedWidget *>(widget->parentWidget());
        SegmentedWidget *tabs = stack ? stack_tabs(stack) : nullptr;
        if (!tabs) continue;
        const QStringList routes = stack->property(STACK_ROUTES).toStringList();
        const int index = stack->indexOf(widget);
        if (index >= 0 && index < routes.size()) tabs->setCurrentItem(routes[index]);
    }
}

QWidget *flash_settings_row(QWidget *target) { return target && target->parentWidget() ? new RowFlash(target) : nullptr; }

SettingsSearchBox::SettingsSearchBox(QWidget *parent) : QWidget(parent) {
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    edit_ = new LineEdit(this);
    edit_->setObjectName("SettingsSearchEdit");
    edit_->setPlaceholderText(settings_tr("Search settings"));
    search_icon_ = new QLabel(edit_);
    search_icon_->setAttribute(Qt::WA_TransparentForMouseEvents);
    clear_button_ = new QToolButton(edit_);
    clear_button_->setObjectName("SettingsSearchClear");
    clear_button_->setFocusPolicy(Qt::NoFocus);
    clear_button_->setCursor(Qt::ArrowCursor);
    clear_button_->hide();
    connect(clear_button_, &QToolButton::clicked, edit_, &QLineEdit::clear);
    layout->addWidget(edit_);
    setFocusProxy(edit_);
    edit_->installEventFilter(this);
    place_decorations();
    connect(edit_, &QLineEdit::textChanged, this, [this](const QString &text) {
        clear_button_->setVisible(!text.isEmpty());
        if (text.trimmed().isEmpty()) {
            hide_results();
            return;
        }
        start_building();
        show_results();
    });
    apply_theme();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { apply_theme(); });
}

SettingsSearchBox::~SettingsSearchBox() {
    if (filtering_) qApp->removeEventFilter(this);
}

QWidget *SettingsSearchBox::popup() const { return popup_; }
QListWidget *SettingsSearchBox::result_list() const { return popup_ ? popup_->list : nullptr; }
QLabel *SettingsSearchBox::empty_label() const { return popup_ ? popup_->empty : nullptr; }
bool SettingsSearchBox::results_visible() const { return popup_ && popup_->isVisible(); }

void SettingsSearchBox::apply_theme() {
    const auto &t = get_ui_theme()->tokens();
    const int icon = dialog_scaled(16);
    search_icon_->setPixmap(tinted_icon(FluentIcon::Search, t.text_muted).pixmap(QSize(icon, icon), search_icon_->devicePixelRatioF()));
    clear_button_->setIcon(tinted_icon(FluentIcon::Close, t.text_muted));
    clear_button_->setIconSize(QSize(dialog_scaled(12), dialog_scaled(12)));
    clear_button_->setStyleSheet(QString("QToolButton#SettingsSearchClear { border: none; background: transparent; border-radius: %1px; }"
                                         "QToolButton#SettingsSearchClear:hover { background: %2; }")
                                     .arg(dialog_scaled(6))
                                     .arg(t.surface_hover));
    if (popup_) popup_->apply_theme();
}

void SettingsSearchBox::place_decorations() {
    const int icon = dialog_scaled(16), button = dialog_scaled(22), gap = dialog_scaled(6);
    search_icon_->setGeometry(dialog_scaled(10), (edit_->height() - icon) / 2, icon, icon);
    clear_button_->setGeometry(edit_->width() - dialog_scaled(6) - button, (edit_->height() - button) / 2, button, button);
    edit_->setTextMargins(icon + gap, 0, button, 0);
}

void SettingsSearchBox::ensure_popup() {
    if (popup_) return;
    popup_ = new SettingsSearchPopup(window());
    popup_->hide();
    connect(popup_->list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) { activate(popup_->list->row(item)); });
}

void SettingsSearchBox::refresh() {
    if (!popup_) return;
    auto *list = popup_->list;
    // For the same query the selection follows its entry when pages built meanwhile add rows
    // above it; a new query starts at its best result.
    QPointer<QWidget> selected_target;
    QString selected_route;
    const int row = list->currentRow();
    if (edit_->text() == listed_query_ && row >= 0 && row < static_cast<int>(results_.size())) {
        selected_target = results_[row].target;
        selected_route = results_[row].route_key;
    }
    listed_query_ = edit_->text();
    results_ = entries ? rank_settings_search(entries(), listed_query_) : std::vector<SettingsSearchEntry>{};
    list->clear();
    int selected = 0;
    for (int i = 0; i < static_cast<int>(results_.size()); ++i) {
        const auto &entry = results_[i];
        auto *item = new QListWidgetItem(list);
        item->setData(TitleRole, entry.title);
        item->setData(PathRole, entry.kind == SettingsSearchEntry::Kind::Page ? settings_tr("Page") : entry.path);
        item->setData(DescriptionRole, entry.description);
        item->setData(ToneRole, entry.tone);
        item->setToolTip(entry.description);
        if (entry.target == selected_target && entry.route_key == selected_route) selected = i;
    }
    if (!results_.empty()) list->setCurrentRow(selected);
    list->setVisible(!results_.empty());
    popup_->empty_box->setVisible(results_.empty());
}

void SettingsSearchBox::show_results() {
    ensure_popup();
    refresh();
    // While pages are still being built, an empty list waits for them instead of saying nothing was found.
    waiting_ = results_.empty() && building_;
    if (waiting_) {
        popup_->hide();
        return;
    }
    place_popup();
    popup_->show();
    popup_->raise();
    if (!filtering_) {
        qApp->installEventFilter(this);
        filtering_ = true;
    }
}

void SettingsSearchBox::hide_results() {
    waiting_ = false;
    if (popup_) popup_->hide();
    if (filtering_) {
        qApp->removeEventFilter(this);
        filtering_ = false;
    }
}

void SettingsSearchBox::place_popup() {
    QWidget *host = window();
    const int edge = dialog_scaled(10);
    const QPoint origin = mapTo(host, QPoint(0, height() + dialog_scaled(4)));
    const int width = std::max(0, std::min(std::max(this->width(), dialog_scaled(POPUP_WIDTH)), host->width() - origin.x() - edge));
    const auto margins = popup_->layout()->contentsMargins();
    int content = margins.top() + margins.bottom();
    if (results_.empty()) {
        // Measured with the style sheet's font.
        popup_->empty->ensurePolished();
        content += popup_->empty_box->layout()->heightForWidth(width - margins.left() - margins.right());
    } else {
        for (int row = 0; row < popup_->list->count(); ++row) content += popup_->list->sizeHintForRow(row);
    }
    const int room = std::max(0, std::min(dialog_scaled(POPUP_MAX_HEIGHT), host->height() - origin.y() - edge));
    popup_->setGeometry(origin.x(), origin.y(), width, std::min(content, room));
}

void SettingsSearchBox::move_selection(int delta) {
    auto *list = result_list();
    if (!list || !list->count()) return;
    const int row = std::clamp(list->currentRow() + delta, 0, list->count() - 1);
    list->setCurrentRow(row);
    list->scrollToItem(list->item(row));
}

void SettingsSearchBox::activate(int row) {
    if (row < 0 || row >= static_cast<int>(results_.size())) return;
    const SettingsSearchEntry entry = results_[row];
    hide_results();
    if (activated) activated(entry);
}

void SettingsSearchBox::start_building() {
    if (building_ || all_built_ || !build_next_page) return;
    building_ = true;
    QTimer::singleShot(0, this, [this] { build_step(); });
}

void SettingsSearchBox::build_step() {
    const bool built = build_next_page && build_next_page();
    if (!built) {
        building_ = false;
        all_built_ = true;
    }
    // A list closed with Esc stays closed.
    if (results_visible() || waiting_) show_results();
    if (built) QTimer::singleShot(0, this, [this] { build_step(); });
}

bool SettingsSearchBox::eventFilter(QObject *watched, QEvent *event) {
    if (watched == edit_) {
        switch (event->type()) {
        case QEvent::KeyPress: {
            auto *key = static_cast<QKeyEvent *>(event);
            const bool open = results_visible();
            const bool has_text = !edit_->text().trimmed().isEmpty();
            switch (key->key()) {
            case Qt::Key_Down:
            case Qt::Key_Up:
            case Qt::Key_PageDown:
            case Qt::Key_PageUp: {
                if (!open) {
                    if (has_text) show_results();
                    return true;
                }
                const int page = std::max(1, popup_->list->viewport()->height() / dialog_scaled(ROW_HEIGHT));
                const int step = key->key() == Qt::Key_Down ? 1 : key->key() == Qt::Key_Up ? -1 : key->key() == Qt::Key_PageDown ? page : -page;
                move_selection(step);
                return true;
            }
            case Qt::Key_Return:
            case Qt::Key_Enter:
                // Never handed on: the dialog would press its default button.
                if (open) activate(popup_->list->currentRow());
                else if (has_text) show_results();
                return true;
            case Qt::Key_Escape:
                if (open) {
                    hide_results();
                    return true;
                }
                if (!edit_->text().isEmpty()) {
                    edit_->clear();
                    return true;
                }
                // An empty field lets Esc close the window as anywhere else.
                return false;
            default:
                return false;
            }
        }
        case QEvent::Resize:
            place_decorations();
            return false;
        case QEvent::FocusIn: {
            // Coming back to the window leaves a closed list closed.
            const auto reason = static_cast<QFocusEvent *>(event)->reason();
            if (reason == Qt::ActiveWindowFocusReason || reason == Qt::PopupFocusReason) return false;
            start_building();
            if (!edit_->text().trimmed().isEmpty()) show_results();
            return false;
        }
        case QEvent::FocusOut:
            hide_results();
            return false;
        case QEvent::MouseButtonPress:
            if (edit_->hasFocus() && !results_visible() && !edit_->text().trimmed().isEmpty()) show_results();
            return false;
        default:
            return false;
        }
    }
    // While the list is open: a press anywhere else closes it.
    if (event->type() == QEvent::MouseButtonPress && results_visible()) {
        auto *widget = qobject_cast<QWidget *>(watched);
        const bool inside = widget && (widget == popup_ || popup_->isAncestorOf(widget) || widget == edit_ || edit_->isAncestorOf(widget));
        if (widget && !inside) hide_results();
    } else if (event->type() == QEvent::Resize && watched == window() && results_visible()) {
        place_popup();
    }
    return false;
}
}
