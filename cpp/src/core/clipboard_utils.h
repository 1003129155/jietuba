#pragma once
#include <QImage>
#include <QString>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>

namespace jietuba {
class SaveService;
class ToolSettingsManager;

// Copies image to the clipboard: on Windows CF_DIBV5 + PNG, retrying while
// another program holds the clipboard. A non-empty file_reference adds CF_HDROP
// in the same transaction, so listeners see one change, not two. The file must exist.
void copy_image_to_clipboard(const QImage &image, const QString &file_reference = {});

// Background progress of one delivery; join() returns once clipboard and save are both done.
class Delivery {
public:
    explicit Delivery(int parts) : pending_(parts) {}
    void part_done();
    bool join(std::optional<double> timeout_seconds = std::nullopt);
    bool is_alive();
private:
    std::mutex lock_;
    std::condition_variable done_;
    int pending_;
};

struct SaveKwargs {
    QString directory;
    QString prefix;
    QString image_format = "PNG";
};

// Copies and/or saves in the background so callers never wait for encoding.
// Clipboard writes share one queue, so consecutive captures leave the last one.
// With both copy and save, the save path is reserved first (an empty file) so
// CF_HDROP can go out with the bitmap without waiting for the file to be written.
// original, when given, is saved beside the file with an "_original" suffix.
std::shared_ptr<Delivery> deliver_image_async(const QImage &image, bool copy_to_clipboard = true, std::shared_ptr<SaveService> save_service = nullptr,
                                              const SaveKwargs &save_kwargs = {}, bool write_file_reference = true, const QImage &original = QImage());
// Waits for queued clipboard writes; called before exit so the last capture lands.
bool wait_for_clipboard_writes(std::optional<double> timeout_seconds = std::nullopt);
// The exit of every capture. trigger is the auto-save moment: "capture" or "pin".
// original is the image before annotations or watermark, saved too when enabled.
std::shared_ptr<Delivery> deliver_screenshot(const QImage &image, ToolSettingsManager *config_manager, bool copy_to_clipboard = true,
                                             const QString &trigger = "capture", const QString &file_prefix = {}, const QImage &original = QImage(),
                                             std::shared_ptr<SaveService> save_service = nullptr);
// Must be called on the main thread before the first background delivery (the relay lives there).
void ensure_main_thread_clipboard();

QByteArray build_dibv5(const QImage &image);
QByteArray build_png(const QImage &image);
QByteArray build_hdrop(const QString &path);
}
