#include "clipboard/ui/forms/group_icon_picker.h"
#include "clipboard/services/group_service.h"
#include "clipboard/ui/dialogs/manage_dialog.h"
#include "clipboard/ui/forms/form_widgets.h"
#include "clipboard/ui/group_kind_style.h"
#include "clipboard/ui/layout_scale.h"
#include "clipboard/ui/resources/emoji_data.h"
#include "core/py_text.h"
#include "core/ui_theme.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/theme.h"
#include "ui/settings_ui/components.h"
#include <QElapsedTimer>
#include <QFrame>
#include <QHBoxLayout>
#include <QLayout>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QTextBoundaryFinder>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

namespace jietuba::clipboard {
namespace {
// Keep emoji cells fixed in size and wrap them to the current viewport width.
class EmojiGridLayout : public QLayout {
public:
    explicit EmojiGridLayout(QWidget *parent, int total_count) : QLayout(parent), cell_size_(scale_ui(36)), total_count_(total_count) {
        setSpacing(scale_ui(2));
        const int margin = scale_ui(2);
        setContentsMargins(margin, margin, margin, margin);
    }

    ~EmojiGridLayout() override {
        while (auto *item = takeAt(0)) delete item;
    }

    void addItem(QLayoutItem *item) override { items_.append(item); invalidate(); }
    int count() const override { return static_cast<int>(items_.size()); }
    QLayoutItem *itemAt(int index) const override { return items_.value(index, nullptr); }
    QLayoutItem *takeAt(int index) override {
        if (index < 0 || index >= count()) return nullptr;
        auto *item = items_.takeAt(index);
        invalidate();
        return item;
    }
    Qt::Orientations expandingDirections() const override { return Qt::Horizontal | Qt::Vertical; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override {
        const int cols = columns(width);
        const int rows = (total_count_ + cols - 1) / cols;
        const auto margins = contentsMargins();
        return margins.top() + margins.bottom() + rows * cell_size_ + std::max(0, rows - 1) * spacing();
    }
    QSize minimumSize() const override {
        const auto margins = contentsMargins();
        return {cell_size_ + margins.left() + margins.right(), cell_size_ + margins.top() + margins.bottom()};
    }
    QSize sizeHint() const override { return minimumSize(); }
    void setGeometry(const QRect &rect) override {
        QLayout::setGeometry(rect);
        const int cols = columns(rect.width());
        const auto margins = contentsMargins();
        const int step = cell_size_ + spacing();
        for (int i = 0; i < count(); ++i)
            items_[i]->setGeometry(QRect(rect.x() + margins.left() + i % cols * step,
                                        rect.y() + margins.top() + i / cols * step, cell_size_, cell_size_));
    }

private:
    int columns(int width) const {
        const auto margins = contentsMargins();
        const int available = width - margins.left() - margins.right();
        return std::max(1, (available + spacing()) / (cell_size_ + spacing()));
    }
    QList<QLayoutItem *> items_;
    int cell_size_;
    int total_count_;
};

// Reserve the full grid height, but yield to input and painting between small batches.
class EmojiGrid : public QWidget {
public:
    EmojiGrid(ManageDialog *dialog, const QStringList &emojis)
        : dialog_(dialog), form_token_(dialog->detail_form_token), emojis_(emojis),
          grid_(new EmojiGridLayout(this, static_cast<int>(emojis.size()))), btn_style_(emoji_btn_style(dialog)) {
        set_own_style(this, "background: transparent;");
        timer_ = new QTimer(this);
        timer_->setSingleShot(true);
        timer_->setInterval(1);
        QObject::connect(timer_, &QTimer::timeout, this, [this] { load_batch(); });
    }

    void start_loading() { timer_->start(); }

private:
    void load_batch() {
        if (!dialog_ || form_token_ != dialog_->detail_form_token || !dialog_->emoji_scroll || dialog_->emoji_scroll->widget() != this) return;
        QElapsedTimer budget;
        budget.start();
        const int end = std::min(grid_->count() + 12, static_cast<int>(emojis_.size()));
        for (int index = grid_->count(); index < end; ++index) {
            const QString icon = emojis_[index];
            auto *btn = new QPushButton(icon, this);
            btn->setFixedSize(scale_ui(36), scale_ui(36));
            btn->setAutoDefault(false);
            btn->setCursor(Qt::PointingHandCursor);
            btn->setStyleSheet(btn_style_);
            QObject::connect(btn, &QPushButton::clicked, dialog_, [dialog = dialog_, icon, token = form_token_] {
                if (dialog) dialog->on_preset_icon_clicked_checked(icon, token);
            });
            grid_->addWidget(btn);
            btn->show();
            if (budget.elapsed() >= 4) break;
        }
        if (grid_->count() < emojis_.size()) timer_->start();
    }

