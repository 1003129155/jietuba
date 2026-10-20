#include "barcode/result_window.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "core/theme.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "settings/tool_settings.h"
#include "ui/dialogs.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/fluent_lite.h"
#include "ui/fluent_lite/labels.h"
#include "ui/fluent_lite/theme.h"
#include "ui/fluent_lite/titlebar.h"
#include "ui/toast.h"
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <cmath>

namespace jietuba {
namespace {
QString tr_text(const char *text) { return i18n_tr(text, "BarcodeResultWindow"); }

constexpr int BADGE_RADIUS = 10;

// A URL to open when the text is an http(s) link. Only http(s): code content is
// untrusted, and file: or custom schemes handed to the system may launch local programs.
std::optional<QUrl> web_link(const QString &text) {
    const QUrl url(text.trimmed(), QUrl::StrictMode);
    if (url.isValid() && (url.scheme() == "http" || url.scheme() == "https") && !url.host().isEmpty()) return url;
    return std::nullopt;
}

// The number badge; the image and the cards share it so both show the same mark.
void paint_badge(QPainter &painter, const QPointF &center, int number) {
    const auto color = get_theme().theme_color();
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    const int radius = dialog_scaled(BADGE_RADIUS);
    painter.drawEllipse(center, radius, radius);
    auto font = painter.font();
    font.setPixelSize(dialog_scaled(12));
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(contrast_ink(color));
    const QRectF box(center.x() - radius, center.y() - radius, 2 * radius, 2 * radius);
    painter.drawText(box, Qt::AlignCenter, QString::number(number));
}
}

CodeImageView::CodeImageView(const QImage &image, const std::vector<DecodedCode> &codes, QWidget *parent) : QWidget(parent), image_(image), codes_(codes) {
    setMouseTracking(true);
    setMinimumSize(dialog_scaled(240), dialog_scaled(180));
}

void CodeImageView::set_active(int index) {
    if (index != active_) {
        active_ = index;
        update();
    }
}

QTransform CodeImageView::image_to_widget() const {
    const double scale = std::min({static_cast<double>(width()) / image_.width(), static_cast<double>(height()) / image_.height(), 1.0});
    return QTransform(scale, 0, 0, scale, (width() - image_.width() * scale) / 2, (height() - image_.height() * scale) / 2);
}

void CodeImageView::paintEvent(QPaintEvent *) {
    const auto transform = image_to_widget();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const auto image_rect = transform.mapRect(QRectF(image_.rect()));
    painter.drawImage(image_rect, image_);
    painter.setPen(QPen(QColor(ui_tokens(this).window_border), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(image_rect);

    const auto color = get_theme().theme_color();
    for (int index = 0; index < static_cast<int>(codes_.size()); ++index) {
        const bool active = index == active_;
        QColor fill(color);
        fill.setAlpha(active ? 110 : 40);
        painter.setPen(QPen(color, active ? 3 : 2));
        painter.setBrush(fill);
        const auto outline = transform.map(codes_[index].outline);
        painter.drawPolygon(outline);
        // A code touching the image edge would clip half the badge; pull it inside.
        const auto corner = outline.boundingRect().topLeft();
        const int radius = dialog_scaled(BADGE_RADIUS);
        const QPointF center(std::min(std::max(corner.x(), static_cast<double>(radius)), static_cast<double>(width() - radius)),
                             std::min(std::max(corner.y(), static_cast<double>(radius)), static_cast<double>(height() - radius)));
        paint_badge(painter, center, index + 1);
    }
    painter.end();
}

void CodeImageView::mouseMoveEvent(QMouseEvent *event) {
    const auto to_image = image_to_widget().inverted();
    const auto point = to_image.map(event->position());
    int hit = -1;
    for (int index = 0; index < static_cast<int>(codes_.size()); ++index)
        if (codes_[index].outline.containsPoint(point, Qt::OddEvenFill)) {
            hit = index;
            break;
        }
    emit hovered(hit);
}

void CodeImageView::leaveEvent(QEvent *) { emit hovered(-1); }

CodeBadge::CodeBadge(int number, QWidget *parent) : QWidget(parent), number_(number) {
    const int diameter = dialog_scaled(2 * BADGE_RADIUS);
    setFixedSize(diameter, diameter);
}

void CodeBadge::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    paint_badge(painter, QRectF(rect()).center(), number_);
    painter.end();
}

CodeText::CodeText(const QString &text, QWidget *parent) : TextEdit(parent) {
    setReadOnly(true);
    setFrameShape(QFrame::NoFrame);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    document()->setDocumentMargin(0);
    connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this,
            [this](const QSizeF &size) { setFixedHeight(static_cast<int>(std::ceil(size.height()))); });
    setPlainText(text);
    apply_theme();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { apply_theme(); });
}

