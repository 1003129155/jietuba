#pragma once
#include "translation/overlay_translation.h"
#include "translation/service.h"
#include <QImage>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QRect>
#include <QSet>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>

namespace jietuba {
class CanvasScene;
class OverlayTranslationWorker;
class ToolSettingsManager;

// The translation laid over one capture's selection. It is a layer on the background, not
// a change to its pixels: it can be taken off and put back, and translated again in another
// language or by another service without recognizing the capture a second time.
//
// Paragraphs appear as their requests come back. One that failed keeps its original text,
// and the next show() asks for those again.
class OverlayTranslationSession : public QObject {
    Q_OBJECT
public:
    // Updates of the layer are gathered for this long, so a burst of replies repaints once.
    static constexpr int FLUSH_INTERVAL_MS = 33;
    OverlayTranslationSession(CanvasScene *scene, ToolSettingsManager *config, QObject *parent = nullptr);
    ~OverlayTranslationSession() override;

    // Lays the translation of selection (scene coordinates) over it. Another selection
    // than the last starts over; the same one only asks for what is still missing. False
    // when the selection has no pixels.
    bool show(const QRectF &selection);
    // Takes the layer off and stops the requests. What is translated stays for show().
    void hide();
    bool shown() const { return shown_; }
    bool busy() const { return !worker_.isNull(); }

    // The original picture instead of the layer, while the translation stays on.
    void set_original_shown(bool shown);
    bool original_shown() const { return original_shown_; }
    // Saved as the setting, then everything is translated again; nothing when unchanged.
    void set_target_language(const QString &code);
    void set_provider(const QString &provider_id);

    // The selection in background pixels that the layer covers; empty before show().
    QRect region() const { return region_; }
    // Where selection (scene coordinates) falls in background pixels.
    QRect region_of(const QRectF &selection) const;
    // The services to choose from, as (id, name): the configured ones and the current one.
    QList<QPair<QString, QString>> providers() const;
    const QVector<OverlayTextBlock> &blocks() const { return blocks_; }
    // Replaces the service the requests go through.
    std::function<std::shared_ptr<TranslationService>()> make_service;

signals:
    void shown_changed(bool shown);
    void busy_changed(bool busy);
    // total is 0 while the count is not known yet (recognition).
    void progress_changed(int done, int total);
    // The run ended without a translation.
    void failed(const QString &message);
    // The run ended with some paragraphs left as they were.
    void partly_failed(const QString &notice);
    void translations_did_not_fit(const QString &text, const QString &notice);
    // The run ended and no paragraph differs from its source.
    void nothing_to_translate();

private:
    void run();
    void cancel();
    // Forgets the translations and the layer; the recognized paragraphs stay.
    void clear_translations();
    void flush();
    void apply_layer();
    void on_finished(OverlayTranslationWorker *worker, bool success);

    QPointer<CanvasScene> scene_;
    ToolSettingsManager *config_;
    QRect region_;
    QImage source_;
    QVector<OverlayTextBlock> blocks_;
    // Per block how it is drawn, once its translation is in.
    QVector<std::optional<OverlayPaintedBlock>> planned_;
    QVector<int> dirty_;
    QSet<int> unplaced_;
    QString target_lang_;
    QTimer flush_timer_;
    QPointer<OverlayTranslationWorker> worker_;
    std::shared_ptr<TranslationService> service_;
    bool shown_ = false;
    bool original_shown_ = false;
};
}
