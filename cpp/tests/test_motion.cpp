#include "ui/fluent_lite/fluent_lite.h"
#include "ui/fluent_lite/fold_box.h"
#include "ui/motion.h"
#include "ui/reorderable_rows.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QMenu>
#include <QTest>
#include <QVBoxLayout>
#include <optional>
#include "fixed_animation_steps.h"

using namespace jietuba;

namespace {
constexpr int ROW = 40;

// Rows of a fixed height in the box, so its full height is known.
void add_rows(FoldBox *box, int count) {
    auto *layout = qobject_cast<QVBoxLayout *>(box->content()->layout());
    if (!layout) {
        layout = new QVBoxLayout(box->content());
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
    }
    for (int i = 0; i < count; ++i) {
        auto *row = new QLabel("row", box->content());
        row->setFixedHeight(ROW);
        layout->addWidget(row);
    }
}

// A shown window with the box at the top of it.
struct Stage {
    QWidget host;
    FoldBox *box;
    // The height the box asked for and its cover, each time that height changed.
    QList<QPair<int, qreal>> steps;
    explicit Stage(int rows) : box(new FoldBox(&host)) {
        auto *layout = new QVBoxLayout(&host);
        layout->setContentsMargins(0, 0, 0, 0);
        add_rows(box, rows);
        layout->addWidget(box);
        layout->addStretch(1);
        host.resize(300, 400);
        host.show();
        QObject::connect(box, &FoldBox::resized, box, [this] { steps.append({box->maximumHeight(), box->cover()}); });
    }
};
}

class MotionTest : public QObject {
    Q_OBJECT
    // The tests read the steps of animations.
    std::optional<FixedAnimationSteps> steps_;
private slots:
    void init() {
        motion::set_enabled_override(true);
        steps_.emplace();
    }
    void cleanup() {
        steps_.reset();
        motion::set_enabled_override(std::nullopt);
    }

    void durationsFollowTheAnimationSwitch() {
        QCOMPARE(motion::duration(motion::FAST_MS), 83);
        QCOMPARE(motion::duration(motion::MID_MS), 167);
        QCOMPARE(motion::duration(motion::SLOW_MS), 250);
        motion::set_enabled_override(false);
        QVERIFY(!motion::enabled());
        QCOMPARE(motion::duration(motion::SLOW_MS), 0);
    }

    void theCurveStartsFastAndSettlesSlowly() {
        const QEasingCurve curve = motion::decelerate();
        QCOMPARE(curve.valueForProgress(0.0), 0.0);
        QCOMPARE(curve.valueForProgress(1.0), 1.0);
        // cubic-bezier(0, 0, 0, 1): at a tenth of the time, x = s^3 gives s = 0.4642 and y = 3(1-s)s^2 + s^3 = 0.4463.
        QVERIFY(std::abs(curve.valueForProgress(0.1) - 0.4463) < 0.01);
        QVERIFY(curve.valueForProgress(0.5) > 0.85);
    }

    void installingTurnsQtsOwnPopupEffectsOff() {
        QApplication::setEffectEnabled(Qt::UI_AnimateCombo, true);
        QApplication::setEffectEnabled(Qt::UI_AnimateMenu, true);
        QApplication::setEffectEnabled(Qt::UI_FadeMenu, true);
        motion::install_popup_fade(*qApp);
        QVERIFY(!QApplication::isEffectEnabled(Qt::UI_AnimateCombo));
        QVERIFY(!QApplication::isEffectEnabled(Qt::UI_AnimateMenu));
        QVERIFY(!QApplication::isEffectEnabled(Qt::UI_FadeMenu));
    }

    void aMenuIsTransparentUntilPaintedThenFadesIn() {
        motion::install_popup_fade(*qApp);
        QMenu menu;
        menu.addAction("Copy");
        menu.popup(QPoint(40, 40));
        QVERIFY(menu.isVisible());
        QCOMPARE(menu.windowOpacity(), 0.0);
        QTRY_COMPARE_WITH_TIMEOUT(menu.windowOpacity(), 1.0, 1000);
        // Shown again, it fades again.
        menu.hide();
        menu.popup(QPoint(40, 40));
        QCOMPARE(menu.windowOpacity(), 0.0);
        QTRY_COMPARE_WITH_TIMEOUT(menu.windowOpacity(), 1.0, 1000);
    }

