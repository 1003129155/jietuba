#pragma once
#include "clipboard/core/models.h"
#include <QImage>
#include <QList>
#include <QPoint>
#include <QStringList>
#include <functional>
#include <optional>

class QWidget;

// Pins from the clipboard: a history item, what the system clipboard holds, or the newest image in history.
// The pin settings choose what the hotkey takes and whether text keeps its formatting.
namespace jietuba::clipboard {
class ClipboardController;
class ClipboardManager;

// get_item comes from the controller in the clipboard window and from the manager on the
// global hotkey path, which has no window. center, when given, is where the pin is centred
// (global coordinates); otherwise the centre of the screen. A file item pins as its files
// would from the clipboard.
bool create_pin_from_clipboard_item(qint64 item_id, ClipboardController *controller, QWidget *clipboard_window = nullptr,
                                    std::optional<QPoint> center = std::nullopt);
bool create_pin_from_clipboard_item(qint64 item_id, ClipboardManager *manager, QWidget *clipboard_window = nullptr, std::optional<QPoint> center = std::nullopt);
// Both paths end here, so placement and the working set trim live in one place.
bool create_pin_from_image(const QImage &image, QWidget *clipboard_window = nullptr, std::optional<QPoint> center = std::nullopt);
// A pin per image and per text of items, in their order, and their files as the pin hotkey pins
// copied files: a pin per picture file, the rest together on one card that opens first. Pins step
// down and right from center. False when none of them can be pinned.
bool create_pins_from_clipboard_items(const QList<ClipboardItem> &items, QWidget *clipboard_window = nullptr, std::optional<QPoint> center = std::nullopt);
// A text-only pin, formatted as in html unless the settings turn formatting off; false for blank text.
bool create_pin_from_text(const QString &text, const QString &html, QWidget *clipboard_window = nullptr, std::optional<QPoint> center = std::nullopt);
// Pins what the system clipboard holds, the first kind found of: PNG data, a bitmap, copied files
// (a pin per picture file, animations playing, the rest as cards), text. With none of them and
// no copied files, the newest image in history. The settings can leave out images, text or files.
// Pictures decode off the UI thread and pin as they are ready. False when there is nothing to
// pin; declining to pin many pictures at once counts as handled.
bool pin_latest_clipboard_image(std::optional<QPoint> center = std::nullopt);
}
