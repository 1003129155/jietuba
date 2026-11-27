#include "scripted_translation.h"
#include "canvas/items/background_item.h"
#include "canvas/scene.h"
#include "core/export.h"
#include "ocr/ocr_manager.h"
#include "settings/tool_settings.h"
#include "tools/action.h"
#include "translation/overlay_translation.h"
#include "translation/overlay_translation_session.h"
#include "translation/overlay_translation_worker.h"
#include "translation/registry.h"
#include "ui/overlay_translation_indicator.h"
#include "ui/translation_sweep.h"
#include <QColor>
#include <QGraphicsView>
#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QtMath>
#include <atomic>
#include <mutex>
#include <cmath>

using namespace jietuba;

namespace {
// The overlay picks its own face; the fake page below draws in Arial so the two are not the
// same font and an unchanged pixel really means unchanged.
QString page_family() {
    static const QString family = [] {
        const int id = QFontDatabase::addApplicationFont(QDir(qEnvironmentVariable("WINDIR")).filePath("Fonts/arial.ttf"));
        return QFontDatabase::applicationFontFamilies(id).value(0);
    }();
    return family;
}

OcrLine line(const QString &text, const QRectF &box) {
    OcrLine item;
    item.text = text;
    item.box = {box.topLeft(), box.topRight(), box.bottomRight(), box.bottomLeft()};
    return item;
}

OcrResult result_of(const QVector<OcrLine> &lines) {
    OcrResult result;
    result.code = 100;
    result.data = lines;
    return result;
}

OverlayTextBlock block_of(const QString &source, const QString &translated, const QRectF &rect, double line_height, int lines = 1) {
    OverlayTextBlock block;
    block.source = source;
    block.translated = translated;
    block.rect = rect;
    block.line_height = line_height;
    block.lines = lines;
    return block;
}

OverlayTextBlock line_block(const QString &source, const QString &translated, const QRectF &rect) {
    auto block = block_of(source, translated, rect, rect.height());
    block.line_texts = {source};
    block.line_rects = {rect};
    return block;
}

bool inks_meet(const OverlayPaintedBlock &a, const OverlayPaintedBlock &b) {
    const auto others = overlay_text_ink(b);
    for (const auto &one : overlay_text_ink(a))
        for (const auto &two : others)
            if (one.intersects(two)) return true;
    return false;
}

// A page of one colour with the given lines already written on it, so the overlay has real
// background and ink to sample.
QImage page(const QSize &size, const QColor &background, const QColor &ink, const QVector<QPair<QString, QRectF>> &texts) {
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(background);
    QPainter painter(&image);
    painter.setPen(ink);
    for (const auto &text : texts) {
        QFont font(page_family());
        font.setPixelSize(std::max(4, static_cast<int>(std::lround(text.second.height() * 0.72))));
        painter.setFont(font);
        painter.drawText(text.second, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, text.first);
    }
    return image;
}

// The layer laid over the source the way applying it does, for comparing against the source.
QImage composited(const QImage &source, const QImage &layer) {
    QImage result = source;
    QPainter painter(&result);
    painter.drawImage(QPoint(0, 0), layer);
    return result;
}

int differing_pixels(const QImage &left, const QImage &right, const QRect &area) {
    int count = 0;
    for (int y = area.top(); y < area.bottom(); ++y)
        for (int x = area.left(); x < area.right(); ++x)
            if (left.pixel(x, y) != right.pixel(x, y)) ++count;
    return count;
}

// What the scene shows, layer and all.
QImage rendered(CanvasScene &scene) {
    QImage image(scene.sceneRect().size().toSize(), QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    scene.render(&painter, QRectF(image.rect()), scene.sceneRect());
    return image;
}

bool near(const QColor &color, const QColor &expected, int tolerance = 8) {
    return std::abs(color.red() - expected.red()) <= tolerance && std::abs(color.green() - expected.green()) <= tolerance &&
           std::abs(color.blue() - expected.blue()) <= tolerance;
}
}

class TranslationOverlayTests : public QObject {
    Q_OBJECT
private slots:
    void wrapsALineIntoItsParagraph() {
        const auto blocks = group_overlay_blocks(result_of({
            line("The quick brown fox", QRectF(10, 10, 300, 14)),
            line("jumps over the lazy dog", QRectF(10, 26, 280, 14)),
            line("and keeps running.", QRectF(12, 42, 260, 14)),
        }));
        QCOMPARE(blocks.size(), 1);
        QCOMPARE(blocks[0].lines, 3);
        // One text for the translation request, lines kept apart.
        // One text, read as the sentence it is; the lines themselves are kept beside it.
        QCOMPARE(blocks[0].source, QString("The quick brown fox jumps over the lazy dog and keeps running."));
        QCOMPARE(blocks[0].line_texts, QStringList({"The quick brown fox", "jumps over the lazy dog", "and keeps running."}));
        QCOMPARE(blocks[0].line_rects.size(), 3);
        // The paragraph covers all three lines.
        QCOMPARE(blocks[0].rect, QRectF(10, 10, 300, 46));
        QCOMPARE(blocks[0].line_height, 14.0);
    }

    void keepsCloseMenuLabelsApartAndStillJoinsProse() {
        const auto blocks = group_overlay_blocks(result_of({
            line("Share", QRectF(20, 16, 40, 14)),
            line("Copy link", QRectF(20, 34, 66, 14)),
            line("Save", QRectF(20, 52, 34, 14)),
            line("Open the", QRectF(20, 90, 64, 14)),
            line("settings window.", QRectF(20, 108, 115, 14)),
        }));
        QCOMPARE(blocks.size(), 4);
        QCOMPARE(blocks[0].source, QString("Share"));
        QCOMPARE(blocks[1].source, QString("Copy link"));
        QCOMPARE(blocks[2].source, QString("Save"));
        QCOMPARE(blocks[3].source, QString("Open the settings window."));
        QCOMPARE(blocks[3].line_rects, (QVector<QRectF>{QRectF(20, 90, 64, 14), QRectF(20, 108, 115, 14)}));
    }

    void tiltedTextFollowsItsOriginalAngleAndLeavesAnIconAlone() {
        const QRectF local(0, 0, 148, 24);
        QTransform placement;
        placement.translate(48, 44);
        placement.rotate(30);
        auto tilted = line("Settings", local);
        for (auto &point : tilted.box) point = placement.map(point);
        auto blocks = group_overlay_blocks(result_of({tilted}));
        QCOMPARE(blocks.size(), 1);
        blocks[0].translated = QStringLiteral("Préférences");

        const QRect icon(131, 54, 19, 13);
        QVERIFY(blocks[0].rect.contains(icon));
        QImage source(225, 155, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::white);
        {
            QPainter painter(&source);
            painter.fillRect(icon, QColor(181, 52, 85));
            painter.setWorldTransform(placement);
            QFont font(page_family());
            font.setPixelSize(22);
            painter.setFont(font);
            painter.drawText(local, Qt::AlignLeft | Qt::AlignVCenter, tilted.text);
        }
        const auto plan = plan_overlay_block(source, blocks, 0, "FR");
        QVERIFY(plan.has_value());
        const auto forward = plan->text_to_image.map(QPointF(1, 0)) - plan->text_to_image.map(QPointF(0, 0));
        QVERIFY(std::abs(qRadiansToDegrees(std::atan2(forward.y(), forward.x())) - 30) < 0.1);
        QVERIFY(!plan->background_region.intersects(icon));
        const auto layer = render_translation_overlay(source, blocks, "FR");
        QVERIFY(!layer.isNull());
        const auto rendered = composited(source, layer);
        QCOMPARE(differing_pixels(rendered, source, icon), 0);
        QVERIFY(differing_pixels(rendered, source, blocks[0].rect.toAlignedRect()) > 50);
        QCOMPARE(paint_translations(source, blocks, "FR"), rendered);
    }

    void skewedTextMapsToItsCornersAndKeepsTheOverflowFallback() {
        const QVector<QPointF> quad{{30, 30}, {156, 49}, {145, 76}, {28, 52}};
        auto skewed = line("Open", {});
        skewed.box = quad;
        auto blocks = group_overlay_blocks(result_of({skewed}));
        QCOMPARE(blocks.size(), 1);
        QImage source(210, 105, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::white);
        blocks[0].translated = "Ouvrir";
        const auto plan = plan_overlay_block(source, blocks, 0, "FR");
        QVERIFY(plan.has_value());
        QVERIFY(!plan->text_to_image.isAffine());
        const auto corners = plan->text_to_image.map(QPolygonF(plan->area));
        for (int index = 0; index < quad.size(); ++index)
            QVERIFY(QLineF(corners.at(index), quad.at(index)).length() < 0.1);

        blocks[0].translated = QString("Open the file in another application ").repeated(12);
        QVector<int> unplaced;
        QVERIFY(paint_translations(source, blocks, "FR", nullptr, &unplaced).isNull());
        QCOMPARE(unplaced, QVector<int>{0});
    }

    void verticalTextKeepsGraphemesUprightAndMovesDownTheColumn() {
        auto column = line(QStringLiteral("設定"), QRectF(50, 15, 26, 66));
        column.vertical = true;
        auto blocks = group_overlay_blocks(result_of({column}));
        QCOMPARE(blocks.size(), 1);
        const auto accented = QStringLiteral("A\u0301");
        blocks[0].translated = accented + u'B';
        QImage source(100, 110, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::white);
        const auto plan = plan_overlay_block(source, blocks, 0, "EN");
        QVERIFY(plan.has_value());
        QCOMPARE(plan->lines, (QStringList{accented, "B"}));
        QVERIFY(!plan->text_to_image.isRotating());
        const QFontMetricsF metrics(plan->font);
        const auto first = metrics.tightBoundingRect(plan->lines[0]).translated(plan->line_origins[0]).center();
        const auto second = metrics.tightBoundingRect(plan->lines[1]).translated(plan->line_origins[1]).center();
        QCOMPARE(first.x(), second.x());
        QCOMPARE(second.y() - first.y(), 33.0);
        const auto layer = render_translation_overlay(source, blocks, "EN");
        QVERIFY(!layer.isNull());
        QVERIFY(differing_pixels(source, composited(source, layer), blocks[0].rect.toAlignedRect()) > 50);
    }

    void joinsLinesTheWayTheirScriptReads() {
        // No space where the script has none, and a word the line end hyphenated is whole again.
        auto blocks = group_overlay_blocks(result_of({
            line(QString::fromUtf8("截图后可以直接"), QRectF(10, 10, 200, 14)),
            line(QString::fromUtf8("在图片上标注。"), QRectF(10, 26, 200, 14)),
        }));
        QCOMPARE(blocks.size(), 1);
        QCOMPARE(blocks[0].source, QString::fromUtf8("截图后可以直接在图片上标注。"));
        blocks = group_overlay_blocks(result_of({
            line("Release the mouse to con-", QRectF(10, 10, 300, 14)),
            line("firm the selection", QRectF(10, 26, 220, 14)),
        }));
        QCOMPARE(blocks.size(), 1);
        QCOMPARE(blocks[0].source, QString("Release the mouse to confirm the selection"));
    }

    // Items of a list follow each other as closely as a paragraph's lines do, and are not one.
    void keepsListItemsApart() {
        auto blocks = group_overlay_blocks(result_of({
            line("1. Open the settings page.", QRectF(10, 10, 300, 14)),
            line("2. Choose a save folder.", QRectF(10, 26, 280, 14)),
            line("which is used from then on", QRectF(10, 42, 290, 14)),
            line(QString::fromUtf8("3、保存"), QRectF(10, 58, 80, 14)),
            line(QString::fromUtf8("• Pin the picture"), QRectF(10, 74, 200, 14)),
            line("- Close it", QRectF(10, 90, 120, 14)),
        }));
        QCOMPARE(blocks.size(), 5);
        QCOMPARE(blocks[1].source, QString("2. Choose a save folder. which is used from then on"));
        QCOMPARE(blocks[1].lines, 2);
        // A paragraph that ended on a short last line is not continued either.
        blocks = group_overlay_blocks(result_of({
            line("The first paragraph runs the full width of the", QRectF(10, 10, 400, 14)),
            line("column and ends here.", QRectF(10, 26, 180, 14)),
            line("The second one starts at the same margin.", QRectF(10, 42, 380, 14)),
        }));
        QCOMPARE(blocks.size(), 2);
        QCOMPARE(blocks[1].source, QString("The second one starts at the same margin."));
    }