    QPointer<ManageDialog> dialog_;
    int form_token_;
    QStringList emojis_;
    EmojiGridLayout *grid_;
    QString btn_style_;
    QTimer *timer_;
};
}

void create_group_icon_picker(ManageDialog *dialog, const QString &current_icon) {
    // Recommended icons, manual input, category tabs and the scrolling grid share one card; the preview tile is in
    // the name row (group_form builds it).
    const int form_token = dialog->detail_form_token;
    auto *panel = new QFrame;
    panel->setObjectName("ManageSectionCard");
    auto *panel_layout = new QVBoxLayout(panel);
    panel_layout->setContentsMargins(scale_ui(12), scale_ui(8), scale_ui(12), scale_ui(6));
    panel_layout->setSpacing(scale_ui(6));

    auto *top_row = new QHBoxLayout;
    top_row->setSpacing(scale_ui(2));
    auto *caption = new QLabel(ManageDialog::tr_text("Recommended"));
    apply_theme_text_style(caption, 12, false, {}, true);
    top_row->addWidget(caption);
    top_row->addSpacing(scale_ui(6));
    auto *recommended = new RecommendedRow;
    dialog->recommended_row = recommended;
    top_row->addWidget(recommended, 1);

    auto *icon_input = new LineEdit;
    dialog->icon_input = icon_input;
    icon_input->setPlaceholderText(ManageDialog::tr_text("Enter or paste emoji..."));
    // No length cap: on_icon_input_changed keeps one character, and 👩‍❤️‍💋‍👨 alone is 11 UTF-16 units.
    icon_input->setText(current_icon);
    icon_input->setFixedWidth(scale_ui(150));
    QObject::connect(icon_input, &QLineEdit::textChanged, dialog, [dialog, form_token](const QString &text) { dialog->on_icon_input_changed_checked(text, form_token); });
    top_row->addWidget(icon_input);
    panel_layout->addLayout(top_row);

    const auto [group_order, emoji_groups] = get_emoji_groups();

    auto *tab_bar = new QHBoxLayout;
    tab_bar->setSpacing(0);
    tab_bar->setContentsMargins(0, 0, 0, 0);
    dialog->emoji_tab_buttons.clear();
    dialog->emoji_group_order = group_order;
    dialog->emoji_groups = emoji_groups;

    for (int idx = 0; idx < group_order.size(); ++idx) {
        auto *btn = new QPushButton(get_group_icon(group_order[idx]));
        btn->setFixedSize(scale_x(36), scale_y(30));
        btn->setCursor(Qt::PointingHandCursor);
        btn->setCheckable(true);
        btn->setStyleSheet(emoji_tab_style(false, dialog));
        QObject::connect(btn, &QPushButton::clicked, dialog, [dialog, idx, form_token] { dialog->switch_emoji_group_checked(idx, form_token); });
        tab_bar->addWidget(btn);
        dialog->emoji_tab_buttons.append(btn);
    }
    tab_bar->addStretch();
    panel_layout->addLayout(tab_bar);

    auto *scroll = new QScrollArea;
    dialog->emoji_scroll = scroll;
    scroll->setWidgetResizable(true);
    scroll->setMinimumHeight(scale_y(110));
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // The scroll bar style cascades from the manage window's scrollbar_qss and follows the theme.
    scroll->setStyleSheet("QScrollArea { border: none; background: transparent; }");
    panel_layout->addWidget(scroll, 1);
    // Whatever height the form has left goes to the icons: a taller window shows more rows.
    dialog->detail_layout->addWidget(panel, 1);

    dialog->emoji_current_idx = 0;
    dialog->switch_emoji_group_checked(0, form_token);
    refresh_group_icon_kind(dialog);
}

RecommendedRow::RecommendedRow() {
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setFixedHeight(scale_ui(34));
    set_own_style(this, "background: transparent;");
}

void RecommendedRow::set_buttons(const QList<QPushButton *> &buttons) {
    for (auto *old : buttons_) {
        old->hide();
        old->setParent(nullptr);
        old->deleteLater();
    }
    buttons_ = buttons;
    arrange();
}

void RecommendedRow::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    arrange();
}

void RecommendedRow::arrange() {
    const int step = scale_ui(36);
    for (int index = 0; index < buttons_.size(); ++index) {
        buttons_[index]->move(index * step, 0);
        buttons_[index]->setVisible(index * step + buttons_[index]->width() <= width());
    }
}

IconPreview::IconPreview(const QString &icon, const QString &kind) : QLabel(icon), kind_(kind.isEmpty() ? KIND_NORMAL() : kind) {
    setFixedSize(scale_ui(52), scale_ui(52));
    setAlignment(Qt::AlignCenter);
    _apply_theme();
}

void IconPreview::set_kind(const QString &kind) {
    if (kind == kind_) return;
    kind_ = kind;
    _apply_theme();
}

void IconPreview::_apply_theme() {
    const auto &tokens = ui_tokens(this);
    const QColor tint = kind_tint(kind_, 0.14, tokens);
    const QString background = QString("rgba(%1, %2, %3, %4)").arg(tint.red()).arg(tint.green()).arg(tint.blue()).arg(tint.alphaF(), 0, 'f', 2);
    setStyleSheet(QString("QLabel { font-size: %1px; background: %2; border: 1px solid %3; border-radius: %4px; }").arg(scale_ui(28)).arg(background, tokens.border).arg(scale_ui(14)));
}

void refresh_group_icon_kind(ManageDialog *dialog) {
    auto *preview = qobject_cast<IconPreview *>(dialog->icon_preview.data());
    auto *row = qobject_cast<RecommendedRow *>(dialog->recommended_row.data());
    if (!preview || !row) return;
    const int group_type = dialog->selected_group_type();
    preview->set_kind(group_kind(group_type));
    const int form_token = dialog->detail_form_token;
    const QString style = emoji_btn_style(dialog);
    QList<QPushButton *> buttons;
    for (const QString &icon : get_recommended_group_icons(group_type, dialog->selected_filter_rule())) {
        auto *btn = new QPushButton(icon, row);
        btn->setFixedSize(scale_ui(34), scale_ui(34));
        btn->setAutoDefault(false);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(style);
        QObject::connect(btn, &QPushButton::clicked, dialog, [dialog, icon, form_token] { dialog->on_preset_icon_clicked_checked(icon, form_token); });
        buttons.append(btn);
    }
    row->set_buttons(buttons);
}

QString emoji_tab_style(bool active, const QWidget *dialog) {
    const auto &tokens = ui_tokens(dialog);
    if (active)
        return QString(R"(
            QPushButton {
                color: %1;
                background: %2; border: none;
                border-bottom: 2px solid %3;
                border-radius: 0px; font-size: %4px;
                padding: %5px 0px;
            }
        )")
            .arg(tokens.text, tokens.accent_soft, tokens.accent)
            .arg(scale_ui(18))
            .arg(scale_y(2));
    return QString(R"(
        QPushButton {
            color: %1;
            background: transparent; border: none;
            border-bottom: 2px solid transparent;
            border-radius: 0px; font-size: %2px;
            padding: %3px 0px;
        }
        QPushButton:hover { background: %4; }
    )")
        .arg(tokens.text)
        .arg(scale_ui(18))
        .arg(scale_y(2))
        .arg(tokens.surface_hover);
}

QString emoji_btn_style(const QWidget *dialog) {
    const auto &tokens = ui_tokens(dialog);
    return QString(R"(
        QPushButton {
            background: transparent; border: none;
            border-radius: %1px; font-size: %2px; padding: 0px;
        }
        QPushButton:hover {
            background: %3;
        }
        QPushButton:pressed {
            background: %4;
        }
    )")
        .arg(scale_ui(4))
        .arg(scale_ui(22))
        .arg(tokens.accent_soft, tokens.surface_hover);
}

void switch_emoji_group(ManageDialog *dialog, int group_idx) {
    dialog->emoji_current_idx = group_idx;
    for (int i = 0; i < dialog->emoji_tab_buttons.size(); ++i) {
        dialog->emoji_tab_buttons[i]->setChecked(i == group_idx);
        dialog->emoji_tab_buttons[i]->setStyleSheet(emoji_tab_style(i == group_idx, dialog));
    }
    const QStringList emojis = dialog->emoji_groups.value(dialog->emoji_group_order.value(group_idx));

    auto *container = new EmojiGrid(dialog, emojis);
    dialog->emoji_scroll->setWidget(container);
    container->start_loading();
}

QString first_icon_cluster(const QString &text) {
    const QString rest = py_lstrip(text);
    if (rest.isEmpty()) return {};
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, rest);
    const qsizetype end = finder.toNextBoundary();
    return end > 0 ? rest.left(end) : rest;
}

void on_icon_input_changed(ManageDialog *dialog, const QString &text) {
    if (py_strip(text).isEmpty()) {
        dialog->icon_preview->setText(QString::fromUtf8("📁"));
        return;
    }
    const QString first_char = first_icon_cluster(text);
    if (first_char.isEmpty()) {
        dialog->icon_preview->setText(QString::fromUtf8("📁"));
        return;
    }
    if (py_len(py_strip(text)) > py_len(first_char)) {
        dialog->icon_input->blockSignals(true);
        dialog->icon_input->setText(first_char);
        dialog->icon_input->blockSignals(false);
    }
    dialog->icon_preview->setText(first_char);
}

void on_preset_icon_clicked(ManageDialog *dialog, const QString &icon) {
    dialog->icon_input->setText(icon);
    dialog->icon_preview->setText(icon);
}
}
