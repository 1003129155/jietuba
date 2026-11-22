#include "translation/overlay_translation_session.h"
#include "canvas/items/background_item.h"
#include "canvas/scene.h"
#include "core/ui_scale.h"
#include "core/logger.h"
#include "settings/tool_settings.h"
#include "translation/overlay_translation_worker.h"
#include <algorithm>

namespace jietuba {
OverlayTranslationSession::OverlayTranslationSession(CanvasScene *scene, ToolSettingsManager *config, QObject *parent)
    : QObject(parent), scene_(scene), config_(config) {
    make_service = [] { return create_default_translation_service(); };
    flush_timer_.setSingleShot(true);
    flush_timer_.setInterval(FLUSH_INTERVAL_MS);
    connect(&flush_timer_, &QTimer::timeout, this, &OverlayTranslationSession::flush);
}

OverlayTranslationSession::~OverlayTranslationSession() { cancel(); }

QRect OverlayTranslationSession::region_of(const QRectF &selection) const {
    if (!scene_ || !scene_->background) return {};
    // The background image starts at the scene's top-left, which is not the origin on a
    // multi-monitor desktop.
    const auto origin = scene_->sceneRect().topLeft();
    const QRect wanted(static_cast<int>(python_round(selection.x() - origin.x())), static_cast<int>(python_round(selection.y() - origin.y())),
                       static_cast<int>(python_round(selection.width())), static_cast<int>(python_round(selection.height())));
    return wanted.intersected(QRect(QPoint(0, 0), scene_->background->pixmap().size()));
}

QList<QPair<QString, QString>> OverlayTranslationSession::providers() const {
    QList<QPair<QString, QString>> result;
    const auto service = make_service();
    if (!service) return result;
    const auto current = service->active_provider_id();
    for (const auto &metadata : service->registry->available_providers()) {
        if (metadata.provider_id != current && !service->is_configured(metadata.provider_id)) continue;
        QString name = metadata.display_name;
        try {
            name = service->provider_label({}, metadata.provider_id);
        } catch (const std::exception &) {
        }
        result.append({metadata.provider_id, name});
    }
    return result;
}

bool OverlayTranslationSession::show(const QRectF &selection) {
    if (!scene_ || !scene_->background) return false;
    const auto background = scene_->background->image();
    if (background.isNull()) return false;
    const auto region = region_of(selection);
    if (region.isEmpty()) return false;
    if (region != region_) {
        cancel();
        region_ = region;
        source_ = background.copy(region);
        dark_ = mostly_dark(source_, source_.rect());
        blocks_.clear();
        clear_translations();
    }
    original_shown_ = false;
    if (!shown_) {
        shown_ = true;
        emit shown_changed(true);
    }
    apply_layer();
    const bool missing = std::any_of(blocks_.cbegin(), blocks_.cend(), [](const OverlayTextBlock &block) { return block.translated.isEmpty(); });
    if (!busy() && (blocks_.isEmpty() || missing)) run();
    notify();
    return true;
}

void OverlayTranslationSession::hide() {
    cancel();
    if (scene_ && scene_->background) scene_->background->set_overlay_visible(false);
    if (shown_) {
        shown_ = false;
        emit shown_changed(false);
    }
    notify();
}

void OverlayTranslationSession::suspend() {
    cancel();
    if (scene_ && scene_->background) scene_->background->set_overlay_visible(false);
    notify();
}

void OverlayTranslationSession::set_original_shown(bool shown) {
    if (original_shown_ == shown) return;
    original_shown_ = shown;
    apply_layer();
    notify();
}

void OverlayTranslationSession::set_target_language(const QString &code) {
    if (!config_ || code.isEmpty() || config_->get_translation_target_lang() == code) return;
    config_->set_translation_target_lang(code);
    clear_translations();
    if (shown_) run();
    notify();
}

void OverlayTranslationSession::set_provider(const QString &provider_id) {
    if (!config_ || provider_id.isEmpty() || config_->get_translation_provider() == provider_id) return;
    config_->set_translation_provider(provider_id);
    clear_translations();
    if (shown_) run();
    notify();
}

void OverlayTranslationSession::notify() {
    if (busy() != busy_told_) {
        busy_told_ = busy();
        emit busy_changed(busy_told_);
    }
    emit waiting_changed();
}

void OverlayTranslationSession::run() {
    retire_worker();
    if (stopping_) {
        // OCR cannot be stopped midway: starting beside it would only add a thread doing the same.
        run_waiting_ = true;
        return;
    }
    run_waiting_ = false;
    const auto params = config_ ? config_->get_translation_request_params() : QVariantMap{{"target_lang", "ZH"}};
    target_lang_ = params.value("target_lang", "ZH").toString();
    service_ = make_service();
    // The service chosen in the settings and the one the service resolved: the same in practice.
    remembered_scope_ = QStringList{config_ ? config_->get_translation_provider() : QString(), service_ ? service_->active_provider_id() : QString(), target_lang_}.join(u'\x1f');
    // Without a parent: it ends on its own thread and may outlive this object.
    auto *worker = new OverlayTranslationWorker(source_, params, service_);
    if (!blocks_.isEmpty()) worker->set_blocks(blocks_);
    worker->set_known_translations(remembered_.value(remembered_scope_));
    worker->set_paint_result(false);
    worker_ = worker;
    // A worker that was replaced is disconnected; a reply it queued before that is dropped
    // here too, so nothing of an earlier selection lands on this one.
    connect(worker, &OverlayTranslationWorker::recognized, this, [this, worker](const QVector<OverlayTextBlock> &blocks) {
        if (worker != worker_) return;
        blocks_ = blocks;
        planned_.fill(std::nullopt, blocks_.size());
        notify();
    });
    connect(worker, &OverlayTranslationWorker::block_translated, this, [this, worker](int index, const QString &translation) {
        if (worker != worker_ || index < 0 || index >= blocks_.size()) return;
        blocks_[index].translated = translation;
        if (!translation.isEmpty()) remembered_[remembered_scope_].insert(blocks_[index].source, translation);
        dirty_.append(index);
        if (!flush_timer_.isActive()) flush_timer_.start();
    });
    connect(worker, &OverlayTranslationWorker::finished_signal, this, [this, worker](bool success) { on_finished(worker, success); });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void OverlayTranslationSession::retire_worker() {
    flush_timer_.stop();
    if (!worker_) return;
    auto *worker = worker_.data();
    worker_ = nullptr;
    // Nothing it sends from now on is this session's business, but when its thread ends a
    // waiting run may start.
    disconnect(worker, nullptr, this, nullptr);
    worker->abort();
    stopping_ = worker;
    connect(worker, &QThread::finished, this, &OverlayTranslationSession::start_waiting_run);
}

void OverlayTranslationSession::start_waiting_run() {
    stopping_ = nullptr;
    if (run_waiting_) run();
    notify();
}

void OverlayTranslationSession::cancel() {
    retire_worker();
    run_waiting_ = false;
}

void OverlayTranslationSession::clear_translations() {
    cancel();
    for (auto &block : blocks_) block.translated.clear();
    planned_.fill(std::nullopt, blocks_.size());
    dirty_.clear();
    unplaced_.clear();
    apply_layer();
}

void OverlayTranslationSession::flush() {
    flush_timer_.stop();
    if (dirty_.isEmpty()) return;
    for (const int index : std::as_const(dirty_)) {
        if (index >= planned_.size()) continue;
        bool does_not_fit = false;
        planned_[index] = plan_overlay_block(source_, blocks_, index, target_lang_, &does_not_fit);
        if (does_not_fit) unplaced_.insert(index);
    }
    dirty_.clear();
    apply_layer();
    notify();
}

void OverlayTranslationSession::apply_layer() {
    if (!scene_ || !scene_->background) return;
    QVector<OverlayPaintedBlock> painted;
    for (const auto &entry : std::as_const(planned_))
        if (entry) painted.append(*entry);
    QImage layer;
    if (!painted.isEmpty()) {
        layer = QImage(source_.size(), QImage::Format_ARGB32_Premultiplied);
        layer.fill(Qt::transparent);
        paint_overlay_blocks(layer, painted);
    }
    scene_->background->set_overlay(layer, region_.topLeft());
    scene_->background->set_overlay_visible(shown_ && !original_shown_);
}

OverlayTranslationWait OverlayTranslationSession::waiting() const {
    if (!busy() || !shown_ || original_shown_ || !scene_) return {};
    return {QRectF(scene_->sceneRect().topLeft() + QPointF(region_.topLeft()), QSizeF(region_.size())), dark_};
}

std::pair<int, int> OverlayTranslationSession::progress() const {
    if (!busy()) return {0, 0};
    const auto done = std::count_if(blocks_.cbegin(), blocks_.cend(), [](const OverlayTextBlock &block) { return !block.translated.isEmpty(); });
    return {static_cast<int>(done), static_cast<int>(blocks_.size())};
}

void OverlayTranslationSession::on_finished(OverlayTranslationWorker *worker, bool success) {
    if (worker != worker_) return;
    worker_ = nullptr;
    flush();
    notify();
    if (!success) {
        emit failed(worker->error_text());
        return;
    }
    // The monthly quota is counted here: the settings it reads belong to the GUI thread.
    try {
        if (service_) service_->record_usage(*service_->provider(), worker->translated_chars());
    } catch (const std::exception &) {
    }
    const auto notice = worker->failure_notice();
    const bool drawn = std::any_of(planned_.cbegin(), planned_.cend(), [](const std::optional<OverlayPaintedBlock> &entry) { return entry.has_value(); });
    log_info(T("覆盖翻译完成: {count} 个段落已替换", {{"count", static_cast<int>(std::count_if(planned_.cbegin(), planned_.cend(), [](const auto &entry) { return entry.has_value(); }))}}),
             "ScreenshotTranslate");
    auto indices = unplaced_.values();
    std::sort(indices.begin(), indices.end());
    // With nothing drawn at all, even a short label that did not fit is told about.
    const auto unplaced = drawn ? unplaced_notice(blocks_, indices) : format_unplaced_translations(blocks_, indices);
    if (!unplaced.isEmpty()) emit translations_did_not_fit(unplaced, notice);
    else if (!notice.isEmpty()) emit partly_failed(notice);
    else if (!drawn) emit nothing_to_translate();
}
}
