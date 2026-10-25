#include "canvas/scene.h"
#include "canvas/selection_model.h"
#include "canvas/view.h"
#include "settings/tool_settings.h"
#include "ui/screenshot_window.h"
#include <QApplication>
#include <QCursor>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

using namespace jietuba;

// Smart selection over real windows: the capture snaps to the control, the window or the
// screen under the pointer, and the wheel walks between those levels. The screen is 100%
// scaled in this setup, so scene, Qt and physical coordinates agree.
class SmartSelectionTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir dir_;
    QSettings *settings_ = nullptr;
    ToolSettingsManager *config_ = nullptr;
    std::unique_ptr<QWidget> probe_;
    QPushButton *first_ = nullptr;
    QPushButton *second_ = nullptr;
    QPointer<ScreenshotWindow> window_;

    QRect screen() const { return QGuiApplication::primaryScreen()->geometry(); }

    ScreenshotWindow *open(const QString &mode) {
        config_->set_smart_selection(true);
        config_->set_smart_selection_mode(mode);
        QImage image(screen().size(), QImage::Format_RGB32);
        image.fill(Qt::gray);
        window_ = new ScreenshotWindow(config_, image, QRectF(screen()));
        // The window shows once its first selection is known, at most the gate's wait later; wheel
        // events need it on screen.
        QTest::qWaitFor([this] { return window_ && window_->isVisible(); }, 3000);
        return window_;
    }

    void hover(const QPoint &global) {
        QCursor::setPos(global);
        auto *viewport = window_->view->viewport();
        const auto local = viewport->mapFromGlobal(global);
        QMouseEvent move(QEvent::MouseMove, QPointF(local), QPointF(global), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &move);
    }

    void wheel(const QPoint &global, int delta) {
        QTest::wheelEvent(window_->windowHandle(), QPointF(window_->mapFromGlobal(global)), QPoint(0, delta));
    }

    QRect selection() const { return window_->scene->selection_model->rect().toRect(); }

    static QRect global_rect(QWidget *widget) { return QRect(widget->mapToGlobal(QPoint(0, 0)), widget->size()); }

private slots:
    void initTestCase() {
        if (QGuiApplication::platformName() != "windows") QSKIP("needs -platform windows: smart selection reads real windows");
        if (!qFuzzyCompare(QGuiApplication::primaryScreen()->devicePixelRatio(), 1.0)) QSKIP("expects a 100% scaled primary screen");
        QVERIFY(dir_.isValid());
        // It binds once per process, so a repeated run (check.ps1 -Mode Leak) keeps the first file.
        if (!settings_) settings_ = new QSettings(dir_.filePath("settings.ini"), QSettings::IniFormat, this);
        config_ = get_tool_settings_manager(settings_);
        config_->set_screenshot_save_enabled(false);
        config_->set_app_setting("ocr_enabled", false);
        config_->set_smart_selection_animation(false);
        probe_ = std::make_unique<QWidget>(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        probe_->setWindowTitle("jietuba smart selection probe");
        auto *layout = new QHBoxLayout(probe_.get());
        layout->setContentsMargins(40, 40, 40, 40);
        layout->setSpacing(30);
        first_ = new QPushButton("First", probe_.get());
        second_ = new QPushButton("Second", probe_.get());
        for (auto *button : {first_, second_}) {
            button->setMinimumSize(120, 60);
            layout->addWidget(button);
        }
        const auto available = QGuiApplication::primaryScreen()->availableGeometry();
        probe_->setGeometry(QRect(available.center() - QPoint(200, 100), QSize(400, 200)));
        probe_->show();
        QVERIFY(QTest::qWaitForWindowExposed(probe_.get()));
    }

    void cleanup() {
        if (window_ && window_->session_active()) window_->cleanup_and_close();
        QApplication::processEvents();
    }

    void cleanupTestCase() { probe_.reset(); }

    // Window mode snaps to the window under the pointer, and to the screen elsewhere.
    void windowModeSnapsToTheWindowUnderThePointer() {
        auto *window = open("window");
        QVERIFY(window->view->smart_selection_enabled);
        hover(global_rect(first_).center());
        QCOMPARE(selection(), probe_->geometry());
        // Outside every listed window the screen is the selection.
        hover(screen().topLeft() + QPoint(3, 3));
        QVERIFY(screen().contains(selection()));
        // A click confirms the hovered window.
        hover(global_rect(second_).center());
        const auto local = window->view->viewport()->mapFromGlobal(global_rect(second_).center());
        QTest::mouseClick(window->view->viewport(), Qt::LeftButton, Qt::NoModifier, local);
        QTRY_VERIFY(window->scene->selection_model->is_confirmed());
        QCOMPARE(selection(), probe_->geometry());
    }

    // Element mode snaps to the control under the pointer once its query returns; the wheel walks outward and back.
    void elementModeWalksFromControlToWindowToScreen() {
        auto *window = open("element");
        // The probe lives in this process, so the finder's responsiveness check waits for this
        // thread. A Debug overlay takes longer to build than that check allows and the window
        // would count as unresponsive for the whole capture; restart detection once the overlay is up.
        window->view->enable_smart_selection("element");
        QVERIFY(window->view->smart_selection_drills_to_elements);
        const auto button = global_rect(first_);
        hover(button.center());
        // The window stands in until the point query returns.
        QTRY_COMPARE_WITH_TIMEOUT(selection(), button, 10000);
        wheel(button.center(), 120);
        QTRY_COMPARE_WITH_TIMEOUT(selection(), probe_->geometry(), 5000);
        wheel(button.center(), 120);
        QVERIFY(selection().contains(probe_->geometry()));
        // Beyond the outermost level it stays there.
        const auto outermost = selection();
        wheel(button.center(), 120);
        QCOMPARE(selection(), outermost);
        wheel(button.center(), -120);
        QCOMPARE(selection(), probe_->geometry());
        // Moving within the same control keeps the level; another control resets it.
        hover(button.center() + QPoint(5, 3));
        QCOMPARE(selection(), probe_->geometry());
        QTRY_COMPARE_WITH_TIMEOUT((hover(global_rect(second_).center()), selection()), global_rect(second_), 5000);
        // Once confirmed the wheel no longer changes the level.
        const auto local = window->view->viewport()->mapFromGlobal(global_rect(second_).center());
        QTest::mouseClick(window->view->viewport(), Qt::LeftButton, Qt::NoModifier, local);
        QTRY_VERIFY(window->scene->selection_model->is_confirmed());
        QCOMPARE(selection(), global_rect(second_));
        wheel(global_rect(second_).center(), 120);
        QCOMPARE(selection(), global_rect(second_));
    }

    // Switching the mode off on a running view drops the finders.
    void turningSmartSelectionOffStopsSnapping() {
        auto *window = open("element");
        window->view->enable_smart_selection("off");
        QVERIFY(!window->view->smart_selection_enabled);
        const auto before = selection();
        hover(global_rect(first_).center());
        QCOMPARE(selection(), before);
        window->view->enable_smart_selection("nonsense");
        QVERIFY(!window->view->smart_selection_enabled);
    }
};

QTEST_MAIN(SmartSelectionTests)
#include "test_smart_selection.moc"
