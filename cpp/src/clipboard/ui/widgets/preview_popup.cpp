#include "clipboard/ui/widgets/preview_popup.h"
#include "clipboard/core/manager.h"
#include "clipboard/ui/widgets/item_delegate.h"
#include "core/logger.h"
#include "core/text_utils.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "ui/fluent_lite/inputs.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPixmap>
#include <QPainter>
#include <QScreen>
#include <QTextDocument>

namespace jietuba::clipboard {
namespace {
QString tr_preview(const char *text) { return QCoreApplication::translate("ClipboardPreview", text); }

// What the picture of an item's preview is loaded from; none for items shown as text.
std::optional<PreviewImageRequest> image_request(const ClipboardItem &item, ClipboardManager *manager) {
    PreviewImageRequest request;
    request.manager = manager;
    request.bound = QSize(scaled(400), scaled(300));
    if (item.content_type == "image") {
        request.image_id = item.image_id.value_or(QString());
        request.thumbnail = item.thumbnail.value_or(QString());
        // Pictures the backend could not decode keep neither: their text shows at once.
        if (request.image_id.isEmpty() && request.thumbnail.isEmpty()) return std::nullopt;
        return request;
    }
    // One file may be a picture; the loader tells from its content.
    if (item.content_type == "file") {
        const QStringList files = item_paths(item);
        if (files.size() != 1) return std::nullopt;
        request.file_path = files.front();
        return request;
    }
    return std::nullopt;
}
}

QPoint side_position(const QSize &size, const QPoint &pos, const std::optional<QRect> &avoid_rect, const QString &prefer_side, std::optional<int> gap_value) {
    const int gap = gap_value ? *gap_value : scaled(10);
    QScreen *screen = QGuiApplication::screenAt(pos);
    if (!screen) screen = QGuiApplication::primaryScreen();
    const QRect screen_geo = screen->availableGeometry();
    const int width = size.width(), height = size.height();

    int x = pos.x() + scaled(20);
    int y = pos.y();

    if (avoid_rect) {
        const int left_x = avoid_rect->left() - width - gap;
        const int right_x = avoid_rect->right() + gap;
        const bool left_fits = avoid_rect->left() - screen_geo.left() >= width + gap;
        const bool right_fits = screen_geo.right() - avoid_rect->right() >= width + gap;
        const int roomier_x = screen_geo.right() - avoid_rect->right() >= avoid_rect->left() - screen_geo.left() ? right_x : left_x;
        if (prefer_side == "left") x = left_fits ? left_x : right_fits ? right_x : roomier_x;
        else x = right_fits ? right_x : left_fits ? left_x : roomier_x;
    } else if (x + width > screen_geo.right()) {
        // The popup must not cover the cursor: the list would get Leave and close it, then reopen it when the mouse returns, flickering.
        x = pos.x() - scaled(20) - width;
    }

    if (x + width > screen_geo.right()) x = screen_geo.right() - width - gap;
    if (x < screen_geo.left()) x = screen_geo.left();
    if (y + height > screen_geo.bottom()) y = screen_geo.bottom() - height - gap;
    if (y < screen_geo.top()) y = screen_geo.top();
    return {x, y};
}

PreviewPopup *PreviewPopup::instance() {
    static PreviewPopup *popup = new PreviewPopup;
    return popup;
}

PreviewPopup::PreviewPopup() : QWidget(nullptr) {
    // Frameless, tool window, on top.
    setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground, false);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setup_ui();
    show_timer_.setSingleShot(true);
    connect(&show_timer_, &QTimer::timeout, this, [this] {
        delay_elapsed_ = true;
        try_show();
    });
    connect(&loader_, &PreviewImageLoader::loaded, this, &PreviewPopup::on_image_loaded);
}

