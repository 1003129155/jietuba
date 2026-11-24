#pragma once
#include "translation/overlay_translation.h"
#include "translation/service.h"
#include <QHash>
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

// What a run is working on, in scene coordinates: the whole selection while it is recognized and
// translated, and whether the picture there is mostly dark. Nothing while no run goes on, or
// while the original is shown instead.
struct OverlayTranslationWait {
    QRectF rect;
    bool dark = false;
    bool empty() const { return rect.isEmpty(); }
    bool operator==(const OverlayTranslationWait &) const = default;
};

// The translation laid over one capture's selection. It is a layer on the background, not
// a change to its pixels: it can be taken off and put back, and translated again in another
// language or by another service without recognizing the capture a second time.
//
// Paragraphs appear as their requests come back. One that failed keeps its original text,
// and the next show() asks for those again. One run goes on at a time: a run asked for while a
// stopped one is still ending waits for it, and only the newest of those starts. A translation
// that came back is remembered for the service and language it came from, so a paragraph met
// again, as when the selection moves a little, is not asked for twice.
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
    // The selection is moving: the layer comes off and the run stops, but the translation stays
    // on, for the show() that follows where the selection ends.
    void suspend();
    // Stops the requests; what is translated stays shown, and show() asks for the rest.
    void stop();
    // Lays blocks translated elsewhere over selection without asking for anything, as when a
    // capture's translation goes on to its pin. False when there is nothing to lay.
    bool adopt(const QRectF &selection, const QVector<OverlayTextBlock> &blocks, bool original_shown);
    bool shown() const { return shown_; }
    bool busy() const { return !worker_.isNull() || run_waiting_ || recognizing_; }
    // A larger picture repaints its layer less often.
    void set_flush_interval(int ms) { flush_timer_.setInterval(ms); }

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
    // How each block is drawn, once its translation is in.
    const QVector<std::optional<OverlayPaintedBlock>> &planned() const { return planned_; }
    OverlayTranslationWait waiting() const;
    // Paragraphs translated and in all while a run goes on; in all is 0 until the selection is
    // recognized.
    std::pair<int, int> progress() const;
    // Replaces the service the requests go through.
    std::function<std::shared_ptr<TranslationService>()> make_service;
    // Recognizes the picture in place of the worker, for a picture whose text is read elsewhere
    // already: done gets its paragraphs, or none and why. It may answer at once.
    using RecognizeDone = std::function<void(const QVector<OverlayTextBlock> &blocks, const QString &error)>;
    std::function<void(RecognizeDone done)> recognize;

signals:
    // The layer was drawn again.
    void layer_changed();
    void shown_changed(bool shown);
    void busy_changed(bool busy);
    // waiting() may have changed.
    void waiting_changed();
    // The run ended without a translation.
    void failed(const QString &message);
    // The run ended with some paragraphs left as they were.
    void partly_failed(const QString &notice);
    void translations_did_not_fit(const QString &text, const QString &notice);
    // The run ended and no paragraph differs from its source.
    void nothing_to_translate();

private:
    void run();
    void start_worker();
    void on_recognized(const QVector<OverlayTextBlock> &blocks, const QString &error);
    void cancel();
    // Stops the current run: its worker is kept until its thread ends.
    void retire_worker();
    void start_waiting_run();
    // busy_changed when busy() differs from what was last told, then waiting_changed: once at
    // the end of each outside call, so stopping one run and queueing the next is not told as
    // two changes.
    void notify();
    // Forgets the translations and the layer; the recognized paragraphs stay.
    void clear_translations();
    void flush();
    // Draws the whole layer again.
    void apply_layer();
    // Draws again only where the blocks in changed fall, before and after.
    void repaint_layer(const QRegion &changed);
    // Puts the layer on the background when changed, and shows it unless the original is.
    void show_layer(bool changed);
    void on_finished(OverlayTranslationWorker *worker, bool success);

    QPointer<CanvasScene> scene_;
    ToolSettingsManager *config_;
    QRect region_;
    QImage source_;
    // Whether the source is mostly dark.
    bool dark_ = false;
    QVector<OverlayTextBlock> blocks_;
    // Per block how it is drawn, once its translation is in.
    QVector<std::optional<OverlayPaintedBlock>> planned_;
    QVector<int> dirty_;
    QSet<int> unplaced_;
    QString target_lang_;
    QTimer flush_timer_;
    QPointer<OverlayTranslationWorker> worker_;
    // A stopped run whose thread has not ended yet, and whether a run waits for it.
    QPointer<OverlayTranslationWorker> stopping_;
    bool run_waiting_ = false;
    // recognize() was asked and has not answered; an answer for an earlier count is dropped.
    bool recognizing_ = false;
    quint64 recognition_ = 0;
    QImage layer_;
    bool busy_told_ = false;
    // Translations that came back, per service and target language, by source text.
    QHash<QString, QHash<QString, QString>> remembered_;
    QString remembered_scope_;
    std::shared_ptr<TranslationService> service_;
    bool shown_ = false;
    bool original_shown_ = false;
};
}
