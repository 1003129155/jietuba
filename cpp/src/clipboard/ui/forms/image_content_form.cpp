#include "clipboard/ui/forms/image_content_form.h"
#include "clipboard/ui/dialogs/manage_dialog.h"
#include "clipboard/ui/forms/form_widgets.h"
#include "core/logger.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/theme.h"
#include <QDragEnterEvent>
#include <QHBoxLayout>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>

namespace jietuba::clipboard {
namespace {
constexpr int PREVIEW_MAX_HEIGHT = 340;
// The new item page has buttons and a note below, so the preview is lower and an image adds no scroll bar.
constexpr int NEW_IMAGE_PREVIEW_MAX_HEIGHT = 240;
constexpr int CHECKER = 8;

QString format_bytes(qint64 size) {
    if (size >= 1024 * 1024) return python_fixed(static_cast<double>(size) / 1024 / 1024, 1) + " MB";
    return QString::number(std::max<qint64>(1, python_round(static_cast<double>(size) / 1024))) + " KB";
}

const QStringList &image_suffixes() {
    static const QStringList suffixes{".png", ".jpg", ".jpeg", ".bmp", ".gif", ".webp", ".tif", ".tiff", ".ico"};
    return suffixes;
}
}

ImagePreview::ImagePreview(const QImage &image, QWidget *parent, int max_height) : QWidget(parent), image_(image), max_height_(max_height) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setCursor(Qt::PointingHandCursor);
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { update(); });
}

int ImagePreview::box_height(int width) const {
    const int pad = dialog_scaled(12) * 2;
    const int inner_w = std::max(1, width - pad);
    const double ratio = static_cast<double>(image_.height()) / std::max(1, image_.width());
    const double fitted = std::min(image_.height() / devicePixelRatioF(), inner_w * ratio);
    return static_cast<int>(std::min(fitted, static_cast<double>(dialog_scaled(max_height_)))) + pad;
}

QSize ImagePreview::sizeHint() const {
    const int width = this->width() > 0 ? this->width() : dialog_scaled(420);
    return QSize(width, box_height(width));
}

void ImagePreview::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    const int height = box_height(width());
    if (this->height() != height) setFixedHeight(height);
}

QRect ImagePreview::image_rect() const {
    const int pad = dialog_scaled(12);
    const QRect area = rect().adjusted(pad, pad, -pad, -pad);
    const double dpr = devicePixelRatioF();
    // Shown at no more than 1:1 physical pixels, so small images are not blurred by enlarging.
    const QSize natural(static_cast<int>(python_round(image_.width() / dpr)), static_cast<int>(python_round(image_.height() / dpr)));
    const QSize size = natural.width() > area.width() || natural.height() > area.height() ? natural.scaled(area.size(), Qt::KeepAspectRatio) : natural;
    QRect result(0, 0, size.width(), size.height());
    result.moveCenter(area.center());
    return result;
}

QPixmap ImagePreview::scaled_pixmap(const QRect &rect) {
    const double dpr = devicePixelRatioF();
    const auto key = std::make_tuple(rect.width(), rect.height(), dpr);
    if (key != cache_key_) {
        QPixmap pixmap = QPixmap::fromImage(image_.scaled(static_cast<int>(python_round(rect.width() * dpr)), static_cast<int>(python_round(rect.height() * dpr)),
                                                          Qt::KeepAspectRatio, Qt::SmoothTransformation));
        pixmap.setDevicePixelRatio(dpr);
        scaled_ = pixmap;
        cache_key_ = key;
    }
    return scaled_;
}

