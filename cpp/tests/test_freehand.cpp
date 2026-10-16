#include "canvas/items/drawing_items.h"
#include "canvas/items/stroke_item.h"
#include "canvas/scene.h"
#include "canvas/undo.h"
#include "settings/tool_settings.h"
#include "tools/base.h"
#include "tools/highlighter.h"
#include "tools/pen.h"
#include <QApplication>
#include <QFile>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QScreen>
#include <QWindow>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLinearGradient>
#include <QPainter>
#include <QSettings>
#include <QSignalSpy>
#include <QStyleOptionGraphicsItem>
#include <QTemporaryDir>
#include <QTest>
#include <QtMath>
#include <cmath>
#include <functional>
#include <memory>

using namespace jietuba;

namespace {
constexpr int W = 360, H = 240;

QPointF point(const QJsonValue &value) {
    const auto pair = value.toArray();
    return {pair[0].toDouble(), pair[1].toDouble()};
}

bool near(double a, double b) { return std::abs(a - b) <= 1e-6; }

QPen test_pen(const QString &kind) {
    QPen pen = kind == "highlighter" ? QPen(QColor(255, 210, 0, 140), 12) : QPen(QColor(220, 40, 60, kind == "solid" ? 255 : 160), 4);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    if (kind == "dashed") {
        pen.setStyle(Qt::DashLine);
        pen.setDashPattern({3, 2});
    }
    return pen;
}

QImage background() {
    QImage image(W, H, QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&image);
    QLinearGradient gradient(0, 0, W, H);
    gradient.setColorAt(0, QColor("#2a3d66"));
    gradient.setColorAt(1, QColor("#e8eefc"));
    painter.fillRect(image.rect(), gradient);
    painter.end();
    return image;
}

// The background is a prepared image: a gradient brush differs by one level in partial repaints,
// a pixmap blits identically.
std::unique_ptr<QGraphicsScene> make_scene() {
    auto scene = std::make_unique<QGraphicsScene>(0, 0, W, H);
    scene->addItem(new QGraphicsPixmapItem(QPixmap::fromImage(background())));
    return scene;
}

QImage render(QGraphicsScene &scene, const QRectF &source = QRectF(0, 0, W, H)) {
    QImage image(int(source.width()), int(source.height()), QImage::Format_ARGB32_Premultiplied);
    image.fill(0);
    QPainter painter(&image);
    scene.render(&painter, QRectF(0, 0, source.width(), source.height()), source);
    painter.end();
    return image;
}

// A slow, dense loop (integer samples) and a few fast strokes: long enough to span several chunks.
QList<QPointF> loop_points() {
    QList<QPointF> points;
    for (int i = 0; i < 260; ++i) {
        const double a = i / 260.0 * 2 * M_PI;
        points.append(QPointF(std::nearbyint(170 + 110 * std::cos(a) + 6 * std::sin(a * 9)), std::nearbyint(120 + 80 * std::sin(a))));
    }
    for (int i = 1; i < 8; ++i) points.append(QPointF(170 + 20 * i, 120 + (i % 2 ? 25 : -25)));
    return points;
}

// Draws loop_points into a StrokeItem the way the pen tool does, not finished yet; with_line holds
// Shift for steps 120..139.
struct Drawn {
    FreehandStroke stroke;
    StrokeItem *item;
};
Drawn draw(QGraphicsScene &scene, const QString &kind, bool with_line = false, const std::function<void(StrokeItem *)> &on_step = {}) {
    const auto points = loop_points();
    Drawn drawn{FreehandStroke(points[0], 45, true), nullptr};
    drawn.item = new StrokeItem(drawn.stroke.path, test_pen(kind), kind == "highlighter");
    scene.addItem(drawn.item);
    for (int k = 0; k + 1 < points.size(); ++k) {
        const bool straight = with_line && k >= 120 && k < 140;
        const QPointF target = straight ? QPointF(points[k + 1].x(), points[k + 1].y() + (k - 120) * 4) : points[k + 1];
        if (drawn.stroke.extend(target, straight)) {
            drawn.item->extend_path(drawn.stroke.path, drawn.stroke.take_dirty_from());
            if (on_step) on_step(drawn.item);
        }
    }
    return drawn;
}

// A stroke built from a whole path at once, as pin windows clone them.
StrokeItem *built(QGraphicsScene &scene, const QString &kind, std::optional<QPainterPath> path = std::nullopt) {
    if (!path) {
        QGraphicsScene scratch;
        path = draw(scratch, kind).stroke.path;
    }
    auto *item = new StrokeItem(*path, test_pen(kind), kind == "highlighter");
    scene.addItem(item);
    return item;
}

// Sweeps back and forth over a few rows: dozens of chunks, apart from each other on screen.
QPainterPath long_path() {
    FreehandStroke stroke(QPointF(20, 40), 45, true);
    for (int i = 1; i < 1400; ++i) {
        const double travelled = i * 0.9, row = std::floor(travelled / 320), along = travelled - row * 320;
        const double x = int(row) % 2 == 0 ? 20 + along : 340 - along;
        stroke.extend(QPointF(x, 40 + 50 * row + 15 * std::sin(i / 7.0)), false);
    }
    return stroke.path;
}

int differing(const QImage &a, const QImage &b) {
    int count = 0;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x) count += a.pixel(x, y) != b.pixel(x, y);
    return count;
}
}