    void aComboBoxListFadesInTheSameWay() {
        motion::install_popup_fade(*qApp);
        QWidget host;
        QComboBox combo(&host);
        combo.addItems({"Always show", "In more menu", "Always hide"});
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        combo.showPopup();
        QWidget *list = combo.view()->window();
        QVERIFY(list->isVisible());
        QCOMPARE(list->windowOpacity(), 0.0);
        QTRY_COMPARE_WITH_TIMEOUT(list->windowOpacity(), 1.0, 1000);
        combo.hidePopup();
    }

    void aPopupHiddenWhileFadingStartsOverWhenShownAgain() {
        motion::install_popup_fade(*qApp);
        QMenu menu;
        menu.addAction("Copy");
        menu.popup(QPoint(40, 40));
        QTRY_VERIFY_WITH_TIMEOUT(menu.windowOpacity() > 0.0, 1000);
        menu.hide();
        menu.popup(QPoint(40, 40));
        QCOMPARE(menu.windowOpacity(), 0.0);
        QTRY_COMPARE_WITH_TIMEOUT(menu.windowOpacity(), 1.0, 1000);
    }

    void withAnimationsOffAPopupAppearsAtOnce() {
        motion::install_popup_fade(*qApp);
        motion::set_enabled_override(false);
        QMenu menu;
        menu.addAction("Copy");
        menu.popup(QPoint(40, 40));
        QCOMPARE(menu.windowOpacity(), 1.0);
        QTest::qWait(50);
        QCOMPARE(menu.windowOpacity(), 1.0);
    }

    void otherWindowsAreLeftAlone() {
        motion::install_popup_fade(*qApp);
        QWidget popup(nullptr, Qt::Popup);
        popup.resize(80, 40);
        popup.show();
        QCOMPARE(popup.windowOpacity(), 1.0);
        QTest::qWait(50);
        QCOMPARE(popup.windowOpacity(), 1.0);
    }

    void aFoldBoxOffScreenOpensAndClosesAtOnce() {
        FoldBox box;
        add_rows(&box, 2);
        QVERIFY(box.isHidden() && !box.isOpen());
        box.setOpen(true);
        QVERIFY(!box.isHidden() && box.settled());
        QCOMPARE(box.height(), 2 * ROW);
        QCOMPARE(box.cover(), 0.0);
        box.setOpen(false);
        QVERIFY(box.isHidden() && box.settled());
    }

    // Opening: the box grows with its content covered and uncovers it once it is full height.
    void aFoldBoxOnScreenGrowsCoveredThenUncovers() {
        Stage stage(3);
        QVERIFY(QTest::qWaitForWindowExposed(&stage.host));
        stage.box->setOpen(true);
        QVERIFY(!stage.box->isHidden() && !stage.box->settled());
        QCOMPARE(stage.box->cover(), 1.0);
        QCOMPARE(stage.box->content()->height(), 3 * ROW);
        QTRY_VERIFY_WITH_TIMEOUT(stage.box->settled(), 2000);
        QCOMPARE(stage.box->height(), 3 * ROW);
        QCOMPARE(stage.box->cover(), 0.0);
        QVERIFY(stage.steps.size() > 3);
        for (int i = 0; i < stage.steps.size(); ++i) {
            if (stage.steps[i].first < 3 * ROW) QCOMPARE(stage.steps[i].second, 1.0);
            if (i > 0) QVERIFY(stage.steps[i].first >= stage.steps[i - 1].first);
        }
        // The content never moved or changed size.
        QCOMPARE(stage.box->content()->geometry(), QRect(0, 0, 300, 3 * ROW));
    }

