#pragma once
#include "pin/pin_text_selection.h"
#include <QPointer>
#include <QThread>
#include <atomic>
#include <functional>
#include <memory>

namespace jietuba {
class PinWindow;
class ToolSettingsManager;

// The recognized text laid out by the text recognition window's settings.
QString pin_layout_text(const OcrTextLayout &layout, ToolSettingsManager *settings);
// The pixel size that shows text about as large as the recognized lines appear on the pin.
int pin_text_pixel_size(const OcrTextLayout &layout, double view_scale);

// Recognition on a worker thread. Takes a QImage copy and no widget, so a window closed
// mid-recognition is never touched from here.
class PinOcrThread : public QThread {
    Q_OBJECT
public:
    // The image is what the pin shows, at the screen's pixel ratio; to_image maps its pixels to
    // the pin's image pixels, where layout comes out.
    PinOcrThread(const QImage &image, const QTransform &to_image, QObject *parent = nullptr) : QThread(parent), image_(image), to_image_(to_image) {}
    // Stops PP-OCR at its next tile or line; the result is then empty.
    void cancel() { cancelled_ = true; }
    std::optional<OcrResult> result;
    OcrTextLayout layout;
    // The recognized line boxes in the pin's image pixels, without their characters.
    std::optional<OcrResult> image_lines;
protected:
    void run() override;
private:
    QImage image_;
    QTransform to_image_;
    std::atomic_bool cancelled_ = false;
};

// A pin's OCR: availability and settings, the text selection, the worker thread's
// lifetime, loading results, and safe cleanup when the window closes.
class PinOCRManager {
public:
    using Callback = std::function<void(bool success, const QString &result)>;
    PinOCRManager(PinWindow *pin_window, ToolSettingsManager *config_manager);
    ~PinOCRManager();
    bool has_result() const { return ocr_has_result_; }
    bool text_selection_enabled() const { return text_selection_enabled_; }
    bool is_running() const { return ocr_thread != nullptr || pixels_wait_ != nullptr; }
    // The text selection once recognition has started; null before.
    PinTextSelection *text_selection() const { return selection_.get(); }
    // Starts recognition and the text selection. The automatic run after pinning follows
    // the automatic OCR setting; force is for user actions (translate, select).
    bool init_now(bool force = false);
    // Ensures a recognition and calls callback(has text, all text) when it ends. The
    // first caller starts it and later ones queue on the same run, so a pin is never
    // recognized twice.
    bool recognize_then(Callback callback);
    // Puts recognized text on the pin, replacing what it showed.
    void load_text(OcrTextLayout layout);
    // What recognition found, in the pin's image pixels: the line boxes and the layout as loaded.
    // Null before, and for text that was not recognized.
    const OcrResult *recognized_lines() const { return recognized_lines_ ? &*recognized_lines_ : nullptr; }
    const OcrTextLayout *recognized_layout() const { return recognized_layout_ ? &*recognized_layout_ : nullptr; }
    // Selecting and copying use layout in place of what was recognized, which stays as it is.
    void show_layout(const OcrTextLayout &layout);
    // Text cannot be selected while a translation is being laid over it.
    void set_translating(bool translating);
    // Called each time recognized text is loaded.
    std::function<void()> text_loaded;
    // Safely releases everything when the window closes.
    void cleanup();
    void set_enabled(bool enabled);
    void set_text_selection_enabled(bool enabled);
    bool toggle_text_selection();
    // Drawing turns the text selection off.
    void set_drawing_mode(bool active);
    QPointer<PinOcrThread> ocr_thread;
private:
    // While recognition waits for smart erase pixels; dropping it drops the wait.
    std::unique_ptr<QObject> pixels_wait_;
    // Every queued callback gets the result, success or not, and the queue empties.
    void flush_pending(bool success, const QString &result);
    // The image is taken on the main thread; the worker only gets plain data.
    bool start_recognition();
    void on_finished();
    void ensure_selection();
    // The user switch combined with temporary states such as thumbnail mode and drawing.
    void apply_text_selection_active();
    PinWindow *win_;
    std::unique_ptr<PinTextSelection> selection_;
    bool ocr_has_result_ = false;
    std::vector<Callback> pending_callbacks_;
    // Starts as the automatic OCR setting: with automatic recognition off the pin has no
    // text selection yet, and the menu must show it off, or the first click only turns it off.
    bool text_selection_enabled_;
    bool temporary_enabled_ = true;
    bool drawing_ = false;
    bool translating_ = false;
    std::optional<OcrResult> recognized_lines_;
    std::optional<OcrTextLayout> recognized_layout_;
};
}
