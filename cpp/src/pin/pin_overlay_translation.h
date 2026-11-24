#pragma once
#include "pin/ocr_text_layout.h"
#include "translation/overlay_translation.h"
#include "ui/translation_sweep.h"
#include <QObject>
#include <QPointer>
#include <QRect>

namespace jietuba {
class OverlayTranslationSession;
class PinToolbar;
class PinWindow;
class ToolSettingsManager;

// The pin's text with a translation laid over it, as on a capture: a layer that switches off for
// the original. The pin's own recognition is used, so its text is recognized once. While it runs
// the pin moves, zooms and takes annotations as usual; mosaic, smart erase and selecting text wait.
class PinOverlayTranslation : public QObject {
    Q_OBJECT
public:
    // Pictures larger than this many pixels repaint their layer less often.
    static constexpr qint64 LARGE_PIXELS = 4'000'000;
    static constexpr int LARGE_FLUSH_MS = 200;
    PinOverlayTranslation(PinWindow *pin, ToolSettingsManager *config);
    ~PinOverlayTranslation() override;
    // Lays the translation over the pin; shown already, retries what failed or opens and closes
    // the toolbar's panel.
    void translate();
    // A translation made on the capture the pin comes from, in the pin's pixels; with go_on the
    // paragraphs still missing are asked for.
    void adopt(const QVector<OverlayTextBlock> &blocks, bool original_shown, bool go_on);
    void stop();
    void set_original_shown(bool shown);
    bool shown() const;
    bool busy() const;
    bool original_shown() const;
    OverlayTranslationSession *session() const { return session_; }
    // The toolbar's translate button and panel follow the translation, as on a capture.
    void bind_toolbar(PinToolbar *toolbar);
    // The text to select and copy: the translation where it is shown, the recognized text elsewhere.
    void sync_text_layer();
    // The sweep's place on screen: the part of the pin that is on a screen.
    QRect sweep_area() const;
    TranslationSweepWindow *sweep() const { return sweep_; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void recognize(const std::function<void(const QVector<OverlayTextBlock> &, const QString &)> &done);
    void on_busy_changed(bool busy);
    void sync_wait();
    void sync_toolbar();
    void sync_tools();
    void place_sweep();
    void report(const QString &message);
    QRectF whole() const;

    PinWindow *pin_;
    ToolSettingsManager *config_;
    OverlayTranslationSession *session_ = nullptr;
    QPointer<TranslationSweepWindow> sweep_;
    QPointer<PinToolbar> toolbar_;
};

// The pin's text with the translation in place of what it covers: each drawn line of a translated
// paragraph, and the recognized lines no translation covers. In image pixels.
OcrResult translated_text_lines(const OcrTextLayout &recognized, const QVector<std::optional<OverlayPaintedBlock>> &planned, const QPoint &origin = {});
}