    // Closing: the content is covered first, and only then does the box shrink and go.
    void aFoldBoxOnScreenCoversThenShrinks() {
        Stage stage(3);
        QVERIFY(QTest::qWaitForWindowExposed(&stage.host));
        stage.box->setOpen(true);
        QTRY_VERIFY_WITH_TIMEOUT(stage.box->settled(), 2000);
        stage.steps.clear();
        stage.box->setOpen(false);
        QVERIFY(!stage.box->isHidden());
        QCOMPARE(stage.box->height(), 3 * ROW);
        QTRY_VERIFY_WITH_TIMEOUT(stage.box->settled(), 2000);
        QVERIFY(stage.box->isHidden());
        QVERIFY(stage.steps.size() > 3);
        // The last report is of the box going hidden, uncovered again for its next opening.
        QStringList trail;
        for (const auto &step : stage.steps) trail << QString("%1@%2").arg(step.first).arg(step.second);
        const QByteArray shown = trail.join(' ').toUtf8();
        for (int i = 0; i + 1 < stage.steps.size(); ++i) {
            QVERIFY2(stage.steps[i].second == 1.0, shown.constData());
            if (i > 0) QVERIFY2(stage.steps[i].first <= stage.steps[i - 1].first, shown.constData());
        }
    }

    void aFoldBoxAskedNotToAnimateOpensAtOnce() {
        Stage stage(3);
        QVERIFY(QTest::qWaitForWindowExposed(&stage.host));
        stage.box->setOpen(true, false);
        QVERIFY(stage.box->settled() && !stage.box->isHidden());
        QCOMPARE(stage.box->maximumHeight(), 3 * ROW);
        QCOMPARE(stage.box->cover(), 0.0);
    }

    void aFoldBoxTurnedBackMidwayEndsWhereItWasSent() {
        Stage stage(3);
        QVERIFY(QTest::qWaitForWindowExposed(&stage.host));
        stage.box->setOpen(true);
        QTest::qWait(40);
        QVERIFY(!stage.box->settled());
        stage.box->setOpen(false);
        QTRY_VERIFY_WITH_TIMEOUT(stage.box->settled(), 2000);
        QVERIFY(stage.box->isHidden());
        // And reopened while it closes.
        stage.box->setOpen(true);
        QTRY_VERIFY_WITH_TIMEOUT(stage.box->settled(), 2000);
        stage.box->setOpen(false);
        QTest::qWait(20);
        stage.box->setOpen(true);
        QTRY_VERIFY_WITH_TIMEOUT(stage.box->settled(), 2000);
        QVERIFY(!stage.box->isHidden());
        QCOMPARE(stage.box->height(), 3 * ROW);
        QCOMPARE(stage.box->cover(), 0.0);
    }

    void anOpenFoldBoxFollowsItsContent() {
        Stage stage(2);
        QVERIFY(QTest::qWaitForWindowExposed(&stage.host));
        stage.box->setOpen(true);
        QTRY_VERIFY_WITH_TIMEOUT(stage.box->settled(), 2000);
        QCOMPARE(stage.box->height(), 2 * ROW);
        add_rows(stage.box, 1);
        QTRY_COMPARE(stage.box->height(), 3 * ROW);
        stage.box->content()->layout()->itemAt(0)->widget()->hide();
        QTRY_COMPARE(stage.box->height(), 2 * ROW);
    }

    void withAnimationsOffAFoldBoxMovesAtOnce() {
        motion::set_enabled_override(false);
        Stage stage(3);
        QVERIFY(QTest::qWaitForWindowExposed(&stage.host));
        stage.box->setOpen(true);
        QVERIFY(stage.box->settled() && !stage.box->isHidden());
        QCOMPARE(stage.box->height(), 3 * ROW);
        stage.box->setOpen(false);
        QVERIFY(stage.box->settled() && stage.box->isHidden());
    }

    void aCheckBoxOnScreenFadesToItsNewState() {
        QWidget host;
        auto *box = new CheckBox("Keep", &host);
        // Not on screen yet: it takes its state at once.
        box->setChecked(true);
        QCOMPARE(box->fill(), 1.0);
        box->setChecked(false);
        QCOMPARE(box->fill(), 0.0);
        host.resize(200, 60);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        box->setChecked(true);
        QVERIFY(box->fill() < 1.0);
        QTRY_COMPARE_WITH_TIMEOUT(box->fill(), 1.0, 1000);
        box->setChecked(false);
        QVERIFY(box->fill() > 0.0);
        QTRY_COMPARE_WITH_TIMEOUT(box->fill(), 0.0, 1000);
    }

