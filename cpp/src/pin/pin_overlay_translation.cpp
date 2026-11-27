#include "pin/pin_overlay_translation.h"
#include "canvas/items/background_item.h"
#include "canvas/scene.h"
#include "core/i18n.h"
#include "pin/pin_canvas.h"
#include "pin/pin_ocr_manager.h"
#include "pin/pin_toolbar.h"
#include "pin/pin_window.h"
#include "settings/tool_settings.h"
#include "tools/controller.h"
#include "translation/overlay_translation_session.h"
#include "ui/dialogs.h"
#include "ui/toast.h"
#include "ui/translation_panel.h"
#include "ui/translation_sweep.h"
#include <QEvent>
#include <QGuiApplication>
#include <QScreen>
#include <algorithm>

namespace jietuba {
namespace {
// The capture's wording, shared so both say the same.
QString capture_tr(const char *text) { return i18n_tr(text, "ActionTools"); }
}

OcrResult translated_text_lines(const OcrTextLayout &recognized, const QVector<std::optional<OverlayPaintedBlock>> &planned, const QPoint &origin) {
    OcrResult result;
    QVector<QRectF> covered;
    for (const auto &entry : planned) {
        if (!entry) continue;
        const QTransform to_image = entry->text_to_image * QTransform::fromTranslate(origin.x(), origin.y());
        for (qsizetype line = 0; line < entry->lines.size() && line < entry->line_rects.size(); ++line) {
            const auto &rect = entry->line_rects[line];
            result.data.append(OcrLine{{to_image.map(rect.topLeft()), to_image.map(rect.topRight()), to_image.map(rect.bottomRight()), to_image.map(rect.bottomLeft())},
                                       entry->lines[line]});
        }
        covered.append(entry->background_region.boundingRect().translated(origin));
    }
    for (const auto &line : recognized.lines()) {
        const auto centre = line.bounds.center();
        if (std::any_of(covered.cbegin(), covered.cend(), [&](const QRectF &rect) { return rect.contains(centre); })) continue;
        const auto &outline = line.outline;
        if (outline.size() < 4) continue;
        result.data.append(OcrLine{{outline[0], outline[1], outline[2], outline[3]}, line.text});
    }
    return result;
}

PinOverlayTranslation::PinOverlayTranslation(PinWindow *pin, ToolSettingsManager *config) : pin_(pin), config_(config) {
    session_ = new OverlayTranslationSession(pin->canvas ? pin->canvas->scene : nullptr, config, this);
    const auto size = pin->orig_size();
    if (static_cast<qint64>(size.width()) * size.height() > LARGE_PIXELS) session_->set_flush_interval(LARGE_FLUSH_MS);
    session_->recognize = [this](const OverlayTranslationSession::RecognizeDone &done) { recognize(done); };
    connect(session_, &OverlayTranslationSession::busy_changed, this, &PinOverlayTranslation::on_busy_changed);
    connect(session_, &OverlayTranslationSession::waiting_changed, this, [this] {
        sync_wait();
        if (toolbar_) {
            const auto [done, total] = session_->progress();
            toolbar_->set_translation_progress(done, total);
        }
    });
    connect(session_, &OverlayTranslationSession::shown_changed, this, [this](bool shown) {
        if (toolbar_) toolbar_->set_translation_shown(shown);
    });
    connect(session_, &OverlayTranslationSession::failed, this, [this](const QString &message) {
        // Nothing was laid over the pin: it is as before, and translating again starts over.
        if (std::none_of(session_->planned().cbegin(), session_->planned().cend(), [](const auto &entry) { return entry.has_value(); })) session_->hide();
        report(message);
    });
    connect(session_, &OverlayTranslationSession::partly_failed, this, &PinOverlayTranslation::report);
    connect(session_, &OverlayTranslationSession::translations_did_not_fit, this, [this](const QString &text, const QString &notice) {
        show_unplaced_overlay_translations(pin_, capture_tr("Translation"), text, notice);
    });
    connect(session_, &OverlayTranslationSession::nothing_to_translate, this, [this] {
        session_->hide();
        show_toast(capture_tr("Nothing to translate: the text is already in the target language."));
    });
    if (auto *ocr = pin_->ocr_manager()) ocr->text_loaded = [this] { sync_text_layer(); };
    pin_->installEventFilter(this);
    if (pin_->toolbar) bind_toolbar(pin_->toolbar);
}

PinOverlayTranslation::~PinOverlayTranslation() {
    if (auto *ocr = pin_->ocr_manager()) {
        ocr->text_loaded = nullptr;
        ocr->set_translating(false);
    }
    if (sweep_) delete sweep_.data();
}

QRectF PinOverlayTranslation::whole() const { return QRectF(QPointF(0, 0), QSizeF(pin_->orig_size())); }

bool PinOverlayTranslation::shown() const { return session_->shown(); }
bool PinOverlayTranslation::busy() const { return session_->busy(); }
bool PinOverlayTranslation::original_shown() const { return session_->original_shown(); }

void PinOverlayTranslation::translate() {
    if (session_->shown()) {
        const bool missing = std::any_of(session_->blocks().cbegin(), session_->blocks().cend(), [](const OverlayTextBlock &block) { return block.translated.isEmpty(); });
        if (!session_->busy() && missing) session_->show(whole());
        else if (toolbar_) toolbar_->toggle_translation_panel();
        return;
    }
    if (!session_->show(whole())) return;
    sync_toolbar();
}

void PinOverlayTranslation::adopt(const QVector<OverlayTextBlock> &blocks, bool original_shown, bool go_on) {
    if (!session_->adopt(whole(), blocks, original_shown)) return;
    if (go_on) session_->show(whole());
    // show() always puts the translation in front; the capture may have shown the original.
    session_->set_original_shown(original_shown);
    sync_toolbar();
    sync_text_layer();
    pin_->refresh_translation_for_scale();
}

void PinOverlayTranslation::stop() { session_->stop(); }

void PinOverlayTranslation::set_original_shown(bool shown) {
    session_->set_original_shown(shown);
    if (toolbar_ && toolbar_->translation_panel) toolbar_->translation_panel->set_original_shown(shown);
    sync_text_layer();
}

void PinOverlayTranslation::recognize(const std::function<void(const QVector<OverlayTextBlock> &, const QString &)> &done) {
    auto *ocr = pin_->ocr_manager();
    if (!ocr) return done({}, {});
    if (const auto *lines = ocr->recognized_lines()) return done(group_overlay_blocks(*lines), {});
    const QPointer<PinOverlayTranslation> self(this);
    const bool started = ocr->recognize_then([self, done](bool success, const QString &message) {
        if (!self) return;
        const auto *lines = self->pin_->ocr_manager() ? self->pin_->ocr_manager()->recognized_lines() : nullptr;
        if (success && lines) done(group_overlay_blocks(*lines), {});
        else done({}, message);
    });
    if (!started) done({}, i18n_tr("OCR recognition could not be started", "PinWindow"));
}

void PinOverlayTranslation::bind_toolbar(PinToolbar *toolbar) {
    if (!toolbar || toolbar_ == toolbar) return;
    toolbar_ = toolbar;
    auto *panel = toolbar->ensure_translation_panel();
    connect(panel, &TranslationPanel::original_shown_changed, this, [this](bool shown) { set_original_shown(shown); });
    connect(panel, &TranslationPanel::target_language_changed, session_, &OverlayTranslationSession::set_target_language);
    connect(panel, &TranslationPanel::provider_changed, session_, &OverlayTranslationSession::set_provider);
    sync_toolbar();
}

void PinOverlayTranslation::sync_toolbar() {
    if (!toolbar_) return;
    if (session_->shown()) {
        auto *panel = toolbar_->ensure_translation_panel();
        panel->set_original_shown(session_->original_shown());
        panel->set_providers(session_->providers(), config_ ? config_->get_translation_provider() : QString());
        panel->set_target_language(config_ ? config_->get_translation_request_params().value("target_lang").toString() : QString("ZH"));
    }
    toolbar_->set_translation_shown(session_->shown());
    toolbar_->set_translation_busy(session_->busy());
    const auto [done, total] = session_->progress();
    toolbar_->set_translation_progress(done, total);
    sync_tools();
}

void PinOverlayTranslation::on_busy_changed(bool busy) {
    if (toolbar_) toolbar_->set_translation_busy(busy);
    sync_tools();
    sync_text_layer();
    if (!busy) pin_->refresh_translation_for_scale();
}

void PinOverlayTranslation::sync_tools() {
    const bool busy = session_->busy();
    // Mosaic and smart erase read the picture under the layer, which is still changing.
    if (toolbar_ && toolbar_->mosaic_btn) toolbar_->mosaic_btn->setEnabled(!busy);
    if (busy && pin_->canvas && pin_->canvas->tool_controller && pin_->canvas->tool_controller->current_tool_id() == "mosaic") pin_->stop_drawing();
    // Half the text is translated until the run ends.
    if (auto *ocr = pin_->ocr_manager()) ocr->set_translating(busy);
}

void PinOverlayTranslation::sync_text_layer() {
    auto *ocr = pin_->ocr_manager();
    if (!ocr || session_->busy()) return;
    const auto *recognized = ocr->recognized_layout();
    if (!recognized) return;
    if (!session_->shown() || session_->original_shown()) ocr->show_layout(*recognized);
    else ocr->show_layout(OcrTextLayout(translated_text_lines(*recognized, session_->planned(), session_->region().topLeft())));
}

QRect PinOverlayTranslation::sweep_area() const {
    const QRect pin = pin_->geometry();
    QRect desktop;
    for (auto *screen : QGuiApplication::screens()) desktop = desktop.united(screen->geometry());
    return desktop.isEmpty() ? pin : pin.intersected(desktop);
}

void PinOverlayTranslation::sync_wait() {
    const auto waiting = session_->waiting();
    if (waiting.empty() || !pin_->isVisible()) {
        if (sweep_) {
            sweep_->end();
            sweep_->deleteLater();
            sweep_ = nullptr;
        }
        return;
    }
    if (!sweep_) {
        // A window of its own over the part of the pin on screen: a pin larger than the screen
        // would repaint all of itself for every frame of the band.
        sweep_ = new TranslationSweepWindow(sweep_area(), waiting.dark);
        sweep_->begin();
    }
    place_sweep();
}

void PinOverlayTranslation::place_sweep() {
    if (!sweep_) return;
    const auto area = sweep_area();
    if (sweep_->geometry() != area) sweep_->setGeometry(area);
    // Pressing the pin brings it to the front, over the band.
    if (sweep_->isVisible()) sweep_->raise();
}

bool PinOverlayTranslation::eventFilter(QObject *watched, QEvent *event) {
    if (watched == pin_) {
        switch (event->type()) {
        case QEvent::Move:
        case QEvent::Resize:
        case QEvent::WindowActivate:
        case QEvent::ZOrderChange:
            place_sweep();
            break;
        case QEvent::Show:
        case QEvent::Hide:
            sync_wait();
            break;
        default:
            break;
        }
    }
    return QObject::eventFilter(watched, event);
}

void PinOverlayTranslation::report(const QString &message) {
    if (!message.isEmpty()) show_modeless_warning_dialog(pin_, capture_tr("Translation"), message);
}
}