namespace {
// The view repaints itself piece by piece; grabWindow reads what has piled up in the window while a
// fresh grab repaints everything. A pixel left behind anywhere makes them differ.
class Screen {
public:
    explicit Screen(QGraphicsScene *scene, qreal scale = 1, QPointF centre = QPointF(180, 120)) : view(scene) {
        view.setViewportUpdateMode(QGraphicsView::SmartViewportUpdate);  // as CanvasView
        view.setFrameShape(QFrame::NoFrame);
        view.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        view.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        view.resize(W, H);
        view.scale(scale, scale);
        view.centerOn(centre);
        view.show();
        settle();
    }
    static void settle() {
        for (int i = 0; i < 3; ++i) QCoreApplication::processEvents();
    }
    int stale_pixels() {
        settle();
        const QImage shown = view.windowHandle()->screen()->grabWindow(view.winId()).toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
        const QImage fresh = view.grab().toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
        return shown == fresh ? 0 : differing(shown, fresh);
    }
    QGraphicsView view;
};

QImage gradient_image() {
    QImage image(W, H, QImage::Format_ARGB32);
    QPainter painter(&image);
    QLinearGradient gradient(0, 0, W, H);
    gradient.setColorAt(0, QColor("#2a3d66"));
    gradient.setColorAt(1, QColor("#e8eefc"));
    painter.fillRect(image.rect(), gradient);
    painter.end();
    return image;
}

// Slow drawing, a few big jumps, a Shift line flung across the whole canvas, freehand again, then
// out past where the stroke started.
QList<std::pair<QPointF, bool>> gesture() {
    QList<std::pair<QPointF, bool>> steps;
    for (int i = 0; i < 60; ++i) steps.append({QPointF(40 + i * 1.6, 60 + 14 * std::sin(i / 6.0)), false});
    for (const QPointF &p : {QPointF(330, 40), QPointF(60, 230), QPointF(300, 210), QPointF(140, 20)}) steps.append({p, false});
    for (const QPointF &p : {QPointF(355, 10), QPointF(5, 235), QPointF(355, 235), QPointF(5, 5), QPointF(180, 238), QPointF(358, 120), QPointF(150, 130)})
        steps.append({p, true});
    for (int i = 0; i < 40; ++i) steps.append({QPointF(150 + i * 5, 130 + 40 * std::sin(i / 4.0)), false});
    for (const QPointF &p : {QPointF(-60, -40), QPointF(420, 300), QPointF(200, 120)}) steps.append({p, false});
    return steps;
}

ToolContext canvas_context(CanvasScene &scene) {
    ToolContext ctx;
    ctx.scene = &scene;
    ctx.selection = scene.selection_model;
    ctx.undo_stack = scene.undo_stack;
    ctx.color = QColor("#e5484d");
    ctx.stroke_width = 4;
    ctx.opacity = 0.8;
    return ctx;
}

template <typename T> int count_items(QGraphicsScene &scene) {
    int count = 0;
    for (auto *item : scene.items()) count += dynamic_cast<T *>(item) != nullptr;
    return count;
}
}

