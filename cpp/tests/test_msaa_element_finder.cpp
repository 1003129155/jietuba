#include "capture/msaa_element_finder.h"
#include "capture/window_finder.h"
#include "canvas/scene.h"
#include "canvas/view.h"
#include "canvas/smart_selection_anim.h"
#include "ui/initial_selection.h"
#include <QApplication>
#include <QCursor>
#include <QFile>
#include <QJsonArray>
#include <QMouseEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QSettings>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QTest>
#include "capture/capture_service.h"
#include "canvas/selection_model.h"
#include "settings/tool_settings.h"
#include "ui/screenshot_window.h"
#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>

using namespace jietuba;
using Elements = std::vector<MSAAElementSnapshot>;
namespace {
Elements elements(const QJsonArray &rows) {
    Elements result;
    for (const auto &row : rows) {
        const auto r = row.toArray();
        result.push_back({{r[0].toInt(), r[1].toInt(), r[2].toInt(), r[3].toInt()}, static_cast<int>(result.size())});
    }
    return result;
}
QJsonArray rects(const Elements &rows) {
    QJsonArray result;
    for (const auto &r : rows) result.append(QJsonArray{r.rect[0], r.rect[1], r.rect[2], r.rect[3]});
    return result;
}
QImage image() { QImage result(400, 300, QImage::Format_RGB32); result.fill(Qt::gray); return result; }
struct CursorRestore { QPoint point = QCursor::pos(); ~CursorRestore() { QCursor::setPos(point); } };
void setup_view(CanvasView &view, MSAAQuery query) {
    view.smart_selection_enabled = view.smart_selection_drills_to_elements = true;
    view.window_finder = std::make_unique<WindowFinder>();
    view.window_finder->windows = {{100, {0, 0, 300, 200}, "fixture"}};
    view.use_element_finder(std::make_unique<MSAAElementFinder>(&view, std::move(query)));
}
// A hover move in scene coordinates, through the view's own mouse handling. Late replies
// re-read the cursor, and scene coordinates are screen coordinates.
void hover(CanvasView &view, const QPointF &scene_pos) {
    QCursor::setPos(scene_pos.toPoint());
    const QPointF view_pos = view.mapFromScene(scene_pos);
    QMouseEvent event(QEvent::MouseMove, view_pos, QPointF(view.viewport()->mapToGlobal(view_pos.toPoint())), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(view.viewport(), &event);
}
QRectF selection(CanvasView &view) { return view.canvas_scene->selection_model->rect(); }
// A 40 px control centred on the queried x; the given x answers slowly.
MSAAQuery controls(int slow_x, int slow_ms) {
    return [slow_x, slow_ms](quintptr, int x, int, bool) {
        std::this_thread::sleep_for(std::chrono::milliseconds(x == slow_x ? slow_ms : 10));
        return Elements{{{x - 20, 10, x + 20, 60}, 0}};
    };
}
}

class MSAATests : public QObject {
    Q_OBJECT
private slots:
    void pointCacheMatchesFixture() {
        QFile file(JIETUBA_MSAA_FIXTURE);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto fixture = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(InitialSelectionGate::WAIT_MS, fixture["initial_wait_ms"].toInt());
        for (const auto &value : fixture["cases"].toArray()) {
            const auto row = value.toObject();
            const auto point = row["point"].toArray(), minimum = row["min"].toArray();
            const auto answer = elements(row["rects"].toArray());
            MSAAElementFinder finder(nullptr, [answer](quintptr, int, int, bool) { return answer; });
            QTRY_VERIFY_WITH_TIMEOUT(finder.prepare_at(100, point[0].toInt(), point[1].toInt()), 1000);
            QCOMPARE(rects(finder.elements_at(100, point[0].toInt(), point[1].toInt(), QSize(minimum[0].toInt(), minimum[1].toInt()))), row["expected"].toArray());
        }
    }
    void staleInflightResultCannotReplaceNewPoint() {
        MSAAElementFinder finder(nullptr, [](quintptr, int x, int, bool) {
            std::this_thread::sleep_for(std::chrono::milliseconds(x == 20 ? 35 : 1));
            return Elements{{{x - 5, 10, x + 5, 40}, 0}};
        });
        finder.elements_at(100, 20, 20);
        finder.elements_at(100, 80, 20);
        QTRY_VERIFY_WITH_TIMEOUT(finder.prepare_at(100, 80, 20), 1000);
        QTest::qWait(45);
        QJsonArray expected;
        expected.append(QJsonArray{75, 10, 85, 40});
        QCOMPARE(rects(finder.elements_at(100, 80, 20)), expected);
        QVERIFY(finder.elements_at(200, 80, 20).empty());
        finder.close();
    }
    void parentChainSurvivesSameLeafRefresh() {
        MSAAElementFinder finder(nullptr, [](quintptr, int, int, bool parents) {
            Elements result{{{10, 10, 40, 40}, 0}};
            if (parents) result.push_back({{0, 0, 100, 100}, 1});
            return result;
        });
        finder.request_parent_chain(100, 20, 20);
        QTRY_VERIFY_WITH_TIMEOUT(finder.has_parent_chain(100, 20, 20), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(finder.prepare_at(100, 21, 20), 1000);
        QCOMPARE(finder.elements_at(100, 21, 20).size(), size_t(2));
    }
    void replyForAnEarlierPointIsKeptWhileItsControlHoldsThePointer() {
        CursorRestore restore;
        CanvasScene scene(image(), QRectF(0, 0, 400, 300));
        CanvasView view(&scene);
        setup_view(view, controls(150, 400));
        hover(view, QPointF(100, 30));
        QTRY_COMPARE_WITH_TIMEOUT(selection(view), QRectF(80, 10, 40, 50), 1000);
        // 140 is still being answered when the pointer reaches 150; its control covers 150.
        hover(view, QPointF(140, 30));
        hover(view, QPointF(150, 30));
        QTRY_COMPARE_WITH_TIMEOUT(selection(view), QRectF(120, 10, 40, 50), 300);
        QTRY_COMPARE_WITH_TIMEOUT(selection(view), QRectF(130, 10, 40, 50), 1000);
    }
    void selectionHoldsWhileTheNextControlIsLookedUp() {
        CursorRestore restore;
        CanvasScene scene(image(), QRectF(0, 0, 400, 300));
        CanvasView view(&scene);
        setup_view(view, controls(200, 60));
        hover(view, QPointF(100, 30));
        QTRY_COMPARE_WITH_TIMEOUT(selection(view), QRectF(80, 10, 40, 50), 1000);
        // Outside the known control: the window is not flashed while 200 is answered.
        hover(view, QPointF(200, 30));
        QCOMPARE(selection(view), QRectF(80, 10, 40, 50));
        QTest::qWait(30);
        QCOMPARE(selection(view), QRectF(80, 10, 40, 50));
        QTRY_COMPARE_WITH_TIMEOUT(selection(view), QRectF(180, 10, 40, 50), 1000);
    }
    void slowLookupFallsBackToTheWindowAfterTheHold() {
        CursorRestore restore;
        CanvasScene scene(image(), QRectF(0, 0, 400, 300));
        CanvasView view(&scene);
        setup_view(view, controls(200, 600));
        hover(view, QPointF(100, 30));
        QTRY_COMPARE_WITH_TIMEOUT(selection(view), QRectF(80, 10, 40, 50), 1000);
        QElapsedTimer timer;
        timer.start();
        hover(view, QPointF(200, 30));
        QTRY_COMPARE_WITH_TIMEOUT(selection(view), QRectF(0, 0, 300, 200), 500);
        QVERIFY(timer.elapsed() >= MSAAElementFinder::HOLD_MS - 20);
        QTRY_COMPARE_WITH_TIMEOUT(selection(view), QRectF(180, 10, 40, 50), 1500);
    }
    void retargetingKeepsTheTweenDeadline() {
        std::vector<QRectF> applied;
        SmartSelectionAnimator animator([&](const QRectF &rect) { applied.push_back(rect); });
        const QRectF a(0, 0, 100, 100), b(200, 0, 100, 100), c(400, 0, 100, 100);
        animator.animate_to(a, b);
        QVERIFY(animator.is_running());
        // Blocking sleeps let time pass without ticks: only the clock decides.
        QTest::qSleep(SmartSelectionAnimator::DURATION_MS * 3 / 4);
        animator.animate_to(QRectF(150, 0, 100, 100), c);
        QVERIFY(animator.is_running());
        QCOMPARE(animator.target(), c);
        QTest::qSleep(SmartSelectionAnimator::DURATION_MS / 3);
        QTRY_VERIFY_WITH_TIMEOUT(!animator.is_running(), 200);
        QCOMPARE(applied.back(), c);
        // Idle again: a new change starts a full tween from the shown rect, the first frame near it.
        applied.clear();
        animator.animate_to(c, a);
        QTRY_VERIFY_WITH_TIMEOUT(!applied.empty(), 200);
        QVERIFY(applied.front().left() > a.left() && applied.front().left() < c.left());
        animator.snap_to(b);
        QVERIFY(!animator.is_running());
        QCOMPARE(applied.back(), b);
    }
    void closeDiscardsPendingReply() {
        MSAAElementFinder finder(nullptr, [](quintptr, int, int, bool) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return Elements{{{10, 10, 40, 40}, 0}};
        });
        QSignalSpy changed(&finder, &MSAAElementFinder::cache_updated);
        finder.elements_at(100, 20, 20);
        finder.close();
        QTest::qWait(45);
        QCOMPARE(changed.count(), 0);
        QVERIFY(finder.elements_at(100, 20, 20).empty());
    }
    void firstGateWaitsWithoutBlockingThenUsesResult() {
        CursorRestore restore;
        QCursor::setPos(20, 20);
        CanvasScene scene(image(), QRectF(0, 0, 400, 300));
        CanvasView view(&scene);
        setup_view(view, [](quintptr, int, int, bool) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return Elements{{{10, 10, 40, 40}, 0}};
        });
        InitialSelectionGate gate;
        QSignalSpy ready(&gate, &InitialSelectionGate::ready);
        gate.start(&view);
        QCOMPARE(ready.count(), 0);
        QVERIFY(gate.is_waiting());
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 1000);
        QCOMPARE(view.get_smart_selection_rect(QPointF(20, 20)), QRectF(10, 10, 30, 30));
        QVERIFY(!gate.is_waiting());
    }
    void firstGateTimesOutAndCancelInvalidatesItsTimers() {
        CursorRestore restore;
        QCursor::setPos(20, 20);
        CanvasScene scene(image(), QRectF(0, 0, 400, 300));
        CanvasView view(&scene);
        setup_view(view, [](quintptr, int, int, bool) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            return Elements{};
        });
        InitialSelectionGate gate;
        QSignalSpy ready(&gate, &InitialSelectionGate::ready);
        gate.start(&view);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 100);
        QCOMPARE(view.get_smart_selection_rect(QPointF(20, 20)), QRectF(0, 0, 300, 200));
        gate.start(&view);
        gate.cancel();
        QTest::qWait(60);
        QCOMPARE(ready.count(), 1);
    }
};
namespace {
int benchmark() {
    std::unique_ptr<MSAAElementFinder> finder = std::make_unique<MSAAElementFinder>();
    std::cout << "{\"ready\":\"cpp_qt\"}" << std::endl;
    std::string line;
    while (std::getline(std::cin, line)) {
        const auto command = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        if (command["stop"].toBool()) return 0;
        if (command["reset"].toBool()) finder = std::make_unique<MSAAElementFinder>();
        const auto hwnd = static_cast<quintptr>(command["hwnd"].toDouble());
        QJsonArray rows;
        for (const auto &p : command["points"].toArray()) {
            const auto point = p.toArray();
            const int x = point[0].toInt(), y = point[1].toInt();
            QEventLoop loop;
            QTimer timeout;
            timeout.setSingleShot(true);
            QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
            const auto connection = QObject::connect(finder.get(), &MSAAElementFinder::cache_updated, &loop, &QEventLoop::quit);
            QElapsedTimer timer;
            timer.start();
            finder->request_refresh_if_needed(hwnd, x, y);
            timeout.start(5000);
            loop.exec();
            const auto ms = timer.nsecsElapsed() / 1e6;
            QObject::disconnect(connection);
            const auto answer = finder->elements_at(hwnd, x, y);
            rows.append(QJsonObject{{"ms", ms}, {"backend_ms", finder->last_elapsed_ms()}, {"rects", rects(answer)}, {"error", finder->last_error()}});
        }
        std::cout << QJsonDocument(QJsonObject{{"rows", rows}}).toJson(QJsonDocument::Compact).constData() << std::endl;
    }
    return 0;
}
class FirstPaint : public QObject {
public:
    QElapsedTimer timer;
    QJsonArray samples;
    bool recorded = false;
    int round = 0;
    bool eventFilter(QObject *object, QEvent *event) override {
        if (event->type() == QEvent::Paint && QByteArray(object->metaObject()->className()).endsWith("MaskOverlayWidget") && !recorded) {
            recorded = true;
            auto *window = qobject_cast<ScreenshotWindow *>(object->parent());
            const auto r = window->scene->selection_model->rect();
            samples.append(QJsonObject{{"first_paint_ms", timer.nsecsElapsed() / 1e6}, {"round", round},
                                      {"rect", QJsonArray{r.left(), r.top(), r.left() + r.width(), r.top() + r.height()}},
                                      {"backend_ms", window->view->element_finder ? window->view->element_finder->last_elapsed_ms() : -1}});
        }
        return false;
    }
};
int first_frame(QApplication &app) {
    const auto args = app.arguments();
    CursorRestore restore;
    QCursor::setPos(args[2].toInt(), args[3].toInt());
    QTemporaryDir directory;
    QSettings settings(directory.filePath("settings.ini"), QSettings::IniFormat);
    auto *config = get_tool_settings_manager(&settings);
    config->set_smart_selection_mode("element");
    config->set_smart_selection_animation(true);
    config->set_app_setting("ocr_enabled", false);
    config->set_screenshot_save_enabled(false);
    const auto captured = CaptureService().capture_all_screens();
    FirstPaint observer;
    app.installEventFilter(&observer);
    app.setQuitOnLastWindowClosed(false);
    std::function<void()> start;
    start = [&] {
        ++observer.round;
        observer.recorded = false;
        observer.timer.start();
        auto *window = new ScreenshotWindow(config, captured.first, captured.second);
        QTimer::singleShot(300, &app, [&, window] {
            window->cleanup_and_close();
            window->deleteLater();
            if (observer.round < 3) QTimer::singleShot(80, &app, start);
            else app.quit();
        });
    };
    start();
    app.exec();
    app.removeEventFilter(&observer);
    std::cout << QJsonDocument(QJsonObject{{"samples", observer.samples}}).toJson(QJsonDocument::Compact).constData() << std::endl;
    return observer.samples.isEmpty() ? 1 : 0;
}
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (app.arguments().contains("--benchmark")) return benchmark();
    if (app.arguments().contains("--first-frame")) return first_frame(app);
    MSAATests test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_msaa_element_finder.moc"
