#include "clipboard/ui/windows/pin_window.h"
#include "clipboard/controllers/clipboard_controller.h"
#include "clipboard/core/manager.h"
#include "core/logger.h"
#include "core/platform_utils.h"
#include "pin/pin_manager.h"
#include "pin/pin_text_content.h"
#include "pin/pin_window.h"
#include "settings/tool_settings.h"
#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QScreen>
#include <QWidget>

namespace jietuba::clipboard {
namespace {
// The newest image is not simply the first row: history sorts pinned items first and
// manual reordering ignores insertion time, so one page is scanned for the latest created_at.
constexpr int LATEST_IMAGE_SCAN_LIMIT = 50;

bool pin_item(qint64 item_id, const std::function<std::optional<ClipboardItem>(qint64)> &get_item, QWidget *clipboard_window, std::optional<QPoint> center) {
    const auto clipboard_item = get_item(item_id);
    if (clipboard_item && clipboard_item->content_type == "text")
        return create_pin_from_text(clipboard_item->content, clipboard_item->html_content.value_or(QString()), clipboard_window, center);
    if (!clipboard_item || clipboard_item->content_type != "image") {
        log_error(T("无法创建钉图：item_id={item_id} 不是图片", {{"item_id", item_id}}), "Clipboard");
        return false;
    }
    if (!clipboard_item->image_id || clipboard_item->image_id->isEmpty()) {
        log_error(T("无法创建钉图：图片 ID 为空"), "Clipboard");
        return false;
    }
    // The full image data, not the thumbnail.
    auto image_data = ClipboardManager::instance().get_image_data(*clipboard_item->image_id);
    if (!image_data || image_data->isEmpty()) {
        log_error(T("无法创建钉图：图片数据加载失败 (image_id={image_id})", {{"image_id", *clipboard_item->image_id}}), "Clipboard");
        return false;
    }
    const QImage image = QImage::fromData(*image_data);
    if (image.isNull()) {
        log_error(T("无法创建钉图：图片解码失败"), "Clipboard");
        return false;
    }
    // The decoded QImage needs no compressed data; the PNG bytes go early.
    image_data.reset();
    if (!create_pin_from_image(image, clipboard_window, center)) return false;
    log_debug(T("从剪贴板创建钉图窗口成功 (item_id={item_id})", {{"item_id", item_id}}), "Clipboard");
    return true;
}

// The screen holding center, else the clipboard window's, else the primary.
QScreen *target_screen(QWidget *clipboard_window, std::optional<QPoint> center) {
    if (center) {
        if (auto *screen = QGuiApplication::screenAt(*center)) return screen;
    } else if (clipboard_window) {
        return clipboard_window->screen();
    }
    return QGuiApplication::primaryScreen();
}

QPoint calculate_pin_position(const QSize &size, QWidget *clipboard_window, std::optional<QPoint> center) {
    const auto *screen = target_screen(clipboard_window, center);
    if (!screen) return QPoint(100, 100);

    const QRect screen_geometry = screen->geometry();
    const QPoint target = center ? *center : QPoint(screen_geometry.x() + screen_geometry.width() / 2, screen_geometry.y() + screen_geometry.height() / 2);
    int pin_x = target.x() - size.width() / 2;
    int pin_y = target.y() - size.height() / 2;
    // Kept inside the screen.
    pin_x = std::max(screen_geometry.x(), std::min(pin_x, screen_geometry.right() - size.width()));
    pin_y = std::max(screen_geometry.y(), std::min(pin_y, screen_geometry.bottom() - size.height()));
    return {pin_x, pin_y};
}

// The system clipboard's image, else its text. An image wins over text, so copied spreadsheet
// cells pin as the picture of the cells and a capture copied with its file path pins the capture.
struct SystemClipboardContent {
    QImage image;
    QString text, html;
};

SystemClipboardContent read_system_clipboard(bool images, bool texts) {
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard) return {};
    if (images)
        if (auto image = clipboard->image(); !image.isNull()) return {image, {}, {}};
    const QMimeData *mime = clipboard->mimeData();
    // hasText() also holds for a bare file list (its paths as text); only a real text format counts.
    if (texts && mime && mime->hasFormat("text/plain")) return {{}, mime->text(), mime->hasHtml() ? mime->html() : QString()};
    return {};
}

// The history image with the latest created_at.
std::optional<ClipboardItem> find_latest_history_image() {
    // With monitoring off the history is stale, and one hotkey does not open the database.
    if (!get_tool_settings_manager()->get_clipboard_enabled()) return std::nullopt;
    auto &manager = ClipboardManager::instance();
    if (!manager.is_available()) return std::nullopt;
    QList<ClipboardItem> candidates;
    for (const auto &item : manager.get_history(0, LATEST_IMAGE_SCAN_LIMIT, std::nullopt, FilterRule::of_kind("image")))
        if (item.image_id && !item.image_id->isEmpty()) candidates.append(item);
    if (candidates.isEmpty()) return std::nullopt;
    const ClipboardItem *latest = nullptr;
    for (const auto &item : candidates)
        if (item.created_at.isValid() && (!latest || item.created_at > latest->created_at)) latest = &item;
    // created_at is normally present; without it, list order.
    return latest ? *latest : candidates.front();
}
}

