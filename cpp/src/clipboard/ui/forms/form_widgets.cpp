#include "clipboard/ui/forms/form_widgets.h"
#include "clipboard/ui/group_kind_style.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "ui/fluent_lite/theme.h"
#include "ui/settings_ui/components.h"
#include <QEvent>
#include <QPainter>
#include <QResizeEvent>
#include <algorithm>

namespace jietuba::clipboard {
QLabel *field_label(const QString &text, bool top_gap) {
    auto *label = new QLabel(text);
    // Spacing through contentsMargins: padding in a style sheet indents the text a few pixels out of line.
    label->setContentsMargins(0, top_gap ? dialog_scaled(8) : 0, 0, 0);
    apply_theme_text_style(label, 12, false, "font-weight: 600;");
    return label;
}

namespace {
// Wrapped hint text that does not report height-for-width. The form sits in a scroll area, which on meeting
// height-for-width lays the whole form out at the preferred heights instead: the form gets taller than the
// viewport, grows a scroll bar and turns narrower. The height for the current width is worked out in
// resizeEvent and handed to the layout through sizeHint. Not setFixedHeight: QLabel floors its wrapped height
// at the minimum height, so once fixed it could only grow.
class HintLabel : public QLabel {
public:
    explicit HintLabel(const QString &text) : QLabel(text) {
        setWordWrap(true);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }
    bool hasHeightForWidth() const override { return false; }
    QSize sizeHint() const override {
        QSize hint = QLabel::sizeHint();
        if (fitted_height_ > 0) hint.setHeight(fitted_height_);
        return hint;
    }
    QSize minimumSizeHint() const override {
        QSize hint = QLabel::minimumSizeHint();
        if (fitted_height_ > 0) hint.setHeight(fitted_height_);
        return hint;
    }

protected:
    void resizeEvent(QResizeEvent *event) override {
        QLabel::resizeEvent(event);
        fit_height();
    }
    void changeEvent(QEvent *event) override {
        QLabel::changeEvent(event);
        if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) fit_height();
    }

private:
    void fit_height() {
        const int height = heightForWidth(width());
        if (height > 0 && height != fitted_height_) {
            fitted_height_ = height;
            updateGeometry();
        }
    }
    int fitted_height_ = -1;
};
}

QLabel *field_hint(const QString &text) {
    auto *label = new HintLabel(text);
    apply_theme_text_style(label, 12, false, {}, true);
    return label;
}

OptionCard::OptionCard(const QString &title, const QString &description, const QString &kind, QWidget *parent)
    : QAbstractButton(parent), title_(title), description_(description), kind_(kind) {
    setText(title);
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMouseTracking(true);
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { update(); });
}

int OptionCard::card_height() const { return dialog_scaled(58); }

QSize OptionCard::sizeHint() const { return QSize(dialog_scaled(150), card_height()); }

QSize OptionCard::minimumSizeHint() const { return QSize(dialog_scaled(96), card_height()); }

namespace {
// The type icon sits left of the text, so a card with one is no taller than one without.
constexpr int KIND_TILE_SIZE = 30;
constexpr int KIND_TILE_GAP = 10;

QFont title_font_of(const QFont &base) {
    QFont font(base);
    font.setPixelSize(dialog_scaled(13));
    font.setWeight(QFont::DemiBold);
    return font;
}

QFont description_font_of(const QFont &base) {
    QFont font(base);
    font.setPixelSize(dialog_scaled(12));
    return font;
}
}

int OptionCard::required_width() const {
    const int text = std::max(QFontMetrics(title_font_of(font())).horizontalAdvance(title_),
                              QFontMetrics(description_font_of(font())).horizontalAdvance(description_));
    const int tile = kind_.isEmpty() ? 0 : dialog_scaled(KIND_TILE_SIZE + KIND_TILE_GAP);
    return text + tile + 2 * dialog_scaled(12) + 2;
}