void CodeText::apply_theme() {
    const auto &t = ui_tokens(this);
    setStyleSheet(QString("QTextEdit { background: transparent; color: %1; font: %2px %3; selection-background-color: %4; selection-color: %5; }")
                      .arg(t.text)
                      .arg(dialog_scaled(13))
                      .arg(FONT_FAMILY, t.accent, t.selected_text));
}

CodeCard::CodeCard(int index, const DecodedCode &code, QWidget *parent) : SimpleCardWidget(parent), index(index) {
    setProperty("active", false);
    // The base constructor applied only its own part of the theme.
    _apply_theme();

    auto *header = new QHBoxLayout;
    header->setSpacing(dialog_scaled(8));
    header->addWidget(new CodeBadge(index + 1, this));
    auto *format_label = new CaptionLabel(code.format_name, this);
    configure_dialog_control(*format_label);
    header->addWidget(format_label, 1);

    copy_button = new PushButton(tr_text("Copy"), this);
    configure_dialog_control(*copy_button);
    const auto text = code.text;
    connect(copy_button, &QPushButton::clicked, this, [this, text] { copy(text); });
    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(copy_button);
    if (const auto link = web_link(code.text)) {
        open_button = new PrimaryPushButton(tr_text("Open link"), this);
        configure_dialog_control(*open_button);
        const auto url = *link;
        connect(open_button, &QPushButton::clicked, this, [url] { QDesktopServices::openUrl(url); });
        buttons->addWidget(open_button);
    }

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(dialog_scaled(12), dialog_scaled(10), dialog_scaled(12), dialog_scaled(10));
    layout->setSpacing(dialog_scaled(6));
    layout->addLayout(header);
    layout->addWidget(new CodeText(code.text, this));
    layout->addLayout(buttons);
}

void CodeCard::set_active(bool active) {
    if (property("active").toBool() != active) {
        setProperty("active", active);
        style()->unpolish(this);
        style()->polish(this);
    }
}

void CodeCard::copy(const QString &text) {
    QApplication::clipboard()->setText(text);
    copy_button->setText(tr_text("Copied"));
    QTimer::singleShot(1500, copy_button, [button = copy_button] { button->setText(tr_text("Copy")); });
}

void CodeCard::_apply_theme() {
    SimpleCardWidget::_apply_theme();
    setStyleSheet(styleSheet() + QString("QFrame#FluentLiteSimpleCard[active=\"true\"] { border: 2px solid %1; }").arg(get_theme().theme_color_hex()));
}

void CodeCard::enterEvent(QEnterEvent *event) {
    SimpleCardWidget::enterEvent(event);
    emit hovered(index);
}

void CodeCard::leaveEvent(QEvent *event) {
    SimpleCardWidget::leaveEvent(event);
    emit hovered(-1);
}