bool create_pin_from_clipboard_item(qint64 item_id, ClipboardController *controller, QWidget *clipboard_window, std::optional<QPoint> center) {
    return pin_item(item_id, [controller](qint64 id) { return controller->get_item(id); }, clipboard_window, center);
}

bool create_pin_from_clipboard_item(qint64 item_id, ClipboardManager *manager, QWidget *clipboard_window, std::optional<QPoint> center) {
    return pin_item(item_id, [manager](qint64 id) { return manager->get_item(id); }, clipboard_window, center);
}

bool create_pin_from_image(const QImage &image, QWidget *clipboard_window, std::optional<QPoint> center) {
    if (image.isNull()) return false;
    auto *config_manager = get_tool_settings_manager();
    const QPoint position = calculate_pin_position(image.size(), clipboard_window, center);
    get_pin_manager()->create_pin(image, position, config_manager);
    // Capture pins trim the working set when the capture window finishes; the clipboard path has no capture window, so it trims here.
    request_trim_working_set(1500);
    return true;
}

bool create_pin_from_text(const QString &text, const QString &html, QWidget *clipboard_window, std::optional<QPoint> center) {
    if (text.trimmed().isEmpty()) return false;
    auto *settings = get_tool_settings_manager();
    // Unformatted, the pin keeps the formatting for its menu to switch to.
    auto content = std::make_unique<PinTextContent>(text, html, settings->get_pin_text_formatting());
    const QPoint position = calculate_pin_position(content->size(), clipboard_window, center);
    get_pin_manager()->create_text_pin(std::move(content), position, settings);
    return true;
}

bool pin_latest_clipboard_image(std::optional<QPoint> center) {
    auto *settings = get_tool_settings_manager();
    const bool images = settings->get_pin_clipboard_image(), texts = settings->get_pin_clipboard_text();
    const auto content = read_system_clipboard(images, texts);
    if (!content.image.isNull()) {
        if (!create_pin_from_image(content.image, nullptr, center)) return false;
        log_debug(T("已钉住系统剪贴板中的图片"), "Clipboard");
        return true;
    }
    if (create_pin_from_text(content.text, content.html, nullptr, center)) {
        log_debug(T("已钉住系统剪贴板中的文字"), "Clipboard");
        return true;
    }
    if (!images) return false;
    const auto found = find_latest_history_image();
    if (!found) {
        log_debug(T("剪贴板和历史中都没有图片，钉图已跳过"), "Clipboard");
        return false;
    }
    return create_pin_from_clipboard_item(found->id, &ClipboardManager::instance(), nullptr, center);
}
}