    // The backing of the checked button travels from the old choice to the new one.
    void aSegmentedSelectorSlidesItsBackingToTheNewChoice() {
        SegmentedWidget selector;
        for (const char *key : {"show", "more", "hide"}) selector.addItem(key, key);
        selector.setCurrentItem("show");
        selector.show();
        QVERIFY(QTest::qWaitForWindowExposed(&selector));
        QList<QPushButton *> buttons = selector.findChildren<QPushButton *>();
        QCOMPARE(buttons.size(), 3);
        std::sort(buttons.begin(), buttons.end(), [](QPushButton *a, QPushButton *b) { return a->x() < b->x(); });
        QCOMPARE(selector.thumbRect(), QRectF(buttons[0]->geometry()));
        selector.setCurrentItem("hide");
        QVERIFY(buttons[2]->isChecked());
        QCOMPARE(selector.thumbRect(), QRectF(buttons[0]->geometry()));
        QList<qreal> lefts;
        QElapsedTimer clock;
        clock.start();
        while (selector.thumbRect() != QRectF(buttons[2]->geometry()) && clock.elapsed() < 1000) {
            lefts << selector.thumbRect().left();
            QTest::qWait(8);
        }
        QCOMPARE(selector.thumbRect(), QRectF(buttons[2]->geometry()));
        QVERIFY(lefts.size() > 3);
        for (int i = 1; i < lefts.size(); ++i) QVERIFY(lefts[i] >= lefts[i - 1]);
        // With animations off it is there at once.
        motion::set_enabled_override(false);
        selector.setCurrentItem("show");
        QCOMPARE(selector.thumbRect(), QRectF(buttons[0]->geometry()));
    }