void PreviewPopup::setup_ui() {
    layout_ = new QVBoxLayout(this);
    layout_->setSpacing(0);
    // The window is as big as what it shows; a window left to size itself is at least 200 x 100.
    layout_->setSizeConstraint(QLayout::SetFixedSize);

    // Title row (text previews only; hidden for images).
    title_label = new QLabel;
    title_label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    title_label->hide();
    layout_->addWidget(title_label);
    title_line_ = new QWidget;
    title_line_->setAutoFillBackground(true);
    QPalette line_palette = title_line_->palette();
    line_palette.setColor(QPalette::Window, QColor("#E0E0E0"));
    title_line_->setPalette(line_palette);
    title_line_->hide();
    layout_->addWidget(title_line_);

    color_label = new QLabel;
    color_label->hide();
    layout_->addWidget(color_label);

    content_widget = new TextEdit;
    content_widget->setReadOnly(true);
    content_widget->setLineWrapMode(QTextEdit::WidgetWidth);
    content_widget->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    content_widget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout_->addWidget(content_widget);

    image_label = new QLabel;
    image_label->setAlignment(Qt::AlignCenter);
    image_label->hide();
    layout_->addWidget(image_label);
    apply_scale();
}

void PreviewPopup::apply_scale() {
    // Cached pictures are scaled to the old size.
    loader_.clear_cache();
    // The type selector names the C++ class (namespaces become "--").
    setStyleSheet(QString(R"(
            jietuba--clipboard--PreviewPopup {
                background: #FAFAFA;
                border: 1px solid #D0D0D0;
                border-radius: %1px;
            }
        )")
                      .arg(scaled(8)));
    const int margin = scaled(2);
    layout_->setContentsMargins(margin, margin, margin, margin);
    // The content has no frame of its own; a line under the title marks where it starts. The title lines up with the text below it.
    title_line_->setFixedHeight(1);
    title_label->setStyleSheet(QString("font-size: %1px; color: #666; font-weight: bold; padding: %2px %3px %4px %3px;")
                                   .arg(scaled(12))
                                   .arg(scaled(4))
                                   .arg(scaled(10))
                                   .arg(scaled(6)));
    content_widget->setStyleSheet(QString(R"(
            QTextEdit {
                background: #FAFAFA;
                border: none;
                padding: %1px %2px;
                font-size: %3px;
                color: #333;
            }
        )")
                                      .arg(scaled(4))
                                      .arg(scaled(8))
                                      .arg(scaled(13)));
    content_widget->document()->setDocumentMargin(scaled(2));
    // Only a maximum size, so the content sizes itself.
    content_widget->setMaximumSize(scaled(500), scaled(400));
    set_own_style(image_label, QString("background: #F0F0F0; border: 1px solid #E0E0E0; border-radius: %1px;").arg(scaled(4)));
}

void PreviewPopup::set_display_enabled(bool enabled) {
    display_enabled_ = enabled;
    if (!display_enabled_) {
        force_cleanup();
        // Nothing hovers while the window is closed, so the pictures need not stay in memory.
        loader_.clear_cache();
    }
}

void PreviewPopup::show_preview(const std::optional<ClipboardItem> &item, const QPoint &pos, int delay_ms, const QString &prefer_side,
                                const std::optional<QRect> &avoid_rect) {
    if (!display_enabled_) return;
    if (!item) {
        hide_preview();
        return;
    }
    // The same item already shown: nothing to do.
    if (current_item_id_ == item->id && isVisible()) return;
    // Hovering again over the item being prepared keeps its load.
    const bool loading_same = pending_item_ && pending_item_->id == item->id && request_serial_;
    pending_item_ = item;
    pending_pos_ = pos;
    pending_prefer_side_ = prefer_side;
    pending_avoid_rect_ = avoid_rect;
    if (!loading_same) {
        // The load starts with the hover and runs alongside the delay.
        loader_.cancel();
        loaded_image_.reset();
        const auto request = image_request(*item, manager_);
        request_serial_ = request ? loader_.request(*request) : 0;
    }
    delay_elapsed_ = delay_ms <= 0;
    if (delay_ms > 0) {
        show_timer_.start(delay_ms);
    } else {
        show_timer_.stop();
        try_show();
    }
}

void PreviewPopup::try_show() {
    if (delay_elapsed_ && (!request_serial_ || loaded_image_)) do_show();
}

