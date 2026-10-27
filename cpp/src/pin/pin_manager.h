#pragma once
#include <QImage>
#include <QList>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <memory>
#include <optional>
#include <utility>

class QGraphicsItem;
class QWidget;

namespace jietuba {
class PinTextContent;
class PinWindow;
class ToolSettingsManager;

// Creates and tracks every pin window: batch operations and cleanup. Main thread only.
class PinManager : public QObject {
    Q_OBJECT
public:
    static PinManager *instance();
    static PinManager *get_instance() { return instance(); }
    // image is the clean selection background; drawing_items are the capture's vector
    // annotations, moved by selection_offset; number_next syncs the number counter.
    PinWindow *create_pin(const QImage &image, const QPoint &position, ToolSettingsManager *config_manager, const QList<QGraphicsItem *> &drawing_items = {},
                          const std::optional<QPoint> &selection_offset = std::nullopt, std::optional<int> number_next = std::nullopt);
    // A pin of content's page at position, sized and shrunk to its screen as an image would be.
    PinWindow *create_text_pin(std::unique_ptr<PinTextContent> content, const QPoint &position, ToolSettingsManager *config_manager);
    // Drops a pin from the list without closing it.
    void remove_pin(PinWindow *pin_window);
    void close_all();
    QList<PinWindow *> get_all_pins() const;
    int count() const { return static_cast<int>(pin_windows.size()); }
    bool has_pins() const { return !pin_windows.isEmpty(); }
    void show_all();
    void hide_all();
    // Each pin to the centre of the screen it is on.
    void move_all_to_screen_center();
    // Pushes saved appearance settings to open pins.
    void refresh_all_appearance();
    void set_all_thumbnail_mode(bool active);
    // (saved, failed).
    std::pair<int, int> save_all_to_directory(const QString &directory, const QString &prefix = "pins");
    // A capture session is open. Its window shows after the pins and stays above them, so the pins keep
    // their place; a pin made meanwhile goes right below the capture window instead of on top of it.
    void begin_capture(QWidget *capture_window);
    void end_capture();
    bool capture_open() const { return capture_window_ != nullptr; }
    // At application exit.
    void cleanup();
    QList<QPointer<PinWindow>> pin_windows;
signals:
    void pin_created(PinWindow *pin_window);
    void pin_closed(PinWindow *pin_window);
    void all_pins_closed();
private:
    PinManager();
    void on_pin_closed(PinWindow *pin_window);
    QPointer<QWidget> capture_window_;
};

inline PinManager *get_pin_manager() { return PinManager::instance(); }
}
