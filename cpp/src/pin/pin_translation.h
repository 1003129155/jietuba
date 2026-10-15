#pragma once
#include <QPoint>
#include <QString>

namespace jietuba {
class OCRTextLayer;
class PinWindow;
class ToolSettingsManager;

// Pin translation: takes the OCR text, calls the translation service and places its window.
class PinTranslationHelper {
public:
    PinTranslationHelper(PinWindow *parent, ToolSettingsManager *config_manager) : parent(parent), config_manager(config_manager) {}
    // Whether translation started.
    bool translate(OCRTextLayer *ocr_text_layer);
    // Shows the recognizing state at once; the OCR manager fills in the result later.
    bool begin_ocr_translation();
    // Hands the pin's OCR result to the shared OCR translation completion.
    void complete_ocr_translation(bool success, const QString &result);
    PinWindow *parent;
    ToolSettingsManager *config_manager;
private:
    // Right of the pin, else left of it; kept inside the screen vertically.
    QPoint calculate_window_position() const;
    void show_error(const QString &message);
};
}