    void keepsAHeadingApartFromTheTextBelow() {
        // A heading is set larger, so its height does not match the body's: same margin and
        // a normal gap, and still a block of its own.
        const auto blocks = group_overlay_blocks(result_of({
            line("Installation", QRectF(10, 10, 180, 28)),
            line("Run the installer and pick a folder.", QRectF(10, 44, 300, 14)),
        }));
        QCOMPARE(blocks.size(), 2);
        QCOMPARE(blocks[0].source, QString("Installation"));
        QCOMPARE(blocks[0].line_height, 28.0);
        QCOMPARE(blocks[1].source, QString("Run the installer and pick a folder."));
    }

    void splitsOnAGap() {
        // Two lines far apart are a label and a body, not one paragraph.
        const auto blocks = group_overlay_blocks(result_of({
            line("Username", QRectF(10, 10, 300, 14)),
            line("Enter the name you signed up with.", QRectF(10, 60, 300, 14)),
        }));
        QCOMPARE(blocks.size(), 2);
    }

    void splitsOnADifferentMargin() {
        // An indented line starts something else even when it sits right below.
        const auto blocks = group_overlay_blocks(result_of({
            line("Username", QRectF(10, 10, 70, 14)),
            line("Enter the name you signed up with.", QRectF(60, 26, 250, 14)),
        }));
        QCOMPARE(blocks.size(), 2);
    }

    void aCaptionBesideAParagraphDoesNotBreakIt() {
        // Two captions in a narrow column fall between the paragraph's lines in height, and come first in
        // recognition order. Few lines make a column, so no gutter is found: read top down, they would cut it.
        QVector<OcrLine> lines{line("caption a", QRectF(400, 18, 100, 14)), line("caption b", QRectF(400, 66, 100, 14))};
        for (int index = 0; index < 6; ++index) lines.append(line(QString("paragraph line %1").arg(index), QRectF(10, 10 + index * 16, 300, 14)));
        const auto blocks = group_overlay_blocks(result_of(lines));
        QCOMPARE(blocks.size(), 3);
        QCOMPARE(blocks[0].lines, 6);
        QCOMPARE(blocks[0].source, QString("paragraph line 0 paragraph line 1 paragraph line 2 paragraph line 3 paragraph line 4 paragraph line 5"));
        QCOMPARE(blocks[1].source, QString("caption a"));
        QCOMPARE(blocks[2].source, QString("caption b"));
    }

    void anIndentedFirstLineStaysInItsParagraph() {
        const auto blocks = group_overlay_blocks(result_of({
            line(QString::fromUtf8("截图后可以直接在图片上标注，也可以先把它钉在屏幕上"), QRectF(40, 10, 260, 14)),
            line(QString::fromUtf8("再慢慢处理，标注的内容会一直保留在图片里面。"), QRectF(10, 26, 280, 14)),
            line(QString::fromUtf8("保存时可以选择格式。"), QRectF(10, 42, 150, 14)),
        }));
        QCOMPARE(blocks.size(), 1);
        QCOMPARE(blocks[0].lines, 3);
        QCOMPARE(blocks[0].rect, QRectF(10, 10, 290, 46));
    }

    void keepsSideBySideColumnsApart() {
        // Same row, two columns: the second is read after the first, and is not its
        // continuation (its top is above the first line's bottom).
        const auto blocks = group_overlay_blocks(result_of({
            line("Name", QRectF(10, 10, 100, 14)),
            line("Value", QRectF(400, 10, 100, 14)),
        }));
        QCOMPARE(blocks.size(), 2);
        QCOMPARE(blocks[0].source, QString("Name"));
        QCOMPARE(blocks[1].source, QString("Value"));
    }

    void readsADoubleColumnPageColumnByColumn() {
        // A newspaper layout: the same row holds the end of one column and the start of the
        // next. Read row by row, the two half-sentences travel as neighbours.
        const auto blocks = group_overlay_blocks(result_of({
            line("Right top", QRectF(500, 10, 300, 14)),
            line("Left top", QRectF(10, 10, 300, 14)),
            line("Right bottom", QRectF(500, 60, 300, 14)),
            line("Left bottom", QRectF(10, 60, 300, 14)),
        }));
        QCOMPARE(blocks.size(), 4);
        QCOMPARE(blocks[0].source, QString("Left top"));
        QCOMPARE(blocks[1].source, QString("Left bottom"));
        QCOMPARE(blocks[2].source, QString("Right top"));
        QCOMPARE(blocks[3].source, QString("Right bottom"));
    }

    void aFullWidthHeadingStaysAboveTheColumns() {
        // The heading crosses the gutter, so it is not sorted into either column: it keeps its
        // place at the top and the columns are read after it.
        const auto blocks = group_overlay_blocks(result_of({
            line("Left one", QRectF(10, 60, 300, 14)),
            line("Left two", QRectF(10, 110, 300, 14)),
            line("Right one", QRectF(500, 60, 300, 14)),
            line("Right two", QRectF(500, 110, 300, 14)),
            line("Installation", QRectF(10, 10, 780, 24)),
        }));
        QCOMPARE(blocks.size(), 5);
        QCOMPARE(blocks[0].source, QString("Installation"));
        QCOMPARE(blocks[1].source, QString("Left one"));
        QCOMPARE(blocks[2].source, QString("Left two"));
        QCOMPARE(blocks[3].source, QString("Right one"));
        QCOMPARE(blocks[4].source, QString("Right two"));
    }

    void keepsASingleColumnInPlainOrder() {
        // Slightly different margins on one column are not columns: the page is read down.
        const auto blocks = group_overlay_blocks(result_of({
            line("alpha", QRectF(10, 10, 200, 14)),
            line("beta", QRectF(12, 40, 260, 14)),
            line("gamma", QRectF(14, 70, 240, 14)),
            line("delta", QRectF(16, 100, 280, 14)),
        }));
        QCOMPARE(blocks.size(), 4);
        QCOMPARE(blocks[0].source, QString("alpha"));
        QCOMPARE(blocks[1].source, QString("beta"));
        QCOMPARE(blocks[2].source, QString("gamma"));
        QCOMPARE(blocks[3].source, QString("delta"));
    }

    void readsTopDownThenLeftToRight() {
        // Recognition order is not reading order, so the blocks are sorted before grouping.
        const auto blocks = group_overlay_blocks(result_of({
            line("bottom", QRectF(10, 80, 100, 14)),
            line("right", QRectF(200, 10, 100, 14)),
            line("top", QRectF(10, 10, 100, 14)),
        }));
        QCOMPARE(blocks.size(), 3);
        QCOMPARE(blocks[0].source, QString("top"));
        QCOMPARE(blocks[1].source, QString("right"));
        QCOMPARE(blocks[2].source, QString("bottom"));
    }

    void dropsLinesThatCannotBePlaced() {
        OcrLine blank;
        blank.text = "   ";
        blank.box = {QPointF(10, 10), QPointF(110, 10), QPointF(110, 24), QPointF(10, 24)};
        OcrLine no_box;
        no_box.text = "nowhere";
        // The engine reports a line it has no position for as a single point.
        OcrLine point;
        point.text = "point";
        point.box = {QPointF(5, 5)};
        const auto blocks = group_overlay_blocks(result_of({blank, no_box, point, line("real", QRectF(10, 40, 100, 14))}));
        QCOMPARE(blocks.size(), 1);
        QCOMPARE(blocks[0].source, QString("real"));
        QVERIFY(group_overlay_blocks(OcrResult{}).isEmpty());
    }

    void capsTheParagraphLength() {
        // A column of unrelated lines that happens to line up stays several paragraphs.
        QVector<OcrLine> lines;
        for (int index = 0; index < 40; ++index) lines.append(line(QString("line %1").arg(index), QRectF(10, 10 + index * 16, 200, 14)));
        const auto blocks = group_overlay_blocks(result_of(lines));
        QCOMPARE(blocks.size(), 2);
        QCOMPARE(blocks[0].lines, 30);
        QCOMPARE(blocks[1].lines, 10);
    }

    void paintsTheTranslationOverTheOriginal() {
        const QColor background(255, 255, 255);
        const auto source = page(QSize(320, 120), background, QColor(20, 20, 20), {{"Hello world", QRectF(10, 10, 260, 18)}});
        int painted = 0;
        const auto layer = render_translation_overlay(source, {block_of("Hello world", "Bonjour le monde", QRectF(10, 10, 260, 18), 18)}, "FR", &painted);
        QCOMPARE(painted, 1);
        QCOMPARE(layer.size(), source.size());
        // Nothing outside the block is touched, so the rest of the capture stays as captured.
        QCOMPARE(qAlpha(layer.pixel(2, 118)), 0);
        QCOMPARE(qAlpha(layer.pixel(318, 2)), 0);
        // The block itself is covered in the page's colour, ready for the new text.
        QCOMPARE(qAlpha(layer.pixel(150, 19)), 255);
        QVERIFY(near(QColor(layer.pixel(9, 8)), background));
        // And the text on it is no longer the original.
        QVERIFY(differing_pixels(composited(source, layer), source, QRect(8, 6, 264, 26)) > 20);
    }

    void paintsStraightOntoACopyOfTheSource() {
        const auto source = page(QSize(320, 120), Qt::white, QColor(20, 20, 20), {{"Hello world", QRectF(10, 10, 260, 18)}});
        const auto before = source.copy();
        int painted = 0;
        const auto translated = paint_translations(source, {block_of("Hello world", "Bonjour le monde", QRectF(10, 10, 260, 18), 18)}, "FR", &painted);
        QCOMPARE(painted, 1);
        QCOMPARE(translated.size(), source.size());
        QCOMPARE(source, before);
        // The block is rewritten on the page's colour; everything below it is the source as it was.
        QVERIFY(differing_pixels(translated, source, QRect(8, 6, 264, 26)) > 20);
        QVERIFY(near(QColor(translated.pixel(9, 8)), Qt::white));
        QCOMPARE(translated.copy(QRect(0, 60, 320, 60)), source.copy(QRect(0, 60, 320, 60)));
        // Nothing changed, nothing to show.
        QVERIFY(paint_translations(source, {block_of("Hello world", "Hello world", QRectF(10, 10, 260, 18), 18)}, "FR", &painted).isNull());
        QCOMPARE(painted, 0);
    }

    // A long screenshot arrives as RGB888 and is pinned: the pixmap made of the result must not
    // need a second copy of it.
    void anOpaqueSourceIsPaintedAsRgb32AndAPixmapSharesIt() {
        const auto page_image = page(QSize(320, 120), Qt::white, QColor(20, 20, 20), {{"Hello world", QRectF(10, 10, 260, 18)}});
        const QVector<OverlayTextBlock> blocks{block_of("Hello world", "Bonjour le monde", QRectF(10, 10, 260, 18), 18)};
        const auto translated = paint_translations(page_image.convertToFormat(QImage::Format_RGB888), blocks, "FR");
        QCOMPARE(translated.format(), QImage::Format_RGB32);
        QCOMPARE(translated, paint_translations(page_image, blocks, "FR").convertToFormat(QImage::Format_RGB32));
        QCOMPARE(QPixmap::fromImage(translated).toImage().constBits(), translated.constBits());

        // Transparency stays.
        auto clear = page_image.convertToFormat(QImage::Format_ARGB32);
        clear.setPixelColor(300, 110, Qt::transparent);
        const auto kept = paint_translations(clear, blocks, "FR");
        QCOMPARE(kept.format(), QImage::Format_ARGB32_Premultiplied);
        QCOMPARE(qAlpha(kept.pixel(300, 110)), 0);
    }