void PreviewPopup::on_image_loaded(quint64 serial, const QImage &image) {
    // A result for an item the mouse has left is dropped.
    if (serial != request_serial_) return;
    loaded_image_ = image;
    try_show();
}

void PreviewPopup::do_show() {
    // The window may hide before the timer fires; an old delayed show must not bring the preview back after cleanup.
    if (!display_enabled_) {
        force_cleanup();
        return;
    }
    if (!pending_item_) return;
    const ClipboardItem item = *pending_item_;
    current_item_id_ = item.id;
    color_label->hide();

    if (item.content_type == "image") {
        // Images the backend could not decode (several pictures copied from Word, EMF) keep no pixels;
        // their text, such as "[PNG+CF_DIB 7.9 MB]", is shown instead of an empty frame.
        if (!show_image_preview(loaded_image_.value_or(QImage()))) show_text_preview(item);
    } else if (item.content_type == "file") {
        // A single image file shows as the picture, anything else as the list of files.
        if (!show_image_preview(loaded_image_.value_or(QImage()))) show_file_preview(item);
    } else if (item.content_type == "text") show_text_preview(item);
    else return;

    // Sizes the window to the layout now, so the position below uses the new size; adjustSize() would stretch it to 200 x 100.
    layout_->activate();
    move(side_position(size(), pending_pos_, pending_avoid_rect_, pending_prefer_side_.isEmpty() ? QString("auto") : pending_prefer_side_));
    show();
}

void PreviewPopup::show_text_preview(const ClipboardItem &item) {
    // The time and source above, the full content below; the list rows leave the source out.
    QString title = item.created_at.isValid() ? item.created_at.toString("yyyy-MM-dd HH:mm:ss") : tr_preview("📝 文本");
    if (const QString source = row_source_text(item.source_app.value_or(QString())); !source.isEmpty()) title += QString::fromUtf8(" · ") + source;
    title_label->setText(title);
    title_label->show();
    title_line_->show();
    // Length limited to stay responsive.
    const QString content = left_chars(item.content, 2000);
    content_widget->setMinimumSize(0, 0);
    content_widget->setMaximumSize(scaled(500), scaled(400));
    content_widget->setPlainText(content);
    adjust_content_size();
    content_widget->show();
    image_label->hide();
    if (item.color) {
        const QSize swatch_size(scaled(240), scaled(128));
        QPixmap swatch(swatch_size);
        QPainter painter(&swatch);
        const int tile = scaled(12);
        for (int y = 0; y < swatch.height(); y += tile)
            for (int x = 0; x < swatch.width(); x += tile)
                painter.fillRect(x, y, tile, tile, (x / tile + y / tile) % 2 ? QColor("#D8D8D8") : QColor(Qt::white));
        painter.fillRect(swatch.rect(), *item.color);
        painter.setPen(QColor("#999999"));
        painter.drawRect(swatch.rect().adjusted(0, 0, -1, -1));
        painter.end();
        color_label->setFixedSize(swatch_size);
        color_label->setPixmap(swatch);
        color_label->show();
        content_widget->setFixedWidth(swatch_size.width());
    }
}