void ImagePreview::paintEvent(QPaintEvent *) {
    const auto &tokens = get_ui_theme()->tokens();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const int radius = dialog_scaled(10);
    painter.setPen(css_color(tokens.border));
    painter.setBrush(css_color(tokens.surface_subtle));
    painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), radius, radius);

    const QRect target = image_rect();
    if (image_.hasAlphaChannel()) {
        const QColor light = !tokens.is_dark() ? QColor(255, 255, 255) : QColor(70, 74, 80);
        const QColor dark = !tokens.is_dark() ? QColor(228, 231, 235) : QColor(56, 60, 66);
        const int cell = dialog_scaled(CHECKER);
        painter.save();
        painter.setClipRect(target);
        for (int y = target.top(); y <= target.bottom(); y += cell)
            for (int x = target.left(); x <= target.right(); x += cell) {
                const int odd = ((x - target.left()) / cell + (y - target.top()) / cell) % 2;
                painter.fillRect(x, y, cell, cell, odd ? dark : light);
            }
        painter.restore();
    }
    painter.drawPixmap(target, scaled_pixmap(target));
}

void ImagePreview::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        emit double_clicked();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

QImage image_from_mime(const QMimeData *mime) {
    if (mime->hasImage()) {
        const QImage image = qvariant_cast<QImage>(mime->imageData());
        if (!image.isNull()) return image;
    }
    if (mime->hasUrls()) {
        for (const auto &url : mime->urls()) {
            const QString path = url.toLocalFile();
            const QString lower = path.toLower();
            if (std::any_of(image_suffixes().begin(), image_suffixes().end(), [&lower](const QString &suffix) { return lower.endsWith(suffix); })) {
                const QImage image(path);
                if (!image.isNull()) return image;
            }
        }
    }
    return {};
}

ImageDropZone::ImageDropZone(const QString &prompt, QWidget *parent) : QWidget(parent), prompt_(prompt) {
    setAcceptDrops(true);
    setMinimumHeight(dialog_scaled(150));
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { update(); });
}

void ImageDropZone::set_image(const std::optional<QImage> &image) {
    if (preview_) {
        layout_->removeWidget(preview_);
        preview_->hide();
        preview_->deleteLater();
        preview_ = nullptr;
    }
    if (image) {
        preview_ = new ImagePreview(*image, this, NEW_IMAGE_PREVIEW_MAX_HEIGHT);
        preview_->setCursor(Qt::ArrowCursor);
        layout_->addWidget(preview_);
    }
    update();
}

void ImageDropZone::paintEvent(QPaintEvent *) {
    if (preview_) return;
    const auto &tokens = get_ui_theme()->tokens();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(css_color(drag_active_ ? tokens.accent : tokens.border_hover), dialog_scaled(2));
    pen.setStyle(Qt::DashLine);
    painter.setPen(pen);
    painter.setBrush(css_color(drag_active_ ? tokens.accent_soft : tokens.surface_subtle));
    const int radius = dialog_scaled(10);
    painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), radius, radius);
    QFont font(this->font());
    font.setPixelSize(dialog_scaled(13));
    painter.setFont(font);
    painter.setPen(css_color(drag_active_ ? tokens.accent : tokens.text_muted));
    painter.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, prompt_);
}

void ImageDropZone::set_drag_active(bool active) {
    drag_active_ = active;
    update();
}

void ImageDropZone::dragEnterEvent(QDragEnterEvent *event) {
    if (!image_from_mime(event->mimeData()).isNull()) {
        event->acceptProposedAction();
        set_drag_active(true);
        return;
    }
    event->ignore();
}

void ImageDropZone::dragLeaveEvent(QDragLeaveEvent *event) {
    set_drag_active(false);
    QWidget::dragLeaveEvent(event);
}

void ImageDropZone::dropEvent(QDropEvent *event) {
    set_drag_active(false);
    const QImage image = image_from_mime(event->mimeData());
    if (image.isNull()) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    emit image_dropped(image);
}