    void leavesUnchangedTextAlone() {
        const auto source = page(QSize(200, 60), Qt::white, QColor(20, 20, 20), {{"Name", QRectF(10, 10, 120, 16)}});
        int painted = -1;
        // A translation that came back the same (a name, a number) keeps the original pixels.
        QVERIFY(render_translation_overlay(source, {block_of("Name", "Name", QRectF(10, 10, 120, 16), 16)}, "FR", &painted).isNull());
        QCOMPARE(painted, 0);
        // So does one with nothing in it.
        QVERIFY(render_translation_overlay(source, {block_of("Name", "   ", QRectF(10, 10, 120, 16), 16)}, "FR", &painted).isNull());
        QCOMPARE(painted, 0);
        QVERIFY(render_translation_overlay(source, {}, "FR", &painted).isNull());
        QVERIFY(render_translation_overlay(QImage(), {block_of("Name", "Nom", QRectF(10, 10, 120, 16), 16)}, "FR", &painted).isNull());
    }

    void keepsDarkPagesDark() {
        // The page colour is sampled from around the block, not assumed to be white.
        const QColor background(24, 28, 34);
        const auto source = page(QSize(300, 80), background, QColor(230, 230, 230), {{"// a comment", QRectF(10, 10, 240, 16)}});
        int painted = 0;
        const auto layer = render_translation_overlay(source, {block_of("// a comment", "// un commentaire", QRectF(10, 10, 240, 16), 16)}, "FR", &painted);
        QCOMPARE(painted, 1);
        // The padded cover around the text kept the page's colour.
        QVERIFY(near(QColor(layer.pixel(9, 9)), background));
        QVERIFY(near(QColor(layer.pixel(247, 26)), background));
        // Beyond the cover the capture is left as it was.
        QCOMPARE(qAlpha(layer.pixel(270, 28)), 0);
        // The translation is drawn light on dark, like the text it replaced.
        int light = 0;
        for (int y = 10; y < 26; ++y)
            for (int x = 10; x < 250; ++x)
                if (QColor(layer.pixel(x, y)).lightness() > background.lightness() + 60) ++light;
        QVERIFY2(light > 10, "no light text was drawn on the dark page");
    }

    // A recognized box hugs its text, so the size of the original can be read from it: the
    // translation is set no larger, however short it is and however much room that leaves.
    void neverSetsATranslationLargerThanItsSource() {
        QFont font(page_family());
        font.setPixelSize(28);
        const QString text = "Getting started";
        const auto box = QFontMetricsF(font).tightBoundingRect(text).translated(40, 60);
        OverlayTextBlock block = block_of(text, QString::fromUtf8("入门"), box, box.height());
        block.line_rects = {box};
        QImage source(400, 100, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::white);
        const auto planned = plan_overlay_block(source, {block}, 0, "ZH");
        QVERIFY(planned.has_value());
        QCOMPARE(planned->lines, QStringList({QString::fromUtf8("入门")}));
        // The face differs from the original's, so the size is near it rather than equal.
        QVERIFY2(planned->font.pixelSize() <= 32 && planned->font.pixelSize() >= 20, qPrintable(QString::number(planned->font.pixelSize())));
        QCOMPARE(planned->line_rects.first(), block.rect);
    }

    void shrinksOnOneLineOnlyWhenTheWholeLabelFits() {
        const auto source = page(QSize(320, 200), Qt::white, QColor(20, 20, 20), {{"Note", QRectF(10, 10, 260, 18)}});
        const auto longer = plan_overlay_block(source, {block_of("Note", "This note is longer than the original", QRectF(10, 10, 260, 18), 18)}, 0, "FR");
        QVERIFY(longer.has_value());
        QCOMPARE(longer->lines.size(), 1);
        QVERIFY(longer->font.pixelSize() >= 10);
        // Within its row, which may have taken blank page beside the block but stays on the image.
        QVERIFY(QFontMetricsF(longer->font).horizontalAdvance(longer->text) <= longer->line_rects.first().width() + 0.5);
        QVERIFY(longer->line_rects.first().right() <= 320.0);

        const QString far_too_long = QString("This note is much longer than the single word it replaces. ").repeated(6);
        int painted = 0;
        QVector<int> unplaced;
        const QVector<OverlayTextBlock> blocks{block_of("Note", far_too_long, QRectF(10, 10, 260, 18), 18)};
        QVERIFY(paint_translations(source, blocks, "FR", &painted, &unplaced).isNull());
        QCOMPARE(painted, 0);
        QCOMPARE(unplaced, QVector<int>{0});
        QCOMPARE(format_unplaced_translations(blocks, unplaced), QString("Note\n") + far_too_long);
    }

    void aNeighbouringTranslationCannotEraseTheUnplacedParagraph() {
        auto paragraph = block_of("First row and second row", QString("A long translation with nowhere to go. ").repeated(8), QRectF(14, 32, 160, 31), 15, 2);
        paragraph.line_rects = {QRectF(14, 32, 160, 15), QRectF(14, 48, 140, 15)};
        auto label = block_of("Save", "Save now", QRectF(14, 15, 130, 16), 16);
        const auto source = page(QSize(250, 100), Qt::white, Qt::black, {{label.source, label.rect}, {"First row", paragraph.line_rects[0]}, {"and second row", paragraph.line_rects[1]}});
        int painted = 0;
        QVector<int> unplaced;
        const auto result = paint_translations(source, {label, paragraph}, "EN", &painted, &unplaced);
        QCOMPARE(painted, 1);
        QCOMPARE(unplaced, QVector<int>{1});
        QVERIFY(differing_pixels(result, source, label.rect.toAlignedRect()) > 0);
        QCOMPARE(differing_pixels(result, source, paragraph.rect.toAlignedRect().adjusted(0, 0, 1, 1)), 0);
    }

    // A label of a word or two, a translation that is not: it keeps its size and runs on
    // over the blank page beside it, as far as the next text or anything that is not page.
    void aShortLabelRunsOnIntoTheBlankRoomBesideIt() {
        QFont font(page_family());
        font.setPixelSize(16);
        const auto box = QFontMetricsF(font).tightBoundingRect("Esc").translated(20, 40).toAlignedRect();
        QImage source(400, 80, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::white);
        const QString translation = "Touche d'echappement";
        const auto label = [&](const QRect &rect, const QString &text) {
            OverlayTextBlock block = block_of(text, text == "Esc" ? translation : QString("x"), QRectF(rect), rect.height());
            block.line_rects = {QRectF(rect)};
            return block;
        };
        const QRect far_text(300, box.top(), 60, box.height());
        const auto free = plan_overlay_block(source, {label(box, "Esc"), label(far_text, "Cancel")}, 0, "FR");
        QVERIFY(free.has_value());
        QCOMPARE(free->lines.size(), 1);
        QVERIFY2(free->font.pixelSize() >= 12, qPrintable(QString::number(free->font.pixelSize())));
        QVERIFY(free->line_rects.first().right() > box.right() + 40);
        QVERIFY(free->line_rects.first().right() < far_text.left());
        QVERIFY(free->cover.right() >= static_cast<int>(free->line_rects.first().right()) - 1);

        // Text right beside it leaves no room even at the minimum size.
        const QRect near_text(box.right() + 8, box.top(), 60, box.height());
        bool does_not_fit = false;
        QVERIFY(!plan_overlay_block(source, {label(box, "Esc"), label(near_text, "Cancel")}, 0, "FR", &does_not_fit));
        QVERIFY(does_not_fit);
        // So does a picture: the room ends where the page does.
        {
            QPainter painter(&source);
            painter.fillRect(QRect(box.right() + 10, box.top() - 4, 30, box.height() + 8), QColor(200, 40, 40));
        }
        QVERIFY(!plan_overlay_block(source, {label(box, "Esc"), label(far_text, "Cancel")}, 0, "FR", &does_not_fit));
        QVERIFY(does_not_fit);
    }

    void keepsAParagraphsLinesWhereTheOriginalsStood() {
        OverlayTextBlock block = block_of("first line of it\nsecond line\nthird", "premiere\ndeuxieme", QRectF(20, 20, 300, 78), 18, 3);
        block.line_rects = {QRectF(20, 20, 300, 18), QRectF(20, 50, 220, 18), QRectF(20, 80, 100, 18)};
        QImage source(360, 140, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::white);
        const auto planned = plan_overlay_block(source, {block}, 0, "FR");
        QVERIFY(planned.has_value());
        QCOMPARE(planned->lines, QStringList({"premiere", "deuxieme"}));
        QCOMPARE(planned->line_rects.at(0).center().y(), 29.0);
        QCOMPARE(planned->line_rects.at(1).center().y(), 59.0);
        // Ragged on the right: set from the left.
        QCOMPARE(planned->alignment, Qt::Alignment(Qt::AlignLeft));

        // Lines sharing a centre were centred, lines sharing a right edge were set from the right.
        block.line_rects = {QRectF(20, 20, 300, 18), QRectF(60, 50, 220, 18), QRectF(120, 80, 100, 18)};
        QCOMPARE(plan_overlay_block(source, {block}, 0, "FR")->alignment, Qt::Alignment(Qt::AlignHCenter));
        block.line_rects = {QRectF(20, 20, 300, 18), QRectF(100, 50, 220, 18), QRectF(220, 80, 100, 18)};
        QCOMPARE(plan_overlay_block(source, {block}, 0, "FR")->alignment, Qt::Alignment(Qt::AlignRight));
        // One short first line is an indent.
        block.line_rects = {QRectF(50, 20, 270, 18), QRectF(20, 50, 300, 18), QRectF(20, 80, 300, 18)};
        QCOMPARE(plan_overlay_block(source, {block}, 0, "FR")->alignment, Qt::Alignment(Qt::AlignLeft));
    }

    // The fill behind the text is what the cover takes, not the frame around it, nor a
    // mixture of the two that is on the page nowhere.
    void coversWithTheFillTheTextStandsOn() {
        const QColor fill(226, 238, 255), frame(40, 60, 110);
        QImage source(260, 80, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::white);
        {
            QPainter painter(&source);
            painter.fillRect(QRect(18, 18, 204, 34), frame);
            painter.fillRect(QRect(21, 21, 198, 28), fill);
            QFont font(page_family());
            font.setPixelSize(16);
            painter.setFont(font);
            painter.setPen(QColor(20, 30, 60));
            painter.drawText(QRect(24, 22, 190, 26), Qt::AlignLeft | Qt::AlignVCenter, "Save changes");
        }
        const auto planned = plan_overlay_block(source, {block_of("Save changes", "Enregistrer", QRectF(22, 22, 196, 26), 26)}, 0, "FR");
        QVERIFY(planned.has_value());
        QCOMPARE(planned->background, fill);
        // Text right beside another block's is not sampled as page either.
        const auto beside = plan_overlay_block(
            source, {block_of("Save changes", "Enregistrer", QRectF(22, 22, 96, 26), 26), block_of("x", "y", QRectF(120, 22, 98, 26), 26)}, 0, "FR");
        QCOMPARE(beside->background, fill);
        // Nothing to sample from: a block of one colour takes that colour.
        QImage flat(100, 40, QImage::Format_ARGB32_Premultiplied);
        flat.fill(QColor(10, 120, 60));
        QCOMPARE(plan_overlay_block(flat, {block_of("a", "b", QRectF(0, 0, 100, 40), 40)}, 0, "FR")->background, QColor(10, 120, 60));
    }

    // Bars standing in for the strokes of a heavy headline: they cover six of the nine rows read
    // across the box, so inside it the ink runs longer than the page. The page still surrounds it.
    void aBlockItsGlyphsFillTakesThePageAroundIt() {
        QImage source(200, 80, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::white);
        {
            QPainter painter(&source);
            for (int y = 20; y < 44; y += 6) painter.fillRect(QRect(20, y, 100, 4), Qt::black);
        }
        const auto planned = plan_overlay_block(source, {block_of("Headline", "Titre", QRectF(20, 20, 100, 24), 24)}, 0, "FR");
        QVERIFY(planned.has_value());
        QCOMPARE(planned->background, QColor(Qt::white));
    }