void PreviewPopup::show_file_preview(const ClipboardItem &item) {
    title_label->hide();
    title_line_->hide();
    image_label->hide();

    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(item.content.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        log_exception(error.error != QJsonParseError::NoError ? error.errorString() : "content is not a JSON object", T("加载文本预览"));
        content_widget->setMinimumSize(0, 0);
        content_widget->setMaximumSize(scaled(500), scaled(400));
        content_widget->setPlainText(item.content);
        adjust_content_size();
        content_widget->show();
        return;
    }
    QStringList files;
    for (const auto &value : document.object().value("files").toArray()) files.append(value.toString());
    if (files.isEmpty()) {
        content_widget->setPlainText(tr_preview("无文件信息"));
        content_widget->show();
        return;
    }

    QStringList lines;
    if (files.size() == 1) {
        lines << QFileInfo(files[0]).fileName() << "" << files[0];
    } else {
        // Grouped by folder, in order of first appearance.
        QList<std::pair<QString, QStringList>> dir_files;
        for (const auto &filepath : files) {
            const QFileInfo info(filepath);
            const QString dir_path = QDir::toNativeSeparators(info.path());
            const QString filename = info.fileName();
            auto it = std::find_if(dir_files.begin(), dir_files.end(), [&dir_path](const auto &entry) { return entry.first == dir_path; });
            if (it == dir_files.end()) dir_files.append({dir_path, {filename}});
            else it->second.append(filename);
        }
        if (dir_files.size() == 1) {
            for (const auto &fn : dir_files.front().second) lines.append(fn);
            lines << "" << dir_files.front().first;
        } else {
            int group_num = 1;
            for (const auto &[dir_path, filenames] : dir_files) {
                if (filenames.size() == 1) {
                    lines.append(QString("[%1] %2").arg(group_num).arg(filenames.front()));
                    lines.append(QDir::toNativeSeparators(QDir(dir_path).filePath(filenames.front())));
                } else {
                    lines.append(QString("[%1]").arg(group_num));
                    for (const auto &fn : filenames) lines.append("  " + fn);
                    lines.append(dir_path);
                }
                group_num += 1;
                lines.append("");
            }
            if (!lines.isEmpty() && lines.back().isEmpty()) lines.removeLast();
        }
    }
    content_widget->setMinimumSize(0, 0);
    content_widget->setMaximumSize(scaled(500), scaled(400));
    content_widget->setPlainText(lines.join("\n"));
    adjust_content_size();
    content_widget->show();
}

void PreviewPopup::adjust_content_size() {
    // Sized to the content: the ideal width when it fits unwrapped, else the maximum width with the document wrapping.
    QTextDocument *doc = content_widget->document();
    const QFontMetrics font_metrics = content_widget->fontMetrics();
    const int line_height = font_metrics.height();
    const QStringList lines = doc->toPlainText().split(u'\n');
    int max_line_width = 0;
    for (const auto &line : lines) max_line_width = std::max(max_line_width, font_metrics.horizontalAdvance(line));

    // CSS padding 4px vertical + 8px horizontal, document margin 2px, and 2px to spare on each side (values at 100%).
    const int h_padding = scaled(8) * 2 + scaled(2) * 2 + scaled(4);
    const int v_padding = scaled(4) * 2 + scaled(2) * 2 + scaled(4);
    const int ideal_width = max_line_width + h_padding;
    const int max_width = scaled(500);
    const int min_width = scaled(80);

    int actual_width = 0, actual_height = 0;
    if (ideal_width <= max_width) {
        actual_width = std::max(ideal_width, min_width);
        doc->setTextWidth(-1);
        actual_height = static_cast<int>(lines.size()) * line_height + v_padding;
    } else {
        actual_width = max_width;
        doc->setTextWidth(max_width - h_padding);
        actual_height = static_cast<int>(doc->size().height()) + v_padding;
    }
    const int max_height = scaled(550);
    const int min_height = line_height + v_padding;
    actual_height = std::max(std::min(actual_height, max_height), min_height);
    content_widget->setFixedSize(actual_width, actual_height);
}

bool PreviewPopup::show_image_preview(const QImage &image) {
    title_label->hide();
    title_line_->hide();
    content_widget->hide();
    // A label left visible would show the previous record's picture.
    image_label->hide();
    if (image.isNull()) return false;
    const QPixmap pixmap = QPixmap::fromImage(image);
    image_label->setPixmap(pixmap);
    image_label->setFixedSize(pixmap.size());
    image_label->show();
    return true;
}

void PreviewPopup::clear_pending() {
    show_timer_.stop();
    loader_.cancel();
    request_serial_ = 0;
    loaded_image_.reset();
    delay_elapsed_ = false;
    pending_item_.reset();
    pending_pos_ = {};
    pending_prefer_side_ = "auto";
    pending_avoid_rect_.reset();
    current_item_id_.reset();
}

void PreviewPopup::hide_preview() {
    clear_pending();
    hide();
}

void PreviewPopup::force_cleanup() {
    clear_pending();
    if (isVisible()) hide();
}
}