class FreehandTests : public QObject {
    Q_OBJECT
    QJsonObject fixtures_;
private slots:
    void initTestCase() {
        QFile file(JIETUBA_FREEHAND_FIXTURE_PATH);
        QVERIFY(file.open(QIODevice::ReadOnly));
        fixtures_ = QJsonDocument::fromJson(file.readAll()).object();
        QVERIFY(!fixtures_.isEmpty());
    }

    void snapsLikeTheReference() {
        for (const auto &value : fixtures_["snaps"].toArray()) {
            const auto o = value.toObject();
            const auto end = snap_line_end(point(o["anchor"]), point(o["pos"]), o["step"].toInt());
            QVERIFY2(near(end.x(), point(o["end"]).x()) && near(end.y(), point(o["end"]).y()), QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
        }
    }

    // A horizontal or vertical line's other coordinate equals the anchor's exactly; a hair off
    // renders as a one-pixel step.
    void axisSnapsAreExact() {
        const QPointF anchor(10.5, 20.25);
        for (int step : {90, 45, 30, 15}) {
            QCOMPARE(snap_line_end(anchor, QPointF(300, 23), step).y(), anchor.y());
            QCOMPARE(snap_line_end(anchor, QPointF(12, -300), step).x(), anchor.x());
        }
    }

    // Every step's change flag, rewritten range and tip, the release, and the finished path, as the
    // reference produces them.
    void strokesLikeTheReference() {
        int scenario = 0;
        for (const auto &value : fixtures_["strokes"].toArray()) {
            const auto o = value.toObject();
            FreehandStroke stroke(point(o["start"]), o["angle_step"].toInt(), o["smooth"].toBool());
            int index = 0;
            for (const auto &step_value : o["steps"].toArray()) {
                const auto step = step_value.toObject();
                const auto where = QString("stroke %1 step %2").arg(scenario).arg(index++).toUtf8();
                QVERIFY2(stroke.extend(point(step["pos"]), step["straight"].toBool()) == step["changed"].toBool(), where.constData());
                QVERIFY2(stroke.take_dirty_from() == step["dirty_from"].toInt(), where.constData());
                QVERIFY2(stroke.path.elementCount() == step["count"].toInt(), where.constData());
                const auto last = stroke.path.elementAt(stroke.path.elementCount() - 1);
                QVERIFY2(near(last.x, point(step["last"]).x()) && near(last.y, point(step["last"]).y()), where.constData());
            }
            const auto where = QString("stroke %1 release").arg(scenario).toUtf8();
            QVERIFY2(stroke.finish() == o["finished"].toBool(), where.constData());
            QVERIFY2(stroke.take_dirty_from() == o["finish_dirty_from"].toInt(), where.constData());
            const auto elements = o["elements"].toArray();
            QCOMPARE(stroke.path.elementCount(), int(elements.size()));
            for (int i = 0; i < elements.size(); ++i) {
                const auto e = elements[i].toArray();
                const auto actual = stroke.path.elementAt(i);
                QVERIFY2(near(actual.x, e[0].toDouble()) && near(actual.y, e[1].toDouble()) && int(actual.type) == e[2].toInt(),
                         QString("stroke %1 element %2").arg(scenario).arg(i).toUtf8().constData());
            }
            ++scenario;
        }
        QVERIFY(scenario > 0);
    }

    void settingDefaultsAndRejectsUnknownAngles() {
        QTemporaryDir dir;
        QSettings settings(dir.filePath("settings.ini"), QSettings::IniFormat);
        ToolSettingsManager manager(settings);
        QCOMPARE(DEFAULT_SHIFT_LINE_ANGLE, fixtures_["default_shift_line_angle"].toInt());
        QCOMPARE(manager.get_shift_line_angle(), DEFAULT_SHIFT_LINE_ANGLE);
        QList<int> angles;
        for (const auto &value : fixtures_["shift_line_angles"].toArray()) angles.append(value.toInt());
        QCOMPARE(SHIFT_LINE_ANGLES, angles);
        for (int angle : SHIFT_LINE_ANGLES) {
            manager.set_shift_line_angle(angle);
            QCOMPARE(manager.get_shift_line_angle(), angle);
        }
        manager.set_shift_line_angle(15);
        manager.set_shift_line_angle(60);
        QCOMPARE(manager.get_shift_line_angle(), DEFAULT_SHIFT_LINE_ANGLE);
        for (const char *stored : {"", "abc", "60", "-45", "1.5"}) {
            settings.setValue("app/shift_line_angle", stored);
            QCOMPARE(manager.get_shift_line_angle(), DEFAULT_SHIFT_LINE_ANGLE);
        }
        ToolContext ctx;
        QCOMPARE(shift_line_angle(ctx), DEFAULT_SHIFT_LINE_ANGLE);
        manager.set_shift_line_angle(15);
        ctx.settings_manager = &manager;
        QCOMPARE(shift_line_angle(ctx), 15);
    }

    void drawingItBitByBitLooksExactlyLikeBuildingItInOneGo_data() {
        QTest::addColumn<QString>("kind");
        for (const char *kind : {"solid", "translucent", "highlighter", "dashed"}) QTest::newRow(kind) << QString(kind);
    }
    void drawingItBitByBitLooksExactlyLikeBuildingItInOneGo() {
        QFETCH(QString, kind);
        auto drawn_scene = make_scene(), built_scene = make_scene();
        draw(*drawn_scene, kind);
        built(*built_scene, kind);
        QCOMPARE(differing(render(*drawn_scene), render(*built_scene)), 0);
    }

    // extend_path copies only the tail the pen changed; the pen editing its own path in place does
    // not reach the item.
    void theItemKeepsItsOwnPathWhileDrawing() {
        auto scene = make_scene();
        auto drawn = draw(*scene, "solid", true);
        QVERIFY(drawn.item->path() == drawn.stroke.path);
        const auto before = drawn.item->path();
        drawn.stroke.path.setElementPositionAt(drawn.stroke.path.elementCount() - 1, 0, 0);
        QVERIFY(drawn.item->path() == before);
    }

    // A wholly exposed solid stroke is one drawPath, pixel for pixel what Qt draws.
    void solidStrokesLookExactlyAsQtDrawsThePath_data() {
        QTest::addColumn<QString>("kind");
        for (const char *kind : {"solid", "translucent", "highlighter"}) QTest::newRow(kind) << QString(kind);
    }
    void solidStrokesLookExactlyAsQtDrawsThePath() {
        QFETCH(QString, kind);
        auto scene = make_scene();
        auto *item = built(*scene, kind);
        QImage expected = background();
        QPainter painter(&expected);
        if (kind == "highlighter") painter.setCompositionMode(QPainter::CompositionMode_Multiply);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(item->pen());
        painter.drawPath(item->path());
        painter.end();
        QCOMPARE(differing(render(*scene), expected), 0);
    }

    // A preview painted with a fresh QStyleOptionGraphicsItem has no exposed area; it still draws the
    // whole stroke.
    void paintingWithABareStyleOptionDrawsTheWholeStroke_data() {
        QTest::addColumn<QString>("kind");
        for (const char *kind : {"solid", "dashed"}) QTest::newRow(kind) << QString(kind);
    }
    void paintingWithABareStyleOptionDrawsTheWholeStroke() {
        QFETCH(QString, kind);
        auto scene = make_scene();
        auto *item = built(*scene, kind);
        const QImage expected = render(*scene);
        auto plain = make_scene();
        QImage image = render(*plain);
        QPainter painter(&image);
        const QStyleOptionGraphicsItem option;
        item->paint(&painter, &option);
        painter.end();
        QCOMPARE(differing(image, expected), 0);
    }

    // The view repaints only the dirty area and paint draws only the chunks meeting it; inside that
    // area the pixels must match a full repaint.
    void partialRepaintMatchesTheFullOne_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<bool>("drawn_live");
        for (const char *kind : {"solid", "highlighter", "dashed"}) {
            QTest::newRow(QString("%1 drawn").arg(kind).toUtf8().constData()) << QString(kind) << true;
            QTest::newRow(QString("%1 built").arg(kind).toUtf8().constData()) << QString(kind) << false;
        }
    }
    void partialRepaintMatchesTheFullOne() {
        QFETCH(QString, kind);
        QFETCH(bool, drawn_live);
        auto scene = make_scene();
        if (drawn_live) draw(*scene, kind);
        else built(*scene, kind);
        const QImage full = render(*scene);
        for (const QRectF &rect : {QRectF(250, 90, 40, 40), QRectF(150, 30, 60, 24), QRectF(0, 0, 90, 90), QRectF(57, 171, 31, 17)}) {
            const QImage crop = full.copy(rect.toRect());
            QCOMPARE(differing(render(*scene, rect), crop), 0);
        }
    }

