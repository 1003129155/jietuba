#pragma once
#include "ocr/ocr_manager.h"
#include <QColor>
#include <QFont>
#include <QImage>
#include <QPoint>
#include <QPolygonF>
#include <QRectF>
#include <QRegion>
#include <QString>
#include <QStringList>
#include <QTransform>
#include <QVector>
#include <optional>

namespace jietuba {
struct OverlayOrientedLine {
    QPolygonF quad;
    QSizeF size;
    QTransform to_image;
    bool vertical = false;
};

// One paragraph of the capture's own text and the room it takes on the image, in source
// pixels. A wrapped line continues its paragraph, a separate label or column does not.
struct OverlayTextBlock {
    // The paragraph as one text: its lines joined the way they read, so a sentence that
    // wraps is translated as a sentence.
    QString source;
    // The text of each line, as recognized.
    QStringList line_texts;
    QString translated;
    QRectF rect;
    // The box of each of its lines, in the order of source's lines.
    QVector<QRectF> line_rects;
    int lines = 1;
    // The tallest of the paragraph's own line boxes: the size its text was set in. The
    // pitch between lines is larger, so the translation is fitted to this instead.
    double line_height = 0;
    std::optional<OverlayOrientedLine> oriented_line;
};

// Reading-order paragraphs from the recognized lines. Lines without a box or with no text
// are dropped, since nothing can be placed for them.
QVector<OverlayTextBlock> group_overlay_blocks(const OcrResult &result);

// A transparent layer as big as source holding each block's translation over a refill of the
// block's sampled background. Text that is unchanged or cannot fit at a readable size is
// skipped. Null when nothing was drawn. painted_blocks receives the drawn count.
QImage render_translation_overlay(const QImage &source, const QVector<OverlayTextBlock> &blocks, const QString &target_lang, int *painted_blocks = nullptr);

// The same drawing done straight onto a copy of source, without a separate layer; null when
// nothing was drawn. unplaced receives the indices of translations that did not fit.
// Safe off the GUI thread.
QImage paint_translations(const QImage &source, const QVector<OverlayTextBlock> &blocks, const QString &target_lang,
                          int *painted_blocks = nullptr, QVector<int> *unplaced = nullptr);
QString format_unplaced_translations(const QVector<OverlayTextBlock> &blocks, const QVector<int> &unplaced);
// The notice for the translations that did not fit, all of them, or nothing when they are all
// short labels: one line of at most three words or six CJK characters. On a dense page a few
// labels nearly always fail to fit; they stay as they are without a notice of their own.
QString unplaced_notice(const QVector<OverlayTextBlock> &blocks, const QVector<int> &unplaced);

// How one block is repainted: the cover that hides its original text, and the translation
// set over it line by line.
struct OverlayPaintedBlock {
    QRectF area;
    QRect cover;
    QRegion background_region;
    QColor background;
    QColor foreground;
    QFont font;
    QString text;
    // text as it is drawn: each line and the row it is centred in.
    QStringList lines;
    QVector<QRectF> line_rects;
    QVector<QPointF> line_origins;
    QTransform text_to_image;
    Qt::Alignment alignment = Qt::AlignLeft;
};

// Where and how block index is repainted, sampled from source; nullopt for a blank
// translation, unchanged text, a block outside the image or a translation that does not fit.
// does_not_fit distinguishes the last case from a block that needs no result.
std::optional<OverlayPaintedBlock> plan_overlay_block(const QImage &source, const QVector<OverlayTextBlock> &blocks, int index, const QString &target_lang,
                                                    bool *does_not_fit = nullptr);

// Every cover, then every text, so no cover hides a neighbour's translation.
void paint_overlay_blocks(QImage &target, const QVector<OverlayPaintedBlock> &painted);
}