BarcodeResultWindow::BarcodeResultWindow(const QImage &image, const std::vector<DecodedCode> &codes, QWidget *parent) : FrostedFramelessDialog(parent) {
    scale_dialog_font(*this);
    setAttribute(Qt::WA_DeleteOnClose, true);
    auto *title_bar = new FluentTitleBar(this);
    configure_dialog_control(*title_bar);
    setTitleBar(title_bar);
    setWindowTitle(tr_text("Scan result"));

    image_view = new CodeImageView(image, codes, this);
    connect(image_view, &CodeImageView::hovered, this, &BarcodeResultWindow::on_image_hovered);

    const auto summary = codes.empty() ? tr_text("No QR code or barcode found") : tr_text("Found %1 code(s)").replace("%1", QString::number(codes.size()));
    summary_label = new BodyLabel(summary, this);
    configure_dialog_control(*summary_label);
    summary_label->setWordWrap(true);

    auto *cards_host = new QWidget;
    auto *cards_layout = new QVBoxLayout(cards_host);
    cards_layout->setContentsMargins(0, 0, 0, 0);
    cards_layout->setSpacing(dialog_scaled(8));
    for (int index = 0; index < static_cast<int>(codes.size()); ++index) {
        auto *card = new CodeCard(index, codes[index], cards_host);
        connect(card, &CodeCard::hovered, this, &BarcodeResultWindow::set_active_code);
        cards_layout->addWidget(card);
        cards.push_back(card);
    }
    if (codes.empty()) {
        auto *empty_hint = new CaptionLabel(tr_text("Make sure the whole code is inside the selection."), cards_host);
        configure_dialog_control(*empty_hint);
        cards_layout->addWidget(empty_hint);
    }
    cards_layout->addStretch(1);

    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll_->setWidget(cards_host);

    panel_ = new QWidget(this);
    panel_->setFixedWidth(dialog_scaled(PANEL_WIDTH));
    auto *panel_layout = new QVBoxLayout(panel_);
    panel_layout->setContentsMargins(0, 0, 0, 0);
    panel_layout->setSpacing(dialog_scaled(8));
    // Takes effect at once: from the next scan a single code skips the window;
    // turned off again on the settings' shortcut behaviour page.
    copy_single_check = new CheckBox(tr_text("Copy directly if only one code"), panel_);
    configure_dialog_control(*copy_single_check);
    copy_single_check->setChecked(get_tool_settings_manager()->get_barcode_copy_single_enabled());
    connect(copy_single_check, &QCheckBox::toggled, this, [](bool checked) { get_tool_settings_manager()->set_barcode_copy_single_enabled(checked); });

    panel_layout->addWidget(summary_label);
    panel_layout->addWidget(scroll_, 1);
    panel_layout->addWidget(copy_single_check);

    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(dialog_scaled(12), title_bar->height() + dialog_scaled(4), dialog_scaled(12), dialog_scaled(12));
    root->setSpacing(dialog_scaled(12));
    root->addWidget(image_view, 1);
    root->addWidget(panel_);

    apply_theme();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { apply_theme(); });
    place(image);
}

void BarcodeResultWindow::apply_theme() {
    scroll_->setStyleSheet("QScrollArea { border: none; background: transparent; }"
                           "QScrollArea > QWidget > QWidget { background: transparent; }" +
                           scrollbar_qss(this));
}

void BarcodeResultWindow::place(const QImage &image) {
    auto *screen = QApplication::screenAt(QCursor::pos());
    if (!screen) screen = QApplication::primaryScreen();
    const auto available = screen->availableGeometry();
    const auto margins = layout()->contentsMargins();
    const int width = image.width() + layout()->spacing() + panel_->width() + margins.left() + margins.right();
    const int height = image.height() + margins.top() + margins.bottom();
    resize(std::max(dialog_scaled(640), std::min(width, static_cast<int>(available.width() * 0.8))),
           std::max(dialog_scaled(420), std::min(height, static_cast<int>(available.height() * 0.8))));
    move(available.center() - rect().center());
}

void BarcodeResultWindow::set_active_code(int index) {
    image_view->set_active(index);
    for (auto *card : cards) card->set_active(card->index == index);
}

void BarcodeResultWindow::on_image_hovered(int index) {
    set_active_code(index);
    // Scroll the list only when pointing from the image: scrolling while the mouse is
    // on a card would move the card out from under it.
    if (index >= 0) scroll_->ensureWidgetVisible(cards[index]);
}

BarcodeResultWindow *show_barcode_result(const QImage &image, bool copy_single) {
    QElapsedTimer started;
    started.start();
    const auto codes = read_codes(image);
    log_info(T("扫码完成: 识别到 {count} 个码, 耗时 {elapsed_ms:.0f} ms",
               {{"count", static_cast<int>(codes.size())}, {"elapsed_ms", started.nsecsElapsed() / 1e6}}),
             "Barcode");

    if (copy_single && codes.size() == 1) {
        QApplication::clipboard()->setText(codes.front().text);
        show_toast(tr_text("Copied: %1").replace("%1", codes.front().text));
        return nullptr;
    }
    auto *window = new BarcodeResultWindow(image, codes);
    track_modeless_dialog(window);
    window->show();
    window->raise();
    window->activateWindow();
    return window;
}
}
