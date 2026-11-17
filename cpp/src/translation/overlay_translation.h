#pragma once
#include "ocr/ocr_manager.h"
#include <QImage>
#include <QPoint>
#include <QRectF>
#include <QString>
#include <QVector>

namespace jietuba {
class CanvasScene;

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

// A copy of source with each block's translation painted over the original text: the
// block's background colour is sampled and refilled, then the translation is drawn in it,
// shrunk until it fits. Blank translations and text that came back unchanged are left
// alone, so the layer stays transparent there. painted_blocks, when given, receives how
// many blocks were actually drawn.
QImage render_translation_overlay(const QImage &source, const QVector<OverlayTextBlock> &blocks, const QString &target_lang, int *painted_blocks = nullptr);

// Puts the layer, which is as big as the selection and transparent outside the rewritten
// text, onto the scene's background at offset, as one undoable step: undo brings the
// original pixels back.
void apply_translation_overlay(CanvasScene *scene, const QImage &layer, const QPoint &offset, const QString &command_text);
}
