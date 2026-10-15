#pragma once
#include <QImage>
#include <QPoint>
#include <functional>
#include <optional>

class QWidget;

// Pins from the clipboard: a history item, the system clipboard's image or the newest image in history.
namespace jietuba::clipboard {
class ClipboardController;
class ClipboardManager;

// get_item comes from the controller in the clipboard window and from the manager on the
// global hotkey path, which has no window. center, when given, is where the pin is centred
// (global coordinates); otherwise the centre of the screen.
bool create_pin_from_clipboard_item(qint64 item_id, ClipboardController *controller, QWidget *clipboard_window = nullptr,
                                    std::optional<QPoint> center = std::nullopt);
bool create_pin_from_clipboard_item(qint64 item_id, ClipboardManager *manager, QWidget *clipboard_window = nullptr, std::optional<QPoint> center = std::nullopt);
// Both paths end here, so placement and the working set trim live in one place.
bool create_pin_from_image(const QImage &image, QWidget *clipboard_window = nullptr, std::optional<QPoint> center = std::nullopt);
// Pins the system clipboard's image, otherwise the newest image in history. The clipboard
// comes first so the hotkey keeps working with clipboard history turned off.
bool pin_latest_clipboard_image(std::optional<QPoint> center = std::nullopt);
}