    // Rows change order at once, and slide to their new places.
    void reorderedRowsSlideToTheirPlaces() {
        ReorderableRowList list;
        QList<DraggableRow *> rows;
        for (int i = 0; i < 3; ++i) {
            auto *row = new DraggableRow;
            row->setFixedHeight(40);
            rows << list.add_row(row);
        }
        list.resize(300, 200);
        list.show();
        QVERIFY(QTest::qWaitForWindowExposed(&list));
        const QPoint first_place = rows[0]->pos(), second_place = rows[1]->pos();
        QVERIFY(second_place.y() > first_place.y());
        // The first row dragged to below the centre of the second.
        emit rows[0]->grip->dragged(rows[1]->mapToGlobal(QPoint(10, 30)).y());
        QCOMPARE(list.rows(), (QList<DraggableRow *>{rows[1], rows[0], rows[2]}));
        QCOMPARE(rows[1]->pos(), second_place);
        QCOMPARE(rows[0]->pos(), first_place);
        QTRY_COMPARE_WITH_TIMEOUT(rows[1]->pos(), first_place, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(rows[0]->pos(), second_place, 1000);
        // With animations off they are there at once.
        motion::set_enabled_override(false);
        emit rows[0]->grip->dragged(rows[1]->mapToGlobal(QPoint(10, -30)).y());
        QCOMPARE(list.rows(), (QList<DraggableRow *>{rows[0], rows[1], rows[2]}));
        QCOMPARE(rows[0]->pos(), first_place);
        QCOMPARE(rows[1]->pos(), second_place);
    }

    // The bar of the current page travels down the list to the page chosen next.
    void theNavigationBarTravelsToTheNewPage() {
        NavigationInterface navigation;
        for (const char *key : {"general", "capture", "pin", "translation"}) navigation.addItem(key, IconSource{}, key, {});
        navigation.addItem("about", IconSource{}, "about", {}, NavigationItemPosition::BOTTOM);
        navigation.resize(220, 400);
        navigation.setCurrentItem("general");
        navigation.show();
        QVERIFY(QTest::qWaitForWindowExposed(&navigation));
        const auto bar_at = [&](const char *key) {
            const QPushButton *row = navigation.widget(key);
            const QRectF bar = navigation.indicatorRect();
            return !bar.isEmpty() && std::abs(bar.center().y() - QRectF(row->geometry()).center().y()) < 1.0 && std::abs(bar.left() - row->x()) < 1.0;
        };
        QTRY_VERIFY(bar_at("general"));
        navigation.setCurrentItem("translation");
        QList<qreal> tops;
        QElapsedTimer clock;
        clock.start();
        while (!bar_at("translation") && clock.elapsed() < 1000) {
            tops << navigation.indicatorRect().top();
            QTest::qWait(8);
        }
        QVERIFY(bar_at("translation"));
        QVERIFY(tops.size() > 3);
        for (int i = 1; i < tops.size(); ++i) QVERIFY(tops[i] >= tops[i - 1]);
        // A page of the lower list draws its own bar.
        navigation.setCurrentItem("about");
        QTRY_VERIFY(navigation.indicatorRect().isEmpty());
        // And back: there was no bar to travel from, so it is there at once.
        navigation.setCurrentItem("capture");
        QTest::qWait(1);
        QVERIFY(bar_at("capture"));
    }

    // Switching to a page whose groups are read later: the bar does not stop at the page's own row on
    // the way, it goes from the group it was on to the new group.
    void theNavigationBarSkipsThePageRowWhileItsGroupsArePending() {
        NavigationInterface navigation;
        for (const char *key : {"general", "capture", "pin"}) navigation.addItem(key, IconSource{}, key, {});
        navigation.resize(220, 500);
        navigation.setCurrentItem("general");
        navigation.setSubItems("general", {"one", "two"});
        navigation.setExpanded("general", true, false);
        navigation.setCurrentSubItem("general", 0);
        navigation.show();
        QVERIFY(QTest::qWaitForWindowExposed(&navigation));
        const auto bar_on = [&](const QWidget *row) {
            const QRectF bar = navigation.indicatorRect();
            const QRectF place(row->mapTo(navigation.scrollArea()->widget(), QPoint(0, 0)), QSizeF(row->size()));
            return !bar.isEmpty() && std::abs(bar.center().y() - place.center().y()) < 1.0;
        };
        QTRY_VERIFY(bar_on(navigation.subItem("general", 0)));
        const qreal start = navigation.indicatorRect().top();

        navigation.setCurrentItem("capture");
        navigation.setSubItemPending("capture");
        navigation.setExpanded("general", false, false);
        navigation.setExpanded("capture", true, false);
        navigation.setCurrentSubItem({}, -1);
        for (int i = 0; i < 10; ++i) {
            QTest::qWait(8);
            QVERIFY(!bar_on(navigation.widget("capture")));
        }

        navigation.setSubItems("capture", {"one", "two"});
        navigation.setCurrentSubItem("capture", 1);
        navigation.setSubItemPending({});
        const QWidget *target = navigation.subItem("capture", 1);
        QList<qreal> tops{start};
        QElapsedTimer clock;
        clock.start();
        while (!bar_on(target) && clock.elapsed() < 1000) {
            if (!navigation.indicatorRect().isEmpty()) tops << navigation.indicatorRect().top();
            QTest::qWait(8);
        }
        QVERIFY(bar_on(target));
        QVERIFY(tops.size() > 3);
        for (int i = 1; i < tops.size(); ++i) QVERIFY(tops[i] >= tops[i - 1]);
    }

    // A button that draws itself fades to its hover colours and back.
    void aPaintedButtonFadesToItsHoverColours() {
        QWidget host;
        auto *button = new PushButton("Cancel", &host);
        button->move(20, 20);
        host.resize(200, 80);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        if (!button->paints_itself()) QSKIP("the button is drawn by a style sheet here");
        const QStyle::State rest = QStyle::State_Enabled, hover = rest | QStyle::State_MouseOver;
        QVERIFY(button->control_box(rest).fill != button->control_box(hover).fill);
        QCOMPARE(button->shown_box(rest).fill, button->control_box(rest).fill);
        QTest::mouseMove(button, QPoint(10, 10));
        QTRY_VERIFY(button->underMouse());
        // Part-way at first, the hover colour once the fade is over.
        QVERIFY(button->shown_box(hover).fill != button->control_box(hover).fill);
        QTRY_COMPARE_WITH_TIMEOUT(button->shown_box(hover).fill, button->control_box(hover).fill, 1000);
        // With animations off the colour changes at once.
        motion::set_enabled_override(false);
        QTest::mouseMove(&host, QPoint(190, 70));
        QTRY_VERIFY(!button->underMouse());
        QCOMPARE(button->shown_box(rest).fill, button->control_box(rest).fill);
    }
};

QTEST_MAIN(MotionTest)
#include "test_motion.moc"
