#pragma once
#include "pin/ocr_text_layer.h"
#include <QPointer>
#include <QThread>
#include <functional>

namespace jietuba {
class PinWindow;
class ToolSettingsManager;

// Recognition on a worker thread. Takes a QImage copy and no widget, so a window closed
// mid-recognition is never touched from here.
class PinOcrThread : public QThread {
    Q_OBJECT
public:
    explicit PinOcrThread(const QImage &image, QObject *parent = nullptr) : QThread(parent), image_(image) {}
    std::optional<OcrResult> result;
    std::vector<OCRTextItem> prepared_items;
    std::optional<QRectF> prepared_union_rect;
protected:
    void run() override;
private:
    QImage image_;
};

// A pin's OCR: availability and settings, the text layer, the worker thread's
// lifetime, loading results, and safe cleanup when the window closes.
class PinOCRManager {
public:
    using Callback = std::function<void(bool success, const QString &result)>;
    PinOCRManager(PinWindow *pin_window, ToolSettingsManager *config_manager);
    bool has_result() const { return ocr_has_result_; }
    bool text_selection_enabled() const { return text_selection_enabled_; }
    bool is_running() const { return ocr_thread != nullptr; }
    // Creates the text layer and starts recognition. The automatic run after pinning
    // follows the automatic OCR setting; force is for user actions (translate, select).
    bool init_now(bool force = false);
    // Ensures a recognition and calls callback(has text, all text) when it ends. The
    // first caller starts it and later ones queue on the same run, so a pin is never
    // recognized twice.
    bool recognize_then(Callback callback);
    // Safely releases everything when the window closes.
    void cleanup();
    void set_enabled(bool enabled);
    void set_text_selection_enabled(bool enabled);
    bool toggle_text_selection();
    // Drawing hides the layer.
    void set_drawing_mode(bool active);
    void update_geometry(const QRect &rect);
    OCRTextLayer *ocr_text_layer = nullptr;
    QPointer<PinOcrThread> ocr_thread;
private:
    // Every queued callback gets the result, success or not, and the queue empties.
    void flush_pending(bool success, const QString &result);
    // The image is taken on the main thread; the worker only gets plain data.
    bool start_recognition();
    void on_finished(int original_width, int original_height);
    // The user switch combined with temporary states such as thumbnail mode.
    void apply_text_layer_enabled();
    PinWindow *win_;
    bool ocr_has_result_ = false;
    std::vector<Callback> pending_callbacks_;
    // Starts as the automatic OCR setting: with automatic recognition off the pin has no
    // text layer yet, and the menu must show it off, or the first click only turns it off.
    bool text_selection_enabled_;
    bool temporary_enabled_ = true;
};
}