    // When something repaints one corner of the stroke, the stroke draws the chunks there, not all of it.
    void aPartialRepaintDrawsOnlyTheNearbyChunks_data() {
        QTest::addColumn<QString>("kind");
        for (const char *kind : {"solid", "dashed"}) QTest::newRow(kind) << QString(kind);
    }
    void aPartialRepaintDrawsOnlyTheNearbyChunks() {
        QFETCH(QString, kind);
        auto scene = make_scene();
        auto *item = built(*scene, kind, long_path());
        QVERIFY(item->chunks().size() > 20);
        const QRectF corner(150, 80, 20, 20);
        if (kind == "dashed")
            QVERIFY(item->dash_outline(corner).elementCount() * 8 < item->dash_outline(item->boundingRect()).elementCount());
        else
            QVERIFY(item->visible_path(corner).elementCount() * 8 < item->path().elementCount());
    }

    // What each step marks dirty covers every pixel that actually changed, including where a
    // Shift-held line used to be.
    void markedRectsCoverEveryChangedPixel_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<bool>("with_line");
        for (const char *kind : {"solid", "dashed"}) {
            QTest::newRow(QString("%1 freehand").arg(kind).toUtf8().constData()) << QString(kind) << false;
            QTest::newRow(QString("%1 shift line").arg(kind).toUtf8().constData()) << QString(kind) << true;
        }
    }
    void markedRectsCoverEveryChangedPixel() {
        QFETCH(QString, kind);
        QFETCH(bool, with_line);
        auto scene = make_scene();
        QImage previous = render(*scene);
        QSignalSpy changed(scene.get(), &QGraphicsScene::changed);
        int missed = 0;
        draw(*scene, kind, with_line, [&](StrokeItem *) {
            QCoreApplication::processEvents();
            QRectF marked;
            for (const auto &args : changed)
                for (const auto &rect : args.at(0).value<QList<QRectF>>()) marked = marked.united(rect);
            changed.clear();
            const QImage current = render(*scene);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                    if (previous.pixel(x, y) != current.pixel(x, y) && !marked.contains(QPointF(x + 0.5, y + 0.5))) ++missed;
            previous = current;
        });
        QCOMPARE(missed, 0);
    }

    // Growing the bounds repaints them whole, so while drawing they grow by a wide margin and a stroke
    // that keeps to one area grows them only a few times; finishing tightens them so the selection
    // handles sit close to the stroke.
    void boundsGrowInBigStepsAndTightenWhenFinished() {
        auto scene = make_scene();
        int grows = 0;
        QRectF bounds;
        const auto drawn = draw(*scene, "solid", false, [&](StrokeItem *item) {
            if (item->boundingRect() != bounds) ++grows;
            bounds = item->boundingRect();
        });
        QVERIFY(grows <= 3);
        for (const auto &chunk : drawn.item->chunks()) QVERIFY(drawn.item->boundingRect().contains(chunk.rect));
        drawn.item->finish_path();
        const StrokeItem fresh(drawn.stroke.path, test_pen("solid"));
        QCOMPARE(drawn.item->boundingRect(), fresh.boundingRect());
        QVERIFY(drawn.item->boundingRect().contains(drawn.item->shape().boundingRect()));
    }

    // The bounds hug the curve. Painted pixels outside them would never be erased, hit area outside
    // them could never be clicked.
    void boundsHoldEveryPaintedPixelAndTheWholeHitArea_data() {
        QTest::addColumn<qreal>("width");
        QTest::addColumn<bool>("round");
        for (const qreal width : {1., 4., 12., 40.}) QTest::newRow(QString("round %1").arg(width).toUtf8().constData()) << width << true;
        for (const qreal width : {4., 12.}) QTest::newRow(QString("square %1").arg(width).toUtf8().constData()) << width << false;
    }
    void boundsHoldEveryPaintedPixelAndTheWholeHitArea() {
        QFETCH(qreal, width);
        QFETCH(bool, round);
        QPen pen(QColor(220, 40, 60), width);
        if (round) {
            pen.setCapStyle(Qt::RoundCap);
            pen.setJoinStyle(Qt::RoundJoin);
        }
        QGraphicsScene scratch;
        const auto path = draw(scratch, "solid", true).stroke.path;
        QGraphicsScene scene;
        auto *item = new StrokeItem(path, pen);
        scene.addItem(item);
        const QRectF bounds = item->boundingRect();
        QVERIFY(bounds.contains(item->shape().boundingRect()));
        const QRectF source = bounds.adjusted(-8, -8, 8, 8);
        const QImage image = render(scene, source);
        int outside = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                outside += qAlpha(image.pixel(x, y)) && !bounds.contains(source.topLeft() + QPointF(x + 0.5, y + 0.5));
        QCOMPARE(outside, 0);
    }

    // A colour change keeps the chunks and dash outlines, a width change cuts them again; both must
    // draw what a new stroke with that pen draws.
    void restylingAFinishedStrokeDrawsTheNewStyle_data() {
        QTest::addColumn<QString>("kind");
        for (const char *kind : {"solid", "dashed"}) QTest::newRow(kind) << QString(kind);
    }
    void restylingAFinishedStrokeDrawsTheNewStyle() {
        QFETCH(QString, kind);
        auto scene = make_scene();
        const auto drawn = draw(*scene, kind);
        drawn.item->finish_path();
        render(*scene, QRectF(250, 90, 40, 40));  // chunks cut and outlines worked out beforehand

        QPen recolored = drawn.item->pen();
        recolored.setColor(QColor(30, 160, 90, 200));
        drawn.item->setPen(recolored);
        auto expected = make_scene();
        expected->addItem(new StrokeItem(drawn.stroke.path, recolored));
        QCOMPARE(differing(render(*scene), render(*expected)), 0);

        drawn.item->set_stroke_width(9);
        const QPen widened = drawn.item->pen();
        expected = make_scene();
        expected->addItem(new StrokeItem(drawn.stroke.path, widened));
        QCOMPARE(differing(render(*scene), render(*expected)), 0);
        const StrokeItem fresh(drawn.stroke.path, widened);
        QCOMPARE(drawn.item->boundingRect(), fresh.boundingRect());
    }

    void noStalePixelsWhileDrawingAndAfterRelease_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<qreal>("scale");
        QTest::newRow("solid") << QString("solid") << 1.0;
        QTest::newRow("translucent") << QString("translucent") << 1.0;
        QTest::newRow("dashed") << QString("dashed") << 1.0;
        QTest::newRow("highlighter") << QString("highlighter") << 1.0;
        QTest::newRow("zoomed in") << QString("solid") << 2.5;
        QTest::newRow("zoomed in dashed") << QString("dashed") << 2.5;
        QTest::newRow("zoomed out") << QString("solid") << 0.5;
    }
    void noStalePixelsWhileDrawingAndAfterRelease() {
        QFETCH(QString, kind);
        QFETCH(qreal, scale);
        CanvasScene scene(gradient_image(), QRectF(0, 0, W, H), false);
        Screen screen(&scene, scale);
        FreehandStroke stroke(QPointF(40, 60), 45, true);
        auto *item = new StrokeItem(stroke.path, test_pen(kind), kind == "highlighter");
        scene.addItem(item);
        QList<int> stale;
        int step = 0;
        for (const auto &[pos, straight] : gesture()) {
            if (stroke.extend(pos, straight)) item->extend_path(stroke.path, stroke.take_dirty_from());
            if (screen.stale_pixels()) stale.append(step);
            ++step;
        }
        QVERIFY2(stale.isEmpty(), QString("stale after steps %1").arg(QStringList([&] { QStringList s; for (int k : stale) s << QString::number(k); return s; }()).join(',')).toUtf8().constData());
        item->finish_path();
        const StrokeItem fresh(item->path(), item->pen());
        QCOMPARE(item->boundingRect(), fresh.boundingRect());
        QCOMPARE(screen.stale_pixels(), 0);
    }

    // Other items dragged across a finished stroke repaint it in part; the dashes must still line up
    // with the whole stroke.
    void otherItemsRepaintingOverAFinishedStrokeLeaveNothingBehind_data() {
        QTest::addColumn<QString>("kind");
        for (const char *kind : {"solid", "dashed"}) QTest::newRow(kind) << QString(kind);
    }
    void otherItemsRepaintingOverAFinishedStrokeLeaveNothingBehind() {
        QFETCH(QString, kind);
        CanvasScene scene(gradient_image(), QRectF(0, 0, W, H), false);
        Screen screen(&scene);
        FreehandStroke stroke(QPointF(40, 60), 45, true);
        for (const auto &[pos, straight] : gesture()) stroke.extend(pos, straight);
        auto *item = new StrokeItem(stroke.path, test_pen(kind), false);
        scene.addItem(item);
        QCOMPARE(screen.stale_pixels(), 0);
        for (const QRectF &rect : {QRectF(100, 60, 30, 30), QRectF(200, 100, 17, 41), QRectF(40, 150, 60, 9)}) {
            item->update(rect);
            QCOMPARE(screen.stale_pixels(), 0);
        }
        auto *marker = new NumberItem(1, QPointF(20, 20), 12, QColor("#3b82f6"));
        scene.addItem(marker);
        for (int i = 0; i < 30; ++i) {
            marker->setPos(20 + i * 11, 30 + 6 * i);
            QCOMPARE(screen.stale_pixels(), 0);
        }
    }

    void movingAFinishedStrokeLeavesNothingBehind() {
        CanvasScene scene(gradient_image(), QRectF(0, 0, W, H), false);
        Screen screen(&scene);
        FreehandStroke stroke(QPointF(40, 60), 45, true);
        for (const auto &[pos, straight] : gesture().mid(0, 70)) stroke.extend(pos, straight);
        auto *item = new StrokeItem(stroke.path, test_pen("solid"), false);
        scene.addItem(item);
        for (const QPointF &offset : {QPointF(30, 10), QPointF(90, -20), QPointF(-40, 60), QPointF(150, 100), QPointF(0, 0)}) {
            item->setPos(offset);
            QCOMPARE(screen.stale_pixels(), 0);
        }
        // A thinner pen shrinks the bounds; the old edge must be erased.
        item->set_stroke_width(1);
        QCOMPARE(screen.stale_pixels(), 0);
    }

    void switchingToolsMidStrokeKeepsTheStroke_data() {
        QTest::addColumn<QString>("kind");
        QTest::newRow("pen") << QString("pen");
        QTest::newRow("highlighter") << QString("highlighter");
    }
    void switchingToolsMidStrokeKeepsTheStroke() {
        QFETCH(QString, kind);
        CanvasScene scene(gradient_image(), QRectF(0, 0, W, H), false);
        Screen screen(&scene);
        auto ctx = canvas_context(scene);
        std::unique_ptr<Tool> tool = kind == "pen" ? std::unique_ptr<Tool>(new PenTool) : std::unique_ptr<Tool>(new HighlighterTool);
        const int before = scene.undo_stack->count();
        tool->on_press(QPointF(40, 60), Qt::LeftButton, ctx);
        for (const auto &[pos, straight] : gesture().mid(0, 70)) tool->on_move(pos, ctx);
        tool->on_deactivate(ctx);
        QCOMPARE(count_items<StrokeItem>(scene), 1);
        QCOMPARE(scene.undo_stack->count(), before + 1);
        // A release arriving after the switch is not this tool's and must not touch the stroke.
        tool->on_release(QPointF(300, 200), ctx);
        QCOMPARE(scene.undo_stack->count(), before + 1);
        QCOMPARE(screen.stale_pixels(), 0);
        scene.undo_stack->undo();
        QCOMPARE(count_items<StrokeItem>(scene), 0);
        QCOMPARE(screen.stale_pixels(), 0);
    }

    // Focus stolen while the button was down: the release never arrives. The next press keeps the
    // previous stroke as released.
    void aPressWithoutThePreviousReleaseKeepsBothStrokes_data() { switchingToolsMidStrokeKeepsTheStroke_data(); }
    void aPressWithoutThePreviousReleaseKeepsBothStrokes() {
        QFETCH(QString, kind);
        CanvasScene scene(gradient_image(), QRectF(0, 0, W, H), false);
        Screen screen(&scene);
        auto ctx = canvas_context(scene);
        std::unique_ptr<Tool> tool = kind == "pen" ? std::unique_ptr<Tool>(new PenTool) : std::unique_ptr<Tool>(new HighlighterTool);
        const int before = scene.undo_stack->count();
        tool->on_press(QPointF(40, 60), Qt::LeftButton, ctx);
        for (const auto &[pos, straight] : gesture().mid(0, 40)) tool->on_move(pos, ctx);
        tool->on_press(QPointF(200, 200), Qt::LeftButton, ctx);
        for (int i = 1; i < 30; ++i) tool->on_move(QPointF(200 + i * 4, 200 - i * 2), ctx);
        tool->on_release(QPointF(316, 142), ctx);
        QCOMPARE(count_items<StrokeItem>(scene), 2);
        QCOMPARE(scene.undo_stack->count(), before + 2);
        QCOMPARE(screen.stale_pixels(), 0);
    }

    // Pressed and released without moving (jitter under the point spacing): a dot of the pen's
    // width where it was pressed, which can be clicked and takes one undo step.
    void aClickWithoutMovingLeavesADot_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("line_style");
        QTest::newRow("pen") << QString("pen") << QString("solid");
        QTest::newRow("pen dashed") << QString("pen") << QString("dashed");
        QTest::newRow("highlighter") << QString("highlighter") << QString("solid");
    }
    void aClickWithoutMovingLeavesADot() {
        QFETCH(QString, kind);
        QFETCH(QString, line_style);
        CanvasScene scene(gradient_image(), QRectF(0, 0, W, H), false);
        auto ctx = canvas_context(scene);
        std::unique_ptr<Tool> tool;
        if (kind == "pen") {
            auto *pen = new PenTool;
            pen->line_style = line_style;
            tool.reset(pen);
        } else {
            tool.reset(new HighlighterTool);
        }
        const int steps = scene.undo_stack->count();
        const QImage before = render(scene);
        tool->on_press(QPointF(100, 100), Qt::LeftButton, ctx);
        tool->on_move(QPointF(100.4, 100.3), ctx);
        tool->on_release(QPointF(100.4, 100.3), ctx);
        QCOMPARE(count_items<StrokeItem>(scene), 1);
        QCOMPARE(scene.undo_stack->count(), steps + 1);
        StrokeItem *item = nullptr;
        for (auto *candidate : scene.items())
            if ((item = dynamic_cast<StrokeItem *>(candidate))) break;
        QVERIFY(item->contains(QPointF(100, 100)));

        const QImage after = render(scene);
        const double radius = item->pen().widthF() / 2;
        int inked = 0, left = W, right = -1, top = H, bottom = -1;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                if (before.pixel(x, y) == after.pixel(x, y)) continue;
                ++inked;
                QVERIFY(std::hypot(x + 0.5 - 100, y + 0.5 - 100) <= radius + 1);
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
        QVERIFY(inked >= M_PI * radius * radius);
        QVERIFY(right - left + 1 >= 2 * radius && bottom - top + 1 >= 2 * radius);
    }
};

QTEST_MAIN(FreehandTests)
#include "test_freehand.moc"