void build_image_content_form(ManageDialog *dialog, const ClipboardItem &item, const QImage &image, qint64 data_size) {
    dialog->detail_layout->addWidget(field_label(ManageDialog::tr_text("Title (Optional)"), false));
    auto *title_input = new LineEdit;
    dialog->title_input = title_input;
    title_input->setPlaceholderText(ManageDialog::tr_text("Enter title..."));
    title_input->setText(item.title.value_or(QString()));
    dialog->detail_layout->addWidget(title_input);

    dialog->detail_layout->addWidget(field_label(ManageDialog::tr_text("Image")));
    if (image.isNull()) {
        dialog->detail_layout->addWidget(field_hint(ManageDialog::tr_text("The original image is no longer available")));
        dialog->image_preview = nullptr;
    } else {
        auto *preview = new ImagePreview(image, nullptr, PREVIEW_MAX_HEIGHT);
        dialog->image_preview = preview;
        preview->setToolTip(ManageDialog::tr_text("Double-click to pin it on screen"));
        QObject::connect(preview, &ImagePreview::double_clicked, dialog, [dialog] { dialog->pin_image_item(); });
        dialog->detail_layout->addWidget(preview);

        QStringList info{QString("%1 × %2").arg(image.width()).arg(image.height()), "PNG", format_bytes(data_size)};
        if (item.source_app && !item.source_app->isEmpty()) info.append(ManageDialog::tr_text("From {app}").replace("{app}", *item.source_app));
        dialog->detail_layout->addWidget(field_hint(info.join(QString::fromUtf8(" · "))));
    }

    auto *actions = new QHBoxLayout;
    actions->setSpacing(dialog_scaled(8));
    const std::pair<QString, std::function<void()>> buttons[] = {
        {ManageDialog::tr_text("Copy"), [dialog] { dialog->copy_image_item(); }},
        {ManageDialog::tr_text("Pin to Screen"), [dialog] { dialog->pin_image_item(); }},
        {ManageDialog::tr_text("Save As..."), [dialog] { dialog->save_image_item_as(); }},
    };
    for (const auto &[text, handler] : buttons) {
        auto *button = new PushButton(text);
        button->setCursor(Qt::PointingHandCursor);
        button->setEnabled(!image.isNull());
        QObject::connect(button, &QPushButton::clicked, dialog, handler);
        actions->addWidget(button);
    }
    actions->addStretch();
    dialog->detail_layout->addLayout(actions);
    dialog->detail_layout->addStretch();
}

void build_new_image_form(ManageDialog *dialog, bool monitoring) {
    dialog->detail_layout->addWidget(field_label(ManageDialog::tr_text("Title (Optional)"), false));
    auto *title_input = new LineEdit;
    dialog->title_input = title_input;
    title_input->setPlaceholderText(ManageDialog::tr_text("Enter title..."));
    dialog->detail_layout->addWidget(title_input);

    dialog->detail_layout->addWidget(field_label(ManageDialog::tr_text("Image")));
    auto *drop_zone = new ImageDropZone(ManageDialog::tr_text("Drag an image here"));
    dialog->image_drop_zone = drop_zone;
    QObject::connect(drop_zone, &ImageDropZone::image_dropped, dialog, [dialog](const QImage &image) { dialog->set_pending_image(image); });
    drop_zone->setEnabled(monitoring);
    dialog->detail_layout->addWidget(drop_zone);

    auto *actions = new QHBoxLayout;
    actions->setSpacing(dialog_scaled(8));
    auto *paste_btn = new PushButton(ManageDialog::tr_text("Paste from Clipboard"));
    dialog->paste_image_btn = paste_btn;
    QObject::connect(paste_btn, &QPushButton::clicked, dialog, [dialog] { dialog->use_clipboard_image(); });
    auto *browse_btn = new PushButton(ManageDialog::tr_text("Choose Image File"));
    QObject::connect(browse_btn, &QPushButton::clicked, dialog, [dialog] { dialog->browse_image_file(); });
    for (auto *button : {static_cast<QPushButton *>(paste_btn), static_cast<QPushButton *>(browse_btn)}) {
        button->setCursor(Qt::PointingHandCursor);
        button->setEnabled(monitoring);
        actions->addWidget(button);
    }
    actions->addStretch();
    dialog->detail_layout->addLayout(actions);

    const QString hint = monitoring ? ManageDialog::tr_text("The image is saved through clipboard history, so it will replace what is currently on the clipboard")
                                    : ManageDialog::tr_text("Turn on clipboard history first: images are saved through it");
    dialog->detail_layout->addWidget(field_hint(hint));
    dialog->detail_layout->addStretch();
}
}