    // Light bars on a page that darkens from left to right by a third of a level a pixel: the
    // page between them is one run however far it drifts, and the box takes the page's colour
    // near its middle rather than the bars'.
    void heavyTextOnAGradientTakesThePageNotTheInk() {
        QImage source(360, 80, QImage::Format_ARGB32_Premultiplied);
        {
            QPainter painter(&source);
            QLinearGradient page(0, 0, 360, 0);
            page.setColorAt(0, QColor(250, 150, 30));
            page.setColorAt(1, QColor(130, 30, 30));
            painter.fillRect(source.rect(), page);
            for (int y = 20; y < 44; y += 6) painter.fillRect(QRect(80, y, 200, 4), Qt::white);
        }
        const auto planned = plan_overlay_block(source, {block_of("Headline", "Titre", QRectF(80, 20, 200, 24), 24)}, 0, "FR");
        QVERIFY(planned.has_value());
        // Halfway along, the page is (190, 90, 30); the box spans 200 px, about 67 levels of red.
        QVERIFY2(near(planned->background, QColor(190, 90, 30), 40), qPrintable(planned->background.name()));
    }

    // A box that cuts off a descender below and an accent above: the cover follows the ink out,
    // and stops at a grey rule the accent touches, which is not the text's.
    void theCoverFollowsTheInkTheBoxCutsOffButNotARule() {
        QImage source(240, 100, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::white);
        {
            QPainter painter(&source);
            painter.fillRect(QRect(30, 35, 181, 1), QColor(128, 128, 128));
            painter.fillRect(QRect(60, 36, 3, 4), Qt::black);
            painter.fillRect(QRect(100, 50, 3, 15), Qt::black);
            painter.fillRect(QRect(44, 44, 40, 10), Qt::black);
        }
        const auto planned = plan_overlay_block(source, {block_of("text", "Texte", QRectF(40, 40, 160, 20), 20)}, 0, "FR");
        QVERIFY(planned.has_value());
        // Two pixels past the box (38..61), then one more row for each row the ink reaches.
        QCOMPARE(planned->cover.bottom(), 64);
        QCOMPARE(planned->cover.top(), 36);
        QVERIFY(!planned->background_region.contains(QPoint(120, 35)));
    }

    // Recognition, the requests and the painting together: a paragraph the service refuses
    // keeps its original pixels, and the others are still translated.
    void theWorkerKeepsWhatTranslatedWhenOneParagraphFails() {
        const QVector<QPair<QString, QRectF>> texts{{"Settings", QRectF(20, 20, 220, 24)}, {"Cancel", QRectF(20, 90, 220, 24)}, {"Apply", QRectF(20, 160, 220, 24)}};
        const auto source = page(QSize(320, 220), Qt::white, Qt::black, texts);
        set_recognizer_for_test([&](const QImage &) {
            QVector<OcrLine> lines;
            for (const auto &text : texts) lines.append(line(text.first, text.second));
            return result_of(lines);
        });
        const auto restore = qScopeGuard([] { set_recognizer_for_test({}); });
        const auto service = scripted_service([](const QString &text) {
            if (text == "Cancel") return TranslationResult::failure(TranslationErrorCode::INVALID_REQUEST, "text refused");
            return translated_as(text == "Settings" ? QString::fromUtf8("设置") : QString::fromUtf8("应用"));
        });
        OverlayTranslationWorker worker(source, {{"target_lang", "zh-Hans"}}, service);
        QSignalSpy finished(&worker, &OverlayTranslationWorker::finished_signal);
        QSignalSpy progress(&worker, &OverlayTranslationWorker::progress_changed);
        worker.start();
        QVERIFY(finished.wait(10000));
        QVERIFY(worker.wait(5000));
        QVERIFY2(finished.first().first().toBool(), qPrintable(worker.error_text()));
        QCOMPARE(worker.blocks().size(), 3);
        QCOMPARE(worker.blocks().at(0).translated, QString::fromUtf8("设置"));
        QCOMPARE(worker.blocks().at(1).translated, QString());
        QCOMPARE(worker.failed_blocks(), 1);
        QVERIFY2(worker.failure_notice().startsWith("1 "), qPrintable(worker.failure_notice()));
        QVERIFY(worker.failure_notice().endsWith("\ntext refused"));
        QCOMPARE(worker.painted_blocks(), 2);
        QCOMPARE(worker.translated_chars(), qint64(8 + 5));
        // The refused paragraph is untouched, the translated ones are repainted.
        const auto painted = worker.result().convertToFormat(source.format());
        QCOMPARE(differing_pixels(source, painted, texts[1].second.toAlignedRect()), 0);
        QVERIFY(differing_pixels(source, painted, texts[0].second.toAlignedRect()) > 0);
        QVERIFY(differing_pixels(source, painted, texts[2].second.toAlignedRect()) > 0);
        // The progress counts every paragraph, the failed one included.
        QCOMPARE(progress.last().at(0).toInt(), 3);
        QCOMPARE(progress.last().at(1).toInt(), 3);
    }

    void theWorkerFailsWhenNothingWasTranslated() {
        const QVector<QPair<QString, QRectF>> texts{{"Settings", QRectF(20, 20, 220, 24)}, {"Cancel", QRectF(20, 90, 220, 24)}};
        const auto source = page(QSize(320, 160), Qt::white, Qt::black, texts);
        set_recognizer_for_test([&](const QImage &) { return result_of({line("Settings", texts[0].second), line("Cancel", texts[1].second)}); });
        const auto restore = qScopeGuard([] { set_recognizer_for_test({}); });
        const auto run = [&](const TranslationScript &script) {
            OverlayTranslationWorker worker(source, {{"target_lang", "zh-Hans"}}, scripted_service(script));
            QSignalSpy finished(&worker, &OverlayTranslationWorker::finished_signal);
            worker.start();
            const bool reported = finished.wait(10000) && worker.wait(5000) && !finished.first().first().toBool() && worker.result().isNull();
            return reported ? worker.error_text() : QString("no failure reported");
        };
        QCOMPARE(run([](const QString &) { return TranslationResult::failure(TranslationErrorCode::AUTH_FAILED, "invalid key"); }), QString("invalid key"));
        QCOMPARE(run([](const QString &) { return TranslationResult::failure(TranslationErrorCode::INVALID_REQUEST, "text refused"); }), QString("text refused"));
        QCOMPARE(run([](const QString &) { return translated_as(QString()); }), QString("The translation service returned no text."));
    }

    // A translation that does not fit leaves its original and comes back as text, unless it is
    // only a short label among translations that were drawn.
    void theWorkerReturnsTheImageAndTheTextThatWouldNotFit() {
        const QRectF wide(12, 58, 150, 16);
        const QString full = QStringLiteral("Réinitialiser toute la configuration réseau et les profils enregistrés sur cet appareil, puis redémarrer");
        const auto run = [&](const QString &label, const QString &unplaced) {
            QFont font(page_family());
            font.setPixelSize(13);
            const QRectF narrow = QFontMetricsF(font).tightBoundingRect(label).translated(12, 24);
            const auto source = page(QSize(260, 100), Qt::white, Qt::black, {{label, narrow}, {"Cancel", wide}});
            const auto service = scripted_service([&](const QString &text) { return translated_as(text == label ? full : QString("Annuler")); });
            OverlayTranslationWorker worker(source, {{"target_lang", "FR"}}, service);
            auto block = block_of(label, {}, narrow, narrow.height());
            block.line_rects = {narrow};
            worker.set_blocks({block, block_of("Cancel", {}, wide, 16)});
            QSignalSpy finished(&worker, &OverlayTranslationWorker::finished_signal);
            worker.start();
            QVERIFY(finished.wait(10000));
            QVERIFY(worker.wait(5000));
            QVERIFY(worker.succeeded());
            QCOMPARE(worker.painted_blocks(), 1);
            QCOMPARE(worker.failed_blocks(), 0);
            QCOMPARE(worker.unplaced_translations(), unplaced);
            QCOMPARE(differing_pixels(source, worker.result(), narrow.toAlignedRect()), 0);
            QVERIFY(differing_pixels(source, worker.result(), wide.toAlignedRect()) > 0);
        };
        run("Reset all saved network profiles", QString("Reset all saved network profiles\n") + full);
        run("Reset", QString());
    }

    // Small print is translated at its own size rather than not at all.
    void smallPrintIsTranslatedAtItsOwnSize() {
        QFont font(page_family());
        font.setPixelSize(9);
        const QString text = "Terms apply";
        const auto box = QFontMetricsF(font).tightBoundingRect(text).translated(40, 40);
        OverlayTextBlock block = block_of(text, QString::fromUtf8("适用条款"), box, box.height());
        block.line_rects = {box};
        QImage source(300, 80, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::white);
        const auto planned = plan_overlay_block(source, {block}, 0, "ZH");
        QVERIFY(planned.has_value());
        QVERIFY2(planned->font.pixelSize() <= 10, qPrintable(QString::number(planned->font.pixelSize())));
    }

    // Hemmed in on the right, a label's translation takes the blank page on its left and ends
    // where the label ends; with room on both sides it is centred over the label.
    void aHemmedInLabelUsesTheBlankPageOnItsLeft() {
        QFont font(page_family());
        font.setPixelSize(16);
        const auto box = QFontMetricsF(font).tightBoundingRect("Esc").translated(220, 40).toAlignedRect();
        QImage source(400, 80, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::white);
        const auto label = [&](const QRect &rect, const QString &text) {
            OverlayTextBlock block = block_of(text, text == "Esc" ? QString("Touche d'echappement") : QString("x"), QRectF(rect), rect.height());
            block.line_rects = {QRectF(rect)};
            return block;
        };
        const QRect beside(box.right() + 3, box.top(), 60, box.height());
        const auto hemmed = plan_overlay_block(source, {label(box, "Esc"), label(beside, "Cancel")}, 0, "FR");
        QVERIFY(hemmed.has_value());
        QCOMPARE(hemmed->alignment, Qt::Alignment(Qt::AlignRight));
        QVERIFY(hemmed->line_rects.first().left() < box.left() - 40);
        QVERIFY(hemmed->line_rects.first().right() <= box.right() + 1);
        QVERIFY(hemmed->cover.left() <= static_cast<int>(hemmed->line_rects.first().left()) + 1);
        // A little room on the right, the rest on the left: centred.
        const QRect near(box.right() + 30, box.top(), 60, box.height());
        const auto centred = plan_overlay_block(source, {label(box, "Esc"), label(near, "Cancel")}, 0, "FR");
        QVERIFY(centred.has_value());
        QCOMPARE(centred->alignment, Qt::Alignment(Qt::AlignHCenter));
        QVERIFY(centred->line_rects.first().right() > box.right());
        QVERIFY(centred->line_rects.first().left() < box.left());
    }

    // A paragraph that wraps around a box inside its own area keeps its translation clear of
    // that box instead of giving up: the rows beside it are only as wide as the room left.
    void aParagraphWrapsItsTranslationAroundABoxInItsArea() {
        OverlayTextBlock paragraph = block_of("one\ntwo\nthree\nfour", QString("words of a translation ").repeated(8) + QString("and a little more"), QRectF(20, 20, 300, 76), 16, 4);
        paragraph.line_texts = {"The first line of the paragraph runs wide", "and so does the second line of it here", "the third is short", "and the fourth"};
        paragraph.line_rects = {QRectF(20, 20, 300, 16), QRectF(20, 40, 300, 16), QRectF(20, 60, 160, 16), QRectF(20, 80, 160, 16)};
        const QRectF caption(200, 58, 120, 40);
        QImage source(360, 120, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::white);
        const auto planned = plan_overlay_block(source, {paragraph, block_of("Caption", "Legende", caption, 16)}, 0, "EN");
        QVERIFY(planned.has_value());
        QVERIFY(planned->lines.size() > 2);
        const QFontMetricsF metrics(planned->font);
        for (qsizetype index = 0; index < planned->lines.size(); ++index) {
            const auto ink = metrics.tightBoundingRect(planned->lines[index]).translated(planned->line_origins[index]);
            QVERIFY2(!ink.intersects(caption), qPrintable(planned->lines[index]));
        }
    }

