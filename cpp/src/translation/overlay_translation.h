#pragma once
#include "ocr/ocr_manager.h"
#include <QColor>
#include <QFont>
#include <QImage>
#include <QPoint>
#include <QRectF>
#include <QString>
#include <QVector>
#include <optional>

namespace jietuba {
// One paragraph of the capture's own text and the room it takes on the image, in source
// pixels. A wrapped line continues its paragraph, a separate label or column does not.
struct OverlayTextBlock {
    QString source;
    QString translated;
    QRectF rect;
    int lines = 1;
    // The tallest of the paragraph's own line boxes: the size its text was set in. The
    // pitch between lines is larger, so the translation is fitted to this instead.
    double line_height = 0;
};

// Reading-order paragraphs from the recognized lines. Lines without a box or with no text
// are dropped, since nothing can be placed for them.
QVector<OverlayTextBlock> group_overlay_blocks(const OcrResult &result);

// A transparent layer as big as source holding each block's translation over a refill of the
// block's sampled background, shrunk until it fits. Blank translations and text that came back
// unchanged are skipped; null when nothing was drawn. painted_blocks receives the drawn count.
QImage render_translation_overlay(const QImage &source, const QVector<OverlayTextBlock> &blocks, const QString &target_lang, int *painted_blocks = nullptr);

// The same drawing done straight onto a copy of source, without a separate layer; null when
// nothing was drawn. Safe off the GUI thread.
QImage paint_translations(const QImage &source, const QVector<OverlayTextBlock> &blocks, const QString &target_lang, int *painted_blocks = nullptr);

// How one block is repainted: the cover that hides its original text, and the translation
// set over it.
struct OverlayPaintedBlock {
    QRectF area;
    QRect cover;
    QColor background;
    QColor foreground;
    QFont font;
    QString text;
};

// Where and how block index is repainted, sampled from source; nullopt for a blank
// translation, for text that came back unchanged and for a block outside the image.
std::optional<OverlayPaintedBlock> plan_overlay_block(const QImage &source, const QVector<OverlayTextBlock> &blocks, int index, const QString &target_lang);

// Every cover, then every text, so no cover hides a neighbour's translation.
void paint_overlay_blocks(QImage &target, const QVector<OverlayPaintedBlock> &painted);
}
