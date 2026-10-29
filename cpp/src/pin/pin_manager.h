#pragma once
#include <QImage>
#include <QList>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QSize>
#include <memory>
#include <optional>
#include <utility>

class QGraphicsItem;
class QWidget;

namespace jietuba {
struct AnimationSource;
class PinFileCard;
class PinTextContent;
class PinWindow;
class ToolSettingsManager;

// Creates and tracks every pin window: batch operations and cleanup. Main thread only.
class PinManager : public QObject {
    Q_OBJECT
public:
    static PinManager *instance();
    static PinManager *get_instance() { return instance(); }
    // Sizes are device-independent pixels. A small pin has a side under MIN_SIDE; whether one is
    // wanted is asked first.
    static bool is_small_pin(const QSize &size);
    static bool confirm_small_pin(const QSize &size, QWidget *parent);
    // image is the clean selection background; drawing_items are the capture's vector
    // annotations, moved by selection_offset; number_next syncs the number counter.
    PinWindow *create_pin(const QImage &image, const QPoint &position, ToolSettingsManager *config_manager, const QList<QGraphicsItem *> &drawing_items = {},
                          const std::optional<QPoint> &selection_offset = std::nullopt, std::optional<int> number_next = std::nullopt);
    // A pin of content's page at position, sized and shrunk to its screen as an image would be.
    PinWindow *create_text_pin(std::unique_ptr<PinTextContent> content, const QPoint &position, ToolSettingsManager *config_manager);
    // A pin playing source, from its first frame at position.
    PinWindow *create_animation_pin(AnimationSource source, const QPoint &position, ToolSettingsManager *config_manager);
    // A pin of card's files at position; image is the card drawn at 100% when the caller has it.
    PinWindow *create_file_pin(const PinFileCard &card, const QPoint &position, ToolSettingsManager *config_manager, const QImage &image = {});
    // Drops a pin from the list without closing it.
    void remove_pin(PinWindow *pin_window);
    void close_all();
    QList<PinWindow *> get_all_pins() const;
    int count() const { return static_cast<int>(pin_windows.size()); }
    bool has_pins() const { return !pin_windows.isEmpty(); }
    // The open pin already showing this, or null, also with repeats not pointed out. Pictures compare
    // their pixels, texts their text whatever the formatting, cards their files, animations their file.
    PinWindow *image_pin_of(const QImage &image) const;
    PinWindow *text_pin_of(const QString &text) const;
    PinWindow *file_pin_of(const QStringList &files) const;
    PinWindow *animation_pin_of(const QString &file) const;
    // Brings pin to the front and shakes it, showing the pins first if they are hidden; false for null.
    bool point_out(PinWindow *pin);
    // Hidden from the tray: the pins keep their order above one another, shown again. A new pin shows them all.
    bool all_hidden() const { return all_hidden_; }
    void set_all_hidden(bool hidden);
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
    void all_hidden_changed(bool hidden);
private:
    PinManager();
    void on_pin_closed(PinWindow *pin_window);
    bool repeats_pointed_out() const;
    QPointer<QWidget> capture_window_;
    bool all_hidden_ = false;
    // Top first, as they stood when hidden.
    QList<QPointer<PinWindow>> hidden_order_;
};

inline PinManager *get_pin_manager() { return PinManager::instance(); }
}