    // A translation too long for its paragraph's own rows takes rows of blank page below it, and
    // above it when the next text is right below.
    void aParagraphTakesBlankRowsBelowItAndThenAboveIt() {
        auto paragraph = block_of("x", "This translation of the paragraph is a good deal longer than the original text was, and it goes on for a while after that",
                                  QRectF(20, 100, 300, 38), 16, 2);
        paragraph.line_texts = {"The first line of a paragraph that runs", "and its second line"};
        paragraph.line_rects = {QRectF(20, 100, 300, 16), QRectF(20, 122, 200, 16)};
        const auto under = line_block("Below", "Dessous", QRectF(20, 146, 300, 16));
        const auto over = line_block("Above", "Dessus", QRectF(20, 76, 300, 16));
        QImage source(360, 260, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::white);

        const auto boxed = plan_overlay_block(source, {paragraph, under, over}, 0, "EN");
        const auto free = plan_overlay_block(source, {paragraph}, 0, "EN");
        const auto open_above = plan_overlay_block(source, {paragraph, under}, 0, "EN");
        QVERIFY(boxed && free && open_above);
        QVERIFY2(free->font.pixelSize() > boxed->font.pixelSize(), qPrintable(QString("%1 %2").arg(free->font.pixelSize()).arg(boxed->font.pixelSize())));
        QVERIFY(free->lines.size() > 2);
        QVERIFY(free->line_rects.last().bottom() > 138);
        QVERIFY(free->cover.bottom() >= free->line_rects.last().bottom());
        // Rows below were enough: nothing was taken above.
        QCOMPARE(free->area.top(), 100.0);

        QVERIFY2(open_above->font.pixelSize() > boxed->font.pixelSize(), qPrintable(QString("%1 %2").arg(open_above->font.pixelSize()).arg(boxed->font.pixelSize())));
        QVERIFY(open_above->line_rects.first().top() < 100);
        for (const auto &ink : overlay_text_ink(*open_above)) QVERIFY(!ink.intersects(under.rect));
    }

    // A light grey button on a white page is close enough in colour to pass for the page, but the
    // step at its edge ends the room a label's translation runs on into.
    void theRoomBesideALabelEndsAtTheEdgeOfAButton() {
        const QRect box(160, 40, 30, 16);
        const QRect button(box.right() + 24, box.top() - 6, 120, box.height() + 12);
        QImage source(400, 80, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::white);
        {
            QPainter painter(&source);
            painter.fillRect(button, QColor(238, 238, 238));
        }
        const auto planned = plan_overlay_block(source, {line_block("Esc", "Touche d'echappement", QRectF(box))}, 0, "FR");
        QVERIFY(planned.has_value());
        QVERIFY2(planned->cover.right() < button.left(), qPrintable(QString::number(planned->cover.right())));
        QVERIFY(planned->line_rects.first().right() < button.left());
        QVERIFY(planned->line_rects.first().left() < box.left());
    }

    // A sentence on one line goes on onto a blank row below for a clearly larger size; a label of
    // a word stays on one line, and so does a sentence with other text beside the row below.
    void aLongLineRunsOnBelowButALabelStaysOnOne() {
        const QRectF box(70, 20, 250, 16);
        const QString translation = "Reinitialiser tous les profils reseau enregistres maintenant";
        const auto sentence = line_block("Reset all saved network profiles now", translation, box);
        const auto left = line_block("A", "A", QRectF(20, 20, 30, 16));
        const auto right = line_block("B", "B", QRectF(340, 20, 40, 16));
        QImage source(400, 120, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::white);

        const auto wrapped = plan_overlay_block(source, {sentence, left, right}, 0, "FR");
        const auto kept = plan_overlay_block(source, {sentence, left, right, line_block("C", "C", QRectF(70, 44, 250, 16))}, 0, "FR");
        QVERIFY(wrapped && kept);
        QCOMPARE(wrapped->lines.size(), 2);
        QCOMPARE(kept->lines.size(), 1);
        QVERIFY2(wrapped->font.pixelSize() * 5 >= kept->font.pixelSize() * 6, qPrintable(QString("%1 %2").arg(wrapped->font.pixelSize()).arg(kept->font.pixelSize())));
        QCOMPARE(wrapped->alignment, Qt::Alignment(Qt::AlignLeft));
        QVERIFY(wrapped->line_rects.last().top() > box.bottom());
        // Broken between words only.
        QVERIFY(translation.split(' ').contains(wrapped->lines.last().split(' ').first()));

        const auto beside = plan_overlay_block(source, {sentence, left, right, line_block("D", "D", QRectF(330, 44, 40, 16))}, 0, "FR");
        QVERIFY(beside.has_value());
        QCOMPARE(beside->lines.size(), 1);

        const auto label = plan_overlay_block(source, {line_block("Reset", translation, box), left, right}, 0, "FR");
        QVERIFY(label.has_value());
        QCOMPARE(label->lines.size(), 1);
    }

    // The recognizer put the start of a paragraph's last line in the paragraph and its end, a link,
    // apart: the paragraph's box covers the link, its lines do not.
    void aLineSplitOffItsParagraphIsStillTranslated() {
        auto paragraph = block_of("x", "Le premier paragraphe et sa suite", QRectF(20, 20, 300, 38), 16, 2);
        paragraph.line_texts = {"The first line of a paragraph that runs", "and its second line"};
        paragraph.line_rects = {QRectF(20, 20, 300, 16), QRectF(20, 42, 170, 16)};
        const auto link = line_block("the link", "le lien", QRectF(200, 42, 110, 16));
        QImage source(360, 100, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::white);
        const auto planned = plan_overlay_block(source, {paragraph, link}, 1, "FR");
        QVERIFY(planned.has_value());
        for (const auto &ink : overlay_text_ink(*planned))
            for (const auto &line : paragraph.line_rects) QVERIFY(!ink.intersects(line));
    }

    // Two labels with blank page between them both run on into it; the second keeps clear of the
    // first's translation, as painting them all does.
    void aTranslationKeepsClearOfThoseBeforeIt() {
        const QVector<OverlayTextBlock> blocks{line_block("Esc", "Touche d'echappement", QRectF(20, 30, 30, 16)),
                                               line_block("Tab", "Touche de tabulation", QRectF(200, 30, 30, 16)),
                                               line_block("OK", "OK", QRectF(300, 30, 30, 16))};
        QImage source(400, 80, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::white);
        const auto first = plan_overlay_block(source, blocks, 0, "FR");
        const auto alone = plan_overlay_block(source, blocks, 1, "FR");
        const auto second = plan_overlay_block(source, blocks, 1, "FR", nullptr, overlay_text_ink(*first));
        QVERIFY(first && alone && second);
        QVERIFY(inks_meet(*first, *alone));
        QVERIFY(!inks_meet(*first, *second));
        QVERIFY(second->font.pixelSize() < alone->font.pixelSize());

        int painted = 0;
        const auto whole = paint_translations(source, blocks, "FR", &painted);
        QCOMPARE(painted, 2);
        QImage expected = source.convertToFormat(whole.format());
        paint_overlay_blocks(expected, {*first, *second});
        QCOMPARE(whole, expected);
    }

    // The translation is a layer on the background, not a change to its pixels.
    void theBackgroundCarriesALayerItCanTakeOff() {
        const auto source = page(QSize(320, 120), Qt::white, QColor(20, 20, 20), {{"Hello world", QRectF(10, 10, 260, 18)}});
        CanvasScene scene(source, QRectF(0, 0, 320, 120));
        QImage layer(100, 40, QImage::Format_ARGB32_Premultiplied);
        layer.fill(QColor(9, 190, 120));
        scene.background->set_overlay(layer, QPoint(140, 30));
        QVERIFY(scene.background->overlay_visible());
        const auto shown = rendered(scene);
        QCOMPARE(shown.pixelColor(150, 40), QColor(9, 190, 120));
        QCOMPARE(shown.pixelColor(239, 69), QColor(9, 190, 120));
        QCOMPARE(shown.pixelColor(130, 40), source.pixelColor(130, 40));
        // Whatever samples the picture still reads the picture.
        QCOMPARE(scene.background->image().convertToFormat(source.format()), source);
        scene.background->set_overlay_visible(false);
        QVERIFY(!scene.background->overlay_visible());
        QCOMPARE(differing_pixels(rendered(scene), source.convertToFormat(QImage::Format_RGB32), QRect(0, 0, 320, 120)), 0);
    }

    // One capture, start to end: the layer over the selection, the original on demand, only
    // the failed paragraph asked for again, and another language without another recognition.
    void aSessionLaysTheTranslationOverTheSelection() {
        const QRectF selection(40, 30, 300, 200);
        const QRect settings_box(60, 50, 220, 24), cancel_box(60, 120, 220, 24);
        const auto source = page(QSize(400, 260), Qt::white, Qt::black, {{"Settings", QRectF(settings_box)}, {"Cancel", QRectF(cancel_box)}});
        const auto plain = source.convertToFormat(QImage::Format_RGB32);
        CanvasScene scene(source, QRectF(0, 0, 400, 260));
        QTemporaryDir dir;
        QSettings settings(dir.filePath("settings.ini"), QSettings::IniFormat);
        ToolSettingsManager config(settings);

        std::atomic_int recognitions = 0;
        // Boxes are in the selection's own pixels.
        set_recognizer_for_test([&](const QImage &image) {
            ++recognitions;
            if (image.size() != QSize(300, 200)) return result_of({line("Whole page", QRectF(60, 50, 220, 24))});
            return result_of({line("Settings", QRectF(20, 20, 220, 24)), line("Cancel", QRectF(20, 90, 220, 24))});
        });
        const auto restore = qScopeGuard([] { set_recognizer_for_test({}); });
        std::mutex mutex;
        QStringList asked;
        std::atomic_bool refuse_cancel = true;
        OverlayTranslationSession session(&scene, &config);
        session.make_service = [&] {
            return scripted_service([&](const QString &text) {
                const std::lock_guard lock(mutex);
                asked << text;
                if (text == "Cancel" && refuse_cancel) return TranslationResult::failure(TranslationErrorCode::INVALID_REQUEST, "text refused");
                return translated_as(QString::fromUtf8("译") + text);
            });
        };
        QCOMPARE(session.providers(), (QList<QPair<QString, QString>>{{"scripted", "Scripted"}}));
        QSignalSpy partly(&session, &OverlayTranslationSession::partly_failed);
        QSignalSpy shown(&session, &OverlayTranslationSession::shown_changed);
        QSignalSpy failed(&session, &OverlayTranslationSession::failed);

        QVERIFY(session.show(selection));
        QVERIFY(session.shown());
        QVERIFY(session.busy());
        QTRY_VERIFY(!session.busy());
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
        QCOMPARE(shown.count(), 1);
        QCOMPARE(recognitions.load(), 1);
        QCOMPARE(session.region(), QRect(40, 30, 300, 200));
        QCOMPARE(partly.count(), 1);
        QVERIFY(partly.first().first().toString().endsWith("\ntext refused"));
        // The translated paragraph is repainted where it stands; the refused one, and the
        // picture underneath, are as they were.
        QVERIFY(differing_pixels(rendered(scene), plain, settings_box) > 0);
        QCOMPARE(differing_pixels(rendered(scene), plain, cancel_box), 0);
        QCOMPARE(scene.background->image().convertToFormat(source.format()), source);

        session.set_original_shown(true);
        QCOMPARE(differing_pixels(rendered(scene), plain, plain.rect()), 0);
        session.set_original_shown(false);
        QVERIFY(differing_pixels(rendered(scene), plain, settings_box) > 0);

        // Taken off and put back: only the paragraph that failed is asked for.
        session.hide();
        QVERIFY(!session.shown());
        QCOMPARE(differing_pixels(rendered(scene), plain, plain.rect()), 0);
        refuse_cancel = false;
        asked.clear();
        QVERIFY(session.show(selection));
        QTRY_VERIFY(!session.busy());
        QCOMPARE(asked, QStringList({"Cancel"}));
        QCOMPARE(recognitions.load(), 1);
        QCOMPARE(partly.count(), 1);
        QVERIFY(differing_pixels(rendered(scene), plain, cancel_box) > 0);
        // Complete now: showing it again asks for nothing.
        asked.clear();
        session.hide();
        QVERIFY(session.show(selection));
        QVERIFY(!session.busy());
        QVERIFY(differing_pixels(rendered(scene), plain, settings_box) > 0);

        // Another language is saved and translates everything again, without recognizing again.
        // Whatever the default target is here, this one is another.
        const QString other_language = config.get_translation_target_lang() == "JA" ? "KO" : "JA";
        session.set_target_language(other_language);
        QVERIFY(session.busy());
        QCOMPARE(differing_pixels(rendered(scene), plain, plain.rect()), 0);
        QTRY_VERIFY(!session.busy());
        asked.sort();
        QCOMPARE(asked, QStringList({"Cancel", "Settings"}));
        QCOMPARE(config.get_translation_target_lang(), other_language);
        QCOMPARE(recognitions.load(), 1);
        session.set_target_language(other_language);
        QVERIFY(!session.busy());
        // So does another service.
        asked.clear();
        session.set_provider("other");
        QTRY_VERIFY(!session.busy());
        QCOMPARE(asked.size(), 2);
        QCOMPARE(config.get_translation_provider(), QString("other"));

        // Another selection starts over.
        QVERIFY(session.show(QRectF(0, 0, 400, 260)));
        QTRY_VERIFY(!session.busy());
        QCOMPARE(recognitions.load(), 2);
        QCOMPARE(session.blocks().size(), 1);
        QVERIFY(!session.show(QRectF(900, 900, 50, 50)));
    }

