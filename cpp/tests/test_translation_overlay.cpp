#include "canvas/items/background_item.h"
#include "canvas/scene.h"
#include "ocr/ocr_manager.h"
#include "translation/overlay_translation.h"
#include "translation/overlay_translation_worker.h"
#include "translation/registry.h"
#include <QColor>
#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QImage>
#include <QPainter>
#include <QSignalSpy>
#include <QTest>
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

// A translation service whose answer to each text the test decides.
using Script = std::function<TranslationResult(const QString &)>;

class ScriptedProvider : public TranslationProvider {
public:
    explicit ScriptedProvider(Script script) : script_(std::move(script)) { parallel_requests = 2; }
    QString provider_id() const override { return "scripted"; }
    QString display_name() const override { return "Scripted"; }
    bool is_configured() const override { return true; }
    TranslationResult translate(const TranslationRequest &request) override { return script_(request.text); }
private:
    Script script_;
};

class ScriptedConfig : public TranslationConfig {
public:
    QString get_translation_provider() const override { return "scripted"; }
    QVariantMap get_translation_provider_config(const QString &) const override { return {}; }
};

std::shared_ptr<TranslationService> scripted_service(const Script &script) {
    auto registry = std::make_shared<ProviderRegistry>();
    registry->register_provider("scripted", [script](const ProviderConfig &) { return std::make_unique<ScriptedProvider>(script); }, std::nullopt, "Scripted");
    return std::make_shared<TranslationService>(registry, std::make_shared<ScriptedConfig>());
}