void OptionCard::paintEvent(QPaintEvent *) {
    const auto &tokens = get_ui_theme()->tokens();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF rect = QRectF(this->rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const double radius = dialog_scaled_f(10);

    const bool enabled = isEnabled();
    const bool checked = isChecked();
    const bool hovered = enabled && underMouse();
    const bool has_kind = !kind_.isEmpty();
    if (checked && has_kind) {
        painter.setBrush(css_color(tokens.input_background));
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(rect, radius, radius);
        painter.setBrush(kind_tint(kind_, 0.10, tokens));
        painter.setPen(QPen(kind_color(kind_), dialog_scaled_f(1.5)));
    } else if (checked) {
        painter.setBrush(css_color(tokens.accent_soft));
        painter.setPen(QPen(css_color(tokens.accent), dialog_scaled_f(1.5)));
    } else {
        painter.setBrush(css_color(hovered ? tokens.surface_hover : tokens.input_background));
        painter.setPen(QPen(css_color(hovered ? tokens.border_hover : tokens.border), 1));
    }
    painter.drawRoundedRect(rect, radius, radius);

    const QFont title_font = title_font_of(font());
    const QFont description_font = description_font_of(font());
    const int pad_x = dialog_scaled(12);
    const QRectF inner = rect.adjusted(pad_x, dialog_scaled(10), -pad_x, -dialog_scaled(10));
    QRectF text_area = inner;
    QColor title_color;
    if (has_kind) {
        const int tile_size = dialog_scaled(KIND_TILE_SIZE);
        const QRectF tile(inner.left(), inner.center().y() - tile_size / 2.0, tile_size, tile_size);
        painter.setPen(Qt::NoPen);
        painter.setBrush(enabled ? kind_tint(kind_, checked ? 0.24 : 0.14, tokens) : css_color(tokens.border));
        painter.drawRoundedRect(tile, dialog_scaled(8), dialog_scaled(8));
        const int icon_size = dialog_scaled(15);
        const QRectF icon_rect(tile.center().x() - icon_size / 2.0, tile.center().y() - icon_size / 2.0, icon_size, icon_size);
        paint_kind_icon(&painter, icon_rect, kind_, enabled ? kind_color(kind_) : QColor(tokens.text_disabled));
        text_area.setLeft(tile.right() + dialog_scaled(KIND_TILE_GAP));
        title_color = checked ? kind_text_color(kind_, tokens) : css_color(tokens.text);
    } else {
        title_color = css_color(checked ? tokens.accent_text : tokens.text);
    }
    const int text_w = static_cast<int>(text_area.width());
    const double half = text_area.height() / 2;
    const QRectF title_rect(text_area.left(), text_area.top(), text_w, half);
    const QRectF description_rect(text_area.left(), text_area.top() + half, text_w, half);
    painter.setFont(title_font);
    painter.setPen(enabled ? title_color : css_color(tokens.text_disabled));
    painter.drawText(title_rect, Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(title_font).elidedText(title_, Qt::ElideRight, text_w));
    painter.setFont(description_font);
    painter.setPen(css_color(enabled ? tokens.text_muted : tokens.text_disabled));
    painter.drawText(description_rect, Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(description_font).elidedText(description_, Qt::ElideRight, text_w));
}

void OptionCard::changeEvent(QEvent *event) {
    // Greying out and restoring need a repaint and the matching cursor.
    if (event->type() == QEvent::EnabledChange) {
        setCursor(isEnabled() ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
    QAbstractButton::changeEvent(event);
}

void OptionCard::enterEvent(QEnterEvent *event) {
    update();
    QAbstractButton::enterEvent(event);
}

void OptionCard::leaveEvent(QEvent *event) {
    update();
    QAbstractButton::leaveEvent(event);
}

CardGrid::CardGrid(const QList<OptionCard *> &cards, QWidget *parent) : QWidget(parent), cards_(cards), spacing_(dialog_scaled(10)) {
    for (auto *card : cards_) card->setParent(this);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(height_for(WIDE_COLUMNS));
}

QList<OptionCard *> CardGrid::shown() const {
    QList<OptionCard *> result;
    for (auto *card : cards_)
        if (!card->isHidden()) result.append(card);
    return result;
}

int CardGrid::required_width() const {
    int width = 0;
    for (const auto *card : shown()) width = std::max(width, card->required_width());
    return width;
}

int CardGrid::columns(int width) const {
    const int wide = std::max(1, std::min(WIDE_COLUMNS, static_cast<int>(shown().size())));
    return (width - (wide - 1) * spacing_) / wide >= required_width() ? wide : std::min(wide, NARROW_COLUMNS);
}

int CardGrid::card_height() const {
    int height = 0;
    for (const auto *card : shown()) height = std::max(height, card->sizeHint().height());
    return height;
}

int CardGrid::height_for(int columns) const {
    const int rows = (static_cast<int>(shown().size()) + columns - 1) / columns;
    return rows * card_height() + std::max(0, rows - 1) * spacing_;
}

QSize CardGrid::sizeHint() const { return QSize(WIDE_COLUMNS * dialog_scaled(150), height()); }

QSize CardGrid::minimumSizeHint() const { return QSize(NARROW_COLUMNS * dialog_scaled(96) + spacing_, height()); }

void CardGrid::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    const int cols = columns(width());
    const int card_width = (width() - (cols - 1) * spacing_) / cols;
    const int card_h = card_height();
    const auto cards = shown();
    for (int index = 0; index < cards.size(); ++index)
        cards[index]->setGeometry(index % cols * (card_width + spacing_), index / cols * (card_h + spacing_), card_width, card_h);
    const int height = height_for(cols);
    if (this->height() != height) setFixedHeight(height);
}
}