    // While a run goes on, the wait covers the whole selection and the progress counts the
    // paragraphs in; nothing while the original is shown, nor once it ends.
    void aSessionTellsWhatItWorksOnAndHowFar() {
        const auto source = page(QSize(400, 260), Qt::white, Qt::black, {{"Settings", QRectF(60, 50, 220, 24)}, {"Cancel", QRectF(60, 120, 220, 24)}});
        CanvasScene scene(source, QRectF(0, 0, 400, 260));
        std::atomic_bool read = false, answer_cancel = false;
        // Boxes are in the selection's own pixels.
        set_recognizer_for_test([&](const QImage &) {
            while (!read) QThread::msleep(5);
            return result_of({line("Settings", QRectF(20, 20, 220, 24)), line("Cancel", QRectF(20, 90, 220, 24))});
        });
        const auto restore = qScopeGuard([&] {
            read = true;
            answer_cancel = true;
            set_recognizer_for_test({});
        });
        const auto service = [&] {
            return scripted_service([&](const QString &text) {
                while (text == "Cancel" && !answer_cancel) QThread::msleep(5);
                return translated_as(QString::fromUtf8("译") + text);
            });
        };
        OverlayTranslationSession session(&scene, nullptr);
        session.make_service = service;
        QVector<OverlayTranslationWait> seen;
        connect(&session, &OverlayTranslationSession::waiting_changed, this, [&] {
            if (seen.isEmpty() || !(seen.last() == session.waiting())) seen.append(session.waiting());
        });

        QVERIFY(session.show(QRectF(40, 30, 300, 200)));
        const OverlayTranslationWait whole{QRectF(40, 30, 300, 200), false};
        QVERIFY(session.waiting() == whole);
        // Before the selection is read, how much there is to do is not known.
        QCOMPARE(session.progress(), std::make_pair(0, 0));
        session.set_original_shown(true);
        QVERIFY(session.waiting().empty());
        session.set_original_shown(false);
        QVERIFY(session.waiting() == whole);

        read = true;
        QTRY_COMPARE(session.progress(), std::make_pair(1, 2));
        QVERIFY(session.waiting() == whole);
        answer_cancel = true;
        QTRY_VERIFY(!session.busy());
        QVERIFY(session.waiting().empty());
        QCOMPARE(session.progress(), std::make_pair(0, 0));
        QVERIFY(seen.size() >= 2);
        QVERIFY(seen.last().empty());

        // Over a dark page the wait says so, to be drawn light.
        const auto night = page(QSize(200, 100), QColor(20, 22, 26), QColor(230, 230, 230), {{"Night", QRectF(20, 20, 120, 20)}});
        CanvasScene dark_scene(night, QRectF(0, 0, 200, 100));
        OverlayTranslationSession dark_session(&dark_scene, nullptr);
        dark_session.make_service = service;
        read = false;
        QVERIFY(dark_session.show(QRectF(0, 0, 200, 100)));
        QVERIFY(dark_session.waiting().dark);
        read = true;
        QTRY_VERIFY(!dark_session.busy());
    }

    // The wait is swept over the selection on the view after a short delay, and an export of the
    // selection never includes it.
    void theWaitIsSweptOverTheSelectionAndNeverExported() {
        const auto source = page(QSize(320, 200), Qt::white, Qt::black, {{"Settings", QRectF(40, 40, 200, 20)}});
        CanvasScene scene(source, QRectF(0, 0, 320, 200));
        QGraphicsView view(&scene);
        view.setFrameShape(QFrame::NoFrame);
        view.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        view.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        view.resize(320, 200);
        view.show();
        const QImage plain = view.grab().toImage();
        OverlayTranslationIndicator indicator(&view);

        // A wait that ends within the delay shows nothing.
        const OverlayTranslationWait wait{QRectF(40, 40, 200, 120), false};
        indicator.set_waiting(wait);
        indicator.set_waiting({});
        QTest::qWait(TranslationSweep::SHOW_DELAY_MS + 100);
        QVERIFY(!indicator.active());

        indicator.set_waiting(wait);
        QVERIFY(!indicator.active());
        QCOMPARE(view.grab().toImage(), plain);
        QTRY_VERIFY(indicator.active());
        // Halfway across, the band is inside the selection and nowhere else.
        indicator.hold_at_for_test(TranslationSweep::CROSS_MS / 2);
        const QImage shown = view.grab().toImage();
        QVERIFY(differing_pixels(shown, plain, QRect(40, 40, 200, 120)) > 0);
        QCOMPARE(differing_pixels(shown, plain, QRect(0, 0, 320, 40)), 0);
        QCOMPARE(differing_pixels(shown, plain, QRect(0, 160, 320, 40)), 0);
        QCOMPARE(differing_pixels(shown, plain, QRect(0, 40, 40, 120)), 0);
        QCOMPARE(differing_pixels(shown, plain, QRect(240, 40, 80, 120)), 0);
        const QImage exported = ExportService(&scene).export_image(QRectF(0, 0, 320, 200)).convertToFormat(QImage::Format_RGB32);
        QCOMPARE(differing_pixels(exported, source.convertToFormat(QImage::Format_RGB32), QRect(0, 0, 320, 200)), 0);
        // Between two sweeps nothing is drawn.
        indicator.hold_at_for_test(TranslationSweep::CROSS_MS + 100);
        QCOMPARE(view.grab().toImage(), plain);

        indicator.set_waiting({});
        QVERIFY(!indicator.active());
        QCOMPARE(view.grab().toImage(), plain);
    }

    // The band stands out clearly on a light page, in the theme colour, and on a dark one, lighter.
    void theSweepShowsOnLightAndDarkPictures() {
        for (const bool dark : {false, true}) {
            const QColor page = dark ? QColor(20, 22, 26) : QColor(Qt::white);
            QImage image(400, 200, QImage::Format_ARGB32_Premultiplied);
            image.fill(page);
            QPainter painter(&image);
            TranslationSweep::paint_band(painter, image.rect(), dark, TranslationSweep::CROSS_MS / 2);
            painter.end();
            const QRect band = TranslationSweep::band_bounds(image.rect(), TranslationSweep::CROSS_MS / 2);
            QVERIFY(!band.isEmpty() && band.width() < image.width());
            QRgb strongest = page.rgb();
            int contrast = 0;
            for (int x = band.left(); x <= band.right(); ++x) {
                const QRgb pixel = image.pixel(x, 100);
                const int distance = std::max({std::abs(qRed(pixel) - page.red()), std::abs(qGreen(pixel) - page.green()), std::abs(qBlue(pixel) - page.blue())});
                if (distance > contrast) {
                    contrast = distance;
                    strongest = pixel;
                }
            }
            QVERIFY2(contrast >= 40, qPrintable(QString("%1: %2").arg(dark).arg(contrast)));
            if (dark) QVERIFY(qGray(strongest) > qGray(page.rgb()) + 30);
            // Outside the band the picture is untouched.
            QCOMPARE(image.pixel(band.right() + 2 < image.width() ? band.right() + 2 : band.left() - 2, 0), page.rgb());
        }
    }

    // On a long screenshot the sweep is a window of its own over the region, which clicks go
    // through and which never takes the focus.
    void theLongScreenshotSweepIsAClickThroughWindowOverTheRegion() {
        const QRect area(100, 80, 360, 240);
        TranslationSweepWindow sweep(area, false);
        QVERIFY(sweep.isWindow());
        QCOMPARE(sweep.geometry(), area);
        QVERIFY(sweep.windowFlags() & Qt::WindowTransparentForInput);
        QVERIFY(sweep.windowFlags() & Qt::WindowDoesNotAcceptFocus);
        QVERIFY(sweep.windowFlags() & Qt::WindowStaysOnTopHint);
        QVERIFY(sweep.testAttribute(Qt::WA_TransparentForMouseEvents));
        QVERIFY(sweep.testAttribute(Qt::WA_ShowWithoutActivating));
        sweep.begin();
        QVERIFY(sweep.isHidden());
        QTRY_VERIFY(sweep.isVisible() && sweep.active());
        sweep.hold_at_for_test(TranslationSweep::CROSS_MS / 2);
        QImage drawn(area.size(), QImage::Format_ARGB32_Premultiplied);
        drawn.fill(Qt::transparent);
        sweep.render(&drawn);
        // The band alone is drawn; the rest of the window stays see-through.
        int painted = 0;
        for (int y = 0; y < drawn.height(); ++y)
            for (int x = 0; x < drawn.width(); ++x) painted += qAlpha(drawn.pixel(x, y)) > 0;
        QVERIFY(painted > 0 && painted < area.width() * area.height() / 2);
        QCOMPARE(qAlpha(drawn.pixel(0, 0)), 0);
        QCOMPARE(qAlpha(drawn.pixel(area.width() - 1, area.height() - 1)), 0);
        sweep.end();
        QVERIFY(sweep.isHidden());
        QVERIFY(!sweep.active());
    }

    // Twenty selections in quick succession: never two recognitions at once, the runs asked for
    // in between are skipped, and the last selection is the one translated.
    void quickSelectionChangesRunOneAtATimeAndTheNewestWins() {
        const auto source = page(QSize(400, 260), Qt::white, Qt::black, {{"Settings", QRectF(60, 50, 220, 24)}});
        CanvasScene scene(source, QRectF(0, 0, 400, 260));
        std::atomic_int running = 0, most = 0, recognitions = 0, asked = 0;
        // The recognizer cannot be interrupted, like the real engines; it names the width it saw.
        set_recognizer_for_test([&](const QImage &image) {
            const int now = ++running;
            for (int seen = most.load(); now > seen && !most.compare_exchange_weak(seen, now);) {
            }
            ++recognitions;
            QThread::msleep(60);
            --running;
            return result_of({line(QString("Width %1").arg(image.width()), QRectF(10, 10, 120, 20))});
        });
        const auto restore = qScopeGuard([] { set_recognizer_for_test({}); });
        OverlayTranslationSession session(&scene, nullptr);
        session.make_service = [&] {
            return scripted_service([&](const QString &text) {
                ++asked;
                return translated_as(QString::fromUtf8("译") + text);
            });
        };
        QSignalSpy busy(&session, &OverlayTranslationSession::busy_changed);
        for (int step = 0; step < 20; ++step) {
            QVERIFY(session.show(QRectF(10, 10, 200 + step * 5, 150)));
            QTest::qWait(8);
        }
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 10000);
        QCOMPARE(most.load(), 1);
        // A run starts only when the one before has ended, with the newest selection, so the
        // recognitions follow how long they take rather than how many selections were asked for.
        QVERIFY2(recognitions.load() <= 10, qPrintable(QString::number(recognitions.load())));
        QCOMPARE(session.region(), QRect(10, 10, 295, 150));
        QCOMPARE(session.blocks().size(), 1);
        QCOMPARE(session.blocks().first().source, QString("Width 295"));
        QCOMPARE(session.blocks().first().translated, QString::fromUtf8("译Width 295"));
        // Only runs that got past recognition asked for anything.
        QVERIFY(asked.load() <= recognitions.load());
        // Busy once from the first selection to the end, not on and off at every change.
        QCOMPARE(busy.count(), 2);
        QCOMPARE(busy.first().first().toBool(), true);
        QCOMPARE(busy.last().first().toBool(), false);
    }

