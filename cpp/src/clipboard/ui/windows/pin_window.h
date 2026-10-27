#pragma once
#include <QImage>
#include <QPoint>
#include <functional>
#include <optional>

class QWidget;

// Pins from the clipboard: a history item, the system clipboard's image or text, or the newest image in history.
// The pin settings choose what the hotkey takes and whether text keeps its formatting.
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
// A text-only pin, formatted as in html unless the settings turn formatting off; false for blank text.
bool create_pin_from_text(const QString &text, const QString &html, QWidget *clipboard_window = nullptr, std::optional<QPoint> center = std::nullopt);
// Pins the system clipboard's image, else its text, otherwise the newest image in history; the
// settings can leave out images or text. The clipboard comes first so the hotkey keeps working
// with clipboard history turned off.
bool pin_latest_clipboard_image(std::optional<QPoint> center = std::nullopt);
}