TranslationResult translated_as(const QString &text) { return TranslationResult{true, text, {}, std::nullopt, {}}; }

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
        QCOMPARE(blocks[0].source, QString("The quick brown fox\njumps over the lazy dog\nand keeps running."));
        // The paragraph covers all three lines.
        QCOMPARE(blocks[0].rect, QRectF(10, 10, 300, 46));
        QCOMPARE(blocks[0].line_height, 14.0);
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
            line("Username", QRectF(10, 10, 300, 14)),
            line("Enter the name you signed up with.", QRectF(60, 26, 250, 14)),
        }));
        QCOMPARE(blocks.size(), 2);
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
        for (int index = 0; index < 16; ++index) lines.append(line(QString("line %1").arg(index), QRectF(10, 10 + index * 16, 200, 14)));
        const auto blocks = group_overlay_blocks(result_of(lines));
        QCOMPARE(blocks.size(), 2);
        QCOMPARE(blocks[0].lines, 12);
        QCOMPARE(blocks[1].lines, 4);
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

    void growsIntoTheBlankRoomBelow() {
        // A translation far longer than the line it replaces cannot stay in the line's own
        // height, so it takes the empty part of the page under it.
        const auto source = page(QSize(320, 200), Qt::white, QColor(20, 20, 20), {{"Note", QRectF(10, 10, 260, 18)}});
        const QString translation = QString("This note is much longer than the single word it replaces, so it needs several lines of its own.");
        int painted = 0;
        const auto layer = render_translation_overlay(source, {block_of("Note", translation, QRectF(10, 10, 260, 18), 18)}, "FR", &painted);
        QCOMPARE(painted, 1);
        // Below the block's own bottom edge (28) the page was blank, and is now covered.
        QCOMPARE(qAlpha(layer.pixel(150, 40)), 255);
    }

    void doesNotGrowOverSomethingElse() {
        // The same long translation, but the page under it is not blank: growth is refused
        // and the text stays shrunk inside its own line rather than painting over the icon.
        auto source = page(QSize(320, 200), Qt::white, QColor(20, 20, 20),
                           {{"Note", QRectF(10, 10, 260, 18)}, {"Following text", QRectF(10, 50, 260, 18)}});
        const QColor icon(220, 40, 40);
        {
            QPainter painter(&source);
            painter.fillRect(QRect(10, 34, 260, 10), icon);
        }
        const QString translation = QString("This note is much longer than the single word it replaces, so it needs several lines of its own.");
        int painted = 0;
        const auto layer = render_translation_overlay(
            source, {block_of("Note", translation, QRectF(10, 10, 260, 18), 18), block_of("Following text", "Texte suivant", QRectF(10, 50, 260, 18), 18)}, "FR",
            &painted);
        QCOMPARE(painted, 2);
        // Nothing was painted over the icon, which is not the page's background.
        QCOMPARE(qAlpha(layer.pixel(150, 40)), 0);
        // The block's own line, and no more, carries the translation.
        QCOMPARE(qAlpha(layer.pixel(150, 18)), 255);
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
        const auto run = [&](const Script &script) {
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

    void appliesAsOneUndoableStep() {
        const auto source = page(QSize(320, 120), Qt::white, QColor(20, 20, 20), {{"Hello world", QRectF(10, 10, 260, 18)}});
        CanvasScene scene(source, QRectF(0, 0, 320, 120));
        QVERIFY(scene.background);
        QVERIFY(scene.undo_stack);
        QCOMPARE(scene.background->image(), source);

        int painted = 0;
        const auto layer = render_translation_overlay(source, {block_of("Hello world", "Bonjour le monde", QRectF(10, 10, 260, 18), 18)}, "FR", &painted);
        QCOMPARE(painted, 1);
        apply_translation_overlay(&scene, layer, QPoint(0, 0), QStringLiteral("Translate Overlay"));

        auto translated = scene.background->image();
        QVERIFY(differing_pixels(translated, source, QRect(8, 6, 264, 26)) > 20);
        // The pixels outside the layer are the same ones.
        QCOMPARE(translated.copy(QRect(0, 100, 320, 20)), source.copy(QRect(0, 100, 320, 20)));

        // One step, and the capture is exactly what was taken.
        QCOMPARE(scene.undo_stack->count(), 1);
        scene.undo_stack->undo();
        QCOMPARE(scene.background->image(), source);
        scene.undo_stack->redo();
        QVERIFY(differing_pixels(scene.background->image(), source, QRect(8, 6, 264, 26)) > 20);
    }

    void appliesAtTheSelectionsOffset() {
        // The capture is the whole virtual desktop, so the layer goes in at the selection's
        // own place rather than the top-left corner.
        const auto source = page(QSize(320, 120), Qt::white, QColor(20, 20, 20), {{"Hello world", QRectF(150, 40, 160, 18)}});
        CanvasScene scene(source, QRectF(0, 0, 320, 120));
        int painted = 0;
        const auto layer = render_translation_overlay(source.copy(QRect(140, 30, 180, 40)),
                                                      {block_of("Hello world", "Bonjour", QRectF(10, 10, 160, 18), 18)}, "FR", &painted);
        QCOMPARE(painted, 1);
        apply_translation_overlay(&scene, layer, QPoint(140, 30), QStringLiteral("Translate Overlay"));
        const auto translated = scene.background->image();
        // Changed inside the selection only.
        QVERIFY(differing_pixels(translated, source, QRect(140, 30, 180, 40)) > 20);
        QCOMPARE(translated.copy(QRect(0, 0, 140, 120)), source.copy(QRect(0, 0, 140, 120)));
    }

    void ignoresAnApplyWithNothingToDo() {
        const auto source = page(QSize(64, 64), Qt::white, Qt::black, {});
        CanvasScene scene(source, QRectF(0, 0, 64, 64));
        // No layer, no scene: nothing is pushed, so Ctrl+Z keeps meaning the step before.
        apply_translation_overlay(&scene, QImage(), QPoint(0, 0), QStringLiteral("Translate Overlay"));
        apply_translation_overlay(nullptr, QImage(4, 4, QImage::Format_ARGB32), QPoint(0, 0), QStringLiteral("Translate Overlay"));
        // A layer that lands entirely outside the capture is dropped too.
        auto offscreen = QImage(8, 8, QImage::Format_ARGB32_Premultiplied);
        offscreen.fill(Qt::red);
        apply_translation_overlay(&scene, offscreen, QPoint(500, 500), QStringLiteral("Translate Overlay"));
        QCOMPARE(scene.undo_stack->count(), 0);
    }
};

QTEST_MAIN(TranslationOverlayTests)
#include "test_translation_overlay.moc"