    // A paragraph met again, as when the selection moves a little, takes the translation it got
    // before: only what is new is asked for. Another language asks for everything again.
    void aParagraphMetAgainIsNotAskedForTwice() {
        const auto source = page(QSize(400, 260), Qt::white, Qt::black, {{"Settings", QRectF(60, 50, 220, 24)}, {"Cancel", QRectF(60, 120, 220, 24)}, {"Apply", QRectF(60, 190, 220, 24)}});
        CanvasScene scene(source, QRectF(0, 0, 400, 260));
        QTemporaryDir dir;
        QSettings settings(dir.filePath("settings.ini"), QSettings::IniFormat);
        ToolSettingsManager config(settings);
        // The upper selection holds Settings and Cancel, the lower one Cancel and Apply.
        set_recognizer_for_test([](const QImage &image) {
            if (image.height() == 120) return result_of({line("Settings", QRectF(20, 20, 220, 24)), line("Cancel", QRectF(20, 90, 220, 24))});
            return result_of({line("Cancel", QRectF(20, 20, 220, 24)), line("Apply", QRectF(20, 90, 220, 24))});
        });
        const auto restore = qScopeGuard([] { set_recognizer_for_test({}); });
        std::mutex mutex;
        QStringList asked;
        OverlayTranslationSession session(&scene, &config);
        session.make_service = [&] {
            return scripted_service([&](const QString &text) {
                const std::lock_guard lock(mutex);
                asked << text;
                return translated_as(QString::fromUtf8("译") + text);
            });
        };
        QVERIFY(session.show(QRectF(40, 30, 300, 120)));
        QTRY_VERIFY(!session.busy());
        asked.sort();
        QCOMPARE(asked, QStringList({"Cancel", "Settings"}));

        asked.clear();
        QVERIFY(session.show(QRectF(40, 100, 300, 130)));
        QTRY_VERIFY(!session.busy());
        QCOMPARE(asked, QStringList({"Apply"}));
        QCOMPARE(session.blocks().size(), 2);
        QCOMPARE(session.blocks()[0].translated, QString::fromUtf8("译Cancel"));
        QCOMPARE(session.blocks()[1].translated, QString::fromUtf8("译Apply"));

        asked.clear();
        session.set_target_language(config.get_translation_target_lang() == "JA" ? "KO" : "JA");
        QTRY_VERIFY(!session.busy());
        asked.sort();
        QCOMPARE(asked, QStringList({"Apply", "Cancel"}));
    }

    // Another language chosen after a run's thread ended, before the session heard it end,
    // starts at once instead of waiting for an end that was already told.
    void anotherLanguageChosenAsARunEndsStillTranslates() {
        const auto source = page(QSize(400, 260), Qt::white, Qt::black, {{"Settings", QRectF(60, 50, 220, 24)}});
        CanvasScene scene(source, QRectF(0, 0, 400, 260));
        QTemporaryDir dir;
        QSettings settings(dir.filePath("settings.ini"), QSettings::IniFormat);
        ToolSettingsManager config(settings);
        std::atomic<QThread *> worker = nullptr;
        set_recognizer_for_test([&worker](const QImage &) {
            worker = QThread::currentThread();
            return result_of({line("Settings", QRectF(20, 20, 220, 24))});
        });
        const auto restore = qScopeGuard([] { set_recognizer_for_test({}); });
        std::mutex mutex;
        QStringList asked;
        OverlayTranslationSession session(&scene, &config);
        session.make_service = [&] {
            return scripted_service([&](const QString &text) {
                const std::lock_guard lock(mutex);
                asked << text;
                return translated_as(QString::fromUtf8("译") + text);
            });
        };
        QVERIFY(session.show(QRectF(40, 30, 300, 200)));
        // No events are processed here, so what the thread posts as it ends waits in the queue.
        QDeadlineTimer deadline(10000);
        while (!worker && !deadline.hasExpired()) QThread::msleep(1);
        QVERIFY(worker.load());
        QVERIFY(worker.load()->wait(10000));
        QVERIFY(session.busy());

        session.set_target_language(config.get_translation_target_lang() == "JA" ? "KO" : "JA");
        QTRY_VERIFY(!session.busy());
        QCOMPARE(asked, QStringList({"Settings", "Settings"}));
        QCOMPARE(session.blocks().size(), 1);
        QCOMPARE(session.blocks()[0].translated, QString::fromUtf8("译Settings"));
    }

    // Moving the selection takes the layer off at once, and the translation follows when the
    // drag ends; a change without a drag, as from the keyboard, follows after a short pause.
    // The translation stays on all along.
    void theTranslationFollowsTheSelection() {
        const auto source = page(QSize(400, 260), Qt::white, Qt::black, {{"Settings", QRectF(60, 50, 220, 24)}});
        CanvasScene scene(source, QRectF(0, 0, 400, 260));
        QTemporaryDir dir;
        QSettings settings(dir.filePath("settings.ini"), QSettings::IniFormat);
        ToolSettingsManager config(settings);
        std::atomic_int recognitions = 0;
        set_recognizer_for_test([&](const QImage &image) {
            ++recognitions;
            return result_of({line(QString("Width %1").arg(image.width()), QRectF(10, 10, 160, 20))});
        });
        const auto restore = qScopeGuard([] { set_recognizer_for_test({}); });
        ActionTools tools(&scene, &config);
        auto *session = tools.overlay_translation();
        session->make_service = [] { return scripted_service([](const QString &text) { return translated_as(QString::fromUtf8("译") + text); }); };
        QSignalSpy shown(session, &OverlayTranslationSession::shown_changed);
        QRectF first(20, 20, 300, 200);
        scene.selection_model->set_rect(first);
        QVERIFY(session->show(first));
        QTRY_VERIFY(!session->busy());
        QVERIFY(scene.background->overlay_visible());

        scene.selection_model->start_dragging();
        QRectF moved(30, 20, 300, 200);
        scene.selection_model->set_rect(moved);
        QVERIFY(!scene.background->overlay_visible());
        QVERIFY(!session->busy());
        // Held still mid-drag for longer than the pause: nothing runs until the drag ends.
        QTest::qWait(500);
        QVERIFY(!session->busy());
        QCOMPARE(recognitions.load(), 1);
        QRectF wider(30, 20, 340, 200);
        scene.selection_model->set_rect(wider);
        scene.selection_model->stop_dragging();
        QVERIFY(session->busy());
        QTRY_VERIFY(!session->busy());
        QCOMPARE(recognitions.load(), 2);
        QCOMPARE(session->region(), QRect(30, 20, 340, 200));
        QCOMPARE(session->blocks().first().source, QString("Width 340"));
        QVERIFY(scene.background->overlay_visible());

        QRectF nudged(31, 20, 340, 200);
        scene.selection_model->set_rect(nudged);
        QVERIFY(!session->busy());
        QTRY_COMPARE(recognitions.load(), 3);
        QTRY_VERIFY(!session->busy());
        QCOMPARE(session->region(), QRect(31, 20, 340, 200));
        QVERIFY(session->shown());
        QCOMPARE(shown.count(), 1);
    }

    void aSessionReportsWhatItCouldNotDo() {
        const auto source = page(QSize(320, 120), Qt::white, Qt::black, {{"Settings", QRectF(20, 20, 220, 24)}});
        CanvasScene scene(source, QRectF(0, 0, 320, 120));
        const auto restore = qScopeGuard([] { set_recognizer_for_test({}); });
        OverlayTranslationSession session(&scene, nullptr);
        QSignalSpy failed(&session, &OverlayTranslationSession::failed);
        QSignalSpy nothing(&session, &OverlayTranslationSession::nothing_to_translate);

        // Text that comes back unchanged is already in the target language.
        set_recognizer_for_test([](const QImage &) { return result_of({line("Settings", QRectF(20, 20, 220, 24))}); });
        session.make_service = [] { return scripted_service([](const QString &text) { return translated_as(text); }); };
        QVERIFY(session.show(QRectF(0, 0, 320, 120)));
        QTRY_VERIFY(!session.busy());
        QCOMPARE(nothing.count(), 1);
        QVERIFY(!scene.background->overlay_visible());

        // Nothing recognized: the session stays on, so another try is one click away.
        set_recognizer_for_test([](const QImage &) { return result_of({}); });
        QVERIFY(session.show(QRectF(0, 0, 160, 120)));
        QTRY_VERIFY(!session.busy());
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.first().first().toString(), QString("No text was recognized in the selection."));
        QVERIFY(session.shown());
        // A run that is taken off before it ends reports nothing.
        set_recognizer_for_test([](const QImage &) {
            QThread::msleep(150);
            return result_of({});
        });
        QVERIFY(session.show(QRectF(0, 0, 100, 100)));
        QVERIFY(session.busy());
        session.hide();
        QVERIFY(!session.busy());
        QTest::qWait(300);
        QCOMPARE(failed.count(), 1);
    }

    void theCaptureShowsTheCompleteUnplacedTranslationAsPlainText() {
        const QRectF selection(0, 0, 160, 80);
        const QRectF box(10, 16, 36, 14);
        const auto source = page(selection.size().toSize(), Qt::white, Qt::black, {{"Reset", box}});
        CanvasScene scene(source, selection);
        QWidget capture;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings settings(dir.filePath("settings.ini"), QSettings::IniFormat);
        ToolSettingsManager config(settings);
        set_recognizer_for_test([box](const QImage &) { return result_of({line("Reset", box)}); });
        const auto restore = qScopeGuard([] { set_recognizer_for_test({}); });
        ActionTools actions(&scene, &config, &capture);
        auto *session = actions.overlay_translation();
        const QString full = "Reset all settings to their default values, including <custom> profiles and shortcuts.";
        session->make_service = [&] { return scripted_service([&](const QString &) { return translated_as(full); }); };
        QSignalSpy overflow(session, &OverlayTranslationSession::translations_did_not_fit);
        QSignalSpy nothing(session, &OverlayTranslationSession::nothing_to_translate);
        capture.show();
        QVERIFY(session->show(selection));
        QTRY_COMPARE(overflow.count(), 1);
        QCOMPARE(nothing.count(), 0);
        QVERIFY(!scene.background->overlay_visible());
        auto *details = capture.findChild<QPlainTextEdit *>();
        QVERIFY(details);
        QVERIFY(details->isVisible());
        QVERIFY(details->isReadOnly());
        QCOMPARE(details->toPlainText(), QString("Reset\n") + full);
    }

    // A zoomed pin shows its picture resampled under a compensating transform; the layer stays in
    // image pixels over its text, stretched until a copy resampled for that zoom is given.
    void aLayerStaysOverItsTextWhenTheBackgroundIsShownScaled() {
        QImage source(400, 200, QImage::Format_RGB32);
        source.fill(Qt::white);
        CanvasScene scene(source, QRectF(0, 0, 400, 200));
        QImage layer(400, 200, QImage::Format_ARGB32_Premultiplied);
        layer.fill(Qt::transparent);
        {
            QPainter painter(&layer);
            painter.fillRect(QRect(200, 100, 80, 40), Qt::red);
        }
        scene.background->set_overlay(layer, QPoint(0, 0));
        scene.background->set_display_pixmap(QPixmap::fromImage(source.scaled(200, 100)), QTransform::fromScale(2, 2));
        auto image = rendered(scene);
        QCOMPARE(image.pixelColor(240, 120), QColor(Qt::red));
        QCOMPARE(image.pixelColor(150, 50), QColor(Qt::white));
        QCOMPARE(image.pixelColor(320, 170), QColor(Qt::white));

        QImage half(200, 100, QImage::Format_ARGB32_Premultiplied);
        half.fill(Qt::transparent);
        {
            QPainter painter(&half);
            painter.fillRect(QRect(100, 50, 40, 20), Qt::blue);
        }
        scene.background->set_display_overlay(QPixmap::fromImage(half), QPoint(0, 0), QSize(200, 100));
        QCOMPARE(rendered(scene).pixelColor(240, 120), QColor(Qt::blue));
        // A copy for another size is not drawn, and a new layer drops it.
        scene.background->set_display_overlay(QPixmap::fromImage(half), QPoint(0, 0), QSize(100, 50));
        QCOMPARE(rendered(scene).pixelColor(240, 120), QColor(Qt::red));
        scene.background->set_display_overlay(QPixmap::fromImage(half), QPoint(0, 0), QSize(200, 100));
        scene.background->set_overlay(layer, QPoint(0, 0));
        QCOMPARE(rendered(scene).pixelColor(240, 120), QColor(Qt::red));
        // Drawn as it is again once the picture is shown at its own size.
        scene.background->set_display_pixmap(QPixmap::fromImage(source), QTransform());
        QCOMPARE(rendered(scene).pixelColor(240, 120), QColor(Qt::red));
        QCOMPARE(rendered(scene).pixelColor(150, 50), QColor(Qt::white));
    }

    // The layer is drawn again only where replies land, and ends as drawing it whole would.
    void aLayerDrawnAsRepliesComeMatchesOneDrawnWhole() {
        QVector<QPair<QString, QRectF>> texts;
        QVector<OcrLine> lines;
        for (int index = 0; index < 12; ++index) {
            const QRectF box(30 + (index % 2) * 180, 20 + (index / 2) * 46, 160, 22);
            const auto text = QString("Item %1").arg(index);
            texts.append({text, box});
            lines.append(line(text, box));
        }
        const auto source = page(QSize(400, 300), Qt::white, Qt::black, texts);
        CanvasScene scene(source, QRectF(0, 0, 400, 300));
        set_recognizer_for_test([&](const QImage &) { return result_of(lines); });
        const auto restore = qScopeGuard([] { set_recognizer_for_test({}); });
        OverlayTranslationSession session(&scene, nullptr);
        session.set_flush_interval(1);
        session.make_service = [] {
            return scripted_service([](const QString &text) {
                QThread::msleep(15);
                return translated_as(QString::fromUtf8("译文") + text + QString::fromUtf8("比原文长一些"));
            });
        };
        QSignalSpy layers(&session, &OverlayTranslationSession::layer_changed);
        QVERIFY(session.show(scene.sceneRect()));
        QTRY_VERIFY(!session.busy());
        QVERIFY2(layers.count() > 2, "the layer was drawn in one go");
        QVector<OverlayPaintedBlock> painted;
        for (const auto &entry : session.planned())
            if (entry) painted.append(*entry);
        QCOMPARE(painted.size(), 12);
        QImage whole(source.size(), QImage::Format_ARGB32_Premultiplied);
        whole.fill(Qt::transparent);
        paint_overlay_blocks(whole, painted);
        QCOMPARE(scene.background->overlay(), whole);
    }

    // A paragraph the service fails at first comes in after the others. It is still placed before
    // them, as reading order and drawing everything at once have it, and they move out of its way.
    void aReplyComingInLateIsPlacedInReadingOrder() {
        const QVector<QPair<QString, QRectF>> texts{{"Esc", QRectF(20, 30, 30, 16)}, {"Tab", QRectF(200, 30, 30, 16)}, {"OK", QRectF(300, 30, 30, 16)}};
        const auto source = page(QSize(400, 80), Qt::white, Qt::black, texts);
        CanvasScene scene(source, QRectF(0, 0, 400, 80));
        set_recognizer_for_test([&](const QImage &) {
            QVector<OcrLine> lines;
            for (const auto &text : texts) lines.append(line(text.first, text.second));
            return result_of(lines);
        });
        const auto restore = qScopeGuard([] { set_recognizer_for_test({}); });
        std::atomic_int esc_asked = 0;
        OverlayTranslationSession session(&scene, nullptr);
        session.set_flush_interval(1);
        session.make_service = [&] {
            return scripted_service([&](const QString &text) {
                if (text == "Esc" && esc_asked++ == 0) return TranslationResult::failure(TranslationErrorCode::INVALID_REQUEST, "busy");
                if (text == "Esc") return translated_as("Touche d'echappement");
                return translated_as(text == "Tab" ? QString("Touche de tabulation") : text);
            });
        };
        QVERIFY(session.show(scene.sceneRect()));
        QTRY_VERIFY(!session.busy());
        QCOMPARE(session.blocks().size(), 3);
        QVERIFY(session.blocks()[0].translated.isEmpty());
        QVERIFY(session.planned()[1].has_value());

        QVERIFY(session.show(scene.sceneRect()));
        QTRY_VERIFY(!session.busy());
        QCOMPARE(esc_asked.load(), 2);
        QVERIFY(session.planned()[0] && session.planned()[1]);
        QVERIFY(!inks_meet(*session.planned()[0], *session.planned()[1]));
        QVector<QRectF> placed;
        QVector<OverlayPaintedBlock> painted;
        for (int index = 0; index < session.blocks().size(); ++index) {
            const auto expected = plan_overlay_block(scene.background->image(), session.blocks(), index, "ZH", nullptr, placed);
            QCOMPARE(session.planned()[index].has_value(), expected.has_value());
            if (!expected) continue;
            QCOMPARE(session.planned()[index]->font.pixelSize(), expected->font.pixelSize());
            QCOMPARE(session.planned()[index]->line_origins, expected->line_origins);
            placed += overlay_text_ink(*expected);
            painted.append(*expected);
        }
        QImage whole(source.size(), QImage::Format_ARGB32_Premultiplied);
        whole.fill(Qt::transparent);
        paint_overlay_blocks(whole, painted);
        QCOMPARE(scene.background->overlay(), whole);
    }

    // A picture whose text is read elsewhere is not recognized again: the session asks its own
    // recognizer and is busy until it answers. No paragraphs end the run with the reason given,
    // and an answer for a run already stopped is dropped.
    void aSessionCanTakeItsParagraphsFromElsewhere() {
        const QRect settings_box(60, 50, 220, 24);
        const auto source = page(QSize(400, 260), Qt::white, Qt::black, {{"Settings", QRectF(settings_box)}});
        const auto plain = source.convertToFormat(QImage::Format_RGB32);
        std::atomic_int recognitions = 0;
        set_recognizer_for_test([&](const QImage &) {
            ++recognitions;
            return result_of({});
        });
        const auto restore = qScopeGuard([] { set_recognizer_for_test({}); });
        const auto blocks = group_overlay_blocks(result_of({line("Settings", QRectF(settings_box))}));
        const auto service = [] { return scripted_service([](const QString &text) { return translated_as(QString::fromUtf8("译") + text); }); };

        CanvasScene scene(source, QRectF(0, 0, 400, 260));
        OverlayTranslationSession session(&scene, nullptr);
        session.make_service = service;
        OverlayTranslationSession::RecognizeDone answer;
        int asked = 0;
        session.recognize = [&](const OverlayTranslationSession::RecognizeDone &done) {
            ++asked;
            answer = done;
        };
        QVERIFY(session.show(scene.sceneRect()));
        QVERIFY(session.busy());
        QCOMPARE(asked, 1);
        QCOMPARE(session.waiting().rect, scene.sceneRect());
        answer(blocks, {});
        QTRY_VERIFY(!session.busy());
        QCOMPARE(recognitions.load(), 0);
        QVERIFY(differing_pixels(rendered(scene), plain, settings_box) > 0);

        CanvasScene empty_scene(source, QRectF(0, 0, 400, 260));
        OverlayTranslationSession empty(&empty_scene, nullptr);
        empty.make_service = service;
        empty.recognize = [](const OverlayTranslationSession::RecognizeDone &done) { done({}, "nothing here"); };
        QSignalSpy failed(&empty, &OverlayTranslationSession::failed);
        QVERIFY(empty.show(empty_scene.sceneRect()));
        QVERIFY(!empty.busy());
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.first().first().toString(), QString("nothing here"));

        CanvasScene late_scene(source, QRectF(0, 0, 400, 260));
        OverlayTranslationSession late(&late_scene, nullptr);
        late.make_service = service;
        OverlayTranslationSession::RecognizeDone late_answer;
        late.recognize = [&](const OverlayTranslationSession::RecognizeDone &done) { late_answer = done; };
        QVERIFY(late.show(late_scene.sceneRect()));
        late.stop();
        QVERIFY(!late.busy());
        late_answer(blocks, {});
        QVERIFY(!late.busy());
        QVERIFY(late.blocks().isEmpty());
        QCOMPARE(recognitions.load(), 0);
    }

    // Paragraphs translated elsewhere are laid over the picture with nothing asked for, showing the
    // original if that is what showed there. Stopping a run keeps what came back so far.
    void aSessionAdoptsTranslationsAndStoppingKeepsThem() {
        const QRect settings_box(60, 50, 220, 24), cancel_box(60, 120, 220, 24);
        const auto source = page(QSize(400, 260), Qt::white, Qt::black, {{"Settings", QRectF(settings_box)}, {"Cancel", QRectF(cancel_box)}});
        const auto plain = source.convertToFormat(QImage::Format_RGB32);
        const auto recognized = result_of({line("Settings", QRectF(settings_box)), line("Cancel", QRectF(cancel_box))});
        std::atomic_int asked = 0;
        std::atomic_bool release = false;
        const auto service = [&] {
            return scripted_service([&](const QString &text) {
                ++asked;
                while (text == "Cancel" && !release) QThread::msleep(5);
                return translated_as(QString::fromUtf8("译") + text);
            });
        };
        auto blocks = group_overlay_blocks(recognized);
        QCOMPARE(blocks.size(), 2);
        for (auto &block : blocks) block.translated = QString::fromUtf8("译") + block.source;

        CanvasScene scene(source, QRectF(0, 0, 400, 260));
        OverlayTranslationSession session(&scene, nullptr);
        session.make_service = service;
        QVERIFY(session.adopt(scene.sceneRect(), blocks, true));
        QVERIFY(session.shown());
        QVERIFY(!session.busy());
        QCOMPARE(differing_pixels(rendered(scene), plain, plain.rect()), 0);
        session.set_original_shown(false);
        QVERIFY(differing_pixels(rendered(scene), plain, settings_box) > 0);
        QVERIFY(differing_pixels(rendered(scene), plain, cancel_box) > 0);
        QCOMPARE(asked.load(), 0);
        QVERIFY(!session.adopt(scene.sceneRect(), {}, false));

        set_recognizer_for_test([&](const QImage &) { return recognized; });
        const auto restore = qScopeGuard([&] {
            release = true;
            set_recognizer_for_test({});
        });
        CanvasScene running_scene(source, QRectF(0, 0, 400, 260));
        OverlayTranslationSession running(&running_scene, nullptr);
        running.set_flush_interval(1);
        running.make_service = service;
        QVERIFY(running.show(running_scene.sceneRect()));
        QTRY_VERIFY(differing_pixels(rendered(running_scene), plain, settings_box) > 0);
        QVERIFY(running.busy());
        running.stop();
        QVERIFY(!running.busy());
        QVERIFY(running.shown());
        QVERIFY(differing_pixels(rendered(running_scene), plain, settings_box) > 0);
        QCOMPARE(differing_pixels(rendered(running_scene), plain, cancel_box), 0);
        release = true;
        QTest::qWait(50);
    }
};

QTEST_MAIN(TranslationOverlayTests)
#include "test_translation_overlay.moc"
