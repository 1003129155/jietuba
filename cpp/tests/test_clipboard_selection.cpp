#include "clipboard_test_support.h"
#include "clipboard/controllers/selection_manager.h"
#include <QLineEdit>
#include <QListWidget>
#include <QSignalSpy>
#include <QTest>
#include <memory>

using namespace clipboard_test;

namespace {
// A list of five rows with ids 10..14 and its selection manager.
struct Rows {
    QListWidget widget;
    std::unique_ptr<SelectionManager> selection;
    explicit Rows(int count = 5) {
        for (int i = 0; i < count; ++i) {
            auto *row = new QListWidgetItem(QString::number(10 + i));
            row->setData(Qt::UserRole, 10 + i);
            widget.addItem(row);
        }
        selection = std::make_unique<SelectionManager>(&widget, [](qint64) { return std::optional<ClipboardItem>(); });
    }
    std::optional<qint64> current() const { return selection->get_current_item_id(); }
    void key(int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) { QTest::keyClick(&widget, static_cast<Qt::Key>(key), modifiers); }
};
}

// Keyboard and mouse selection in the clipboard list: where the first arrow key lands,
// clamping, activation, the layout-dependent left/right keys and the multiple selection.
class ClipboardSelectionTests : public QObject {
    Q_OBJECT
    Environment env_;

private slots:
    void initTestCase() { QVERIFY(env_.init(this)); }

    void theFirstArrowKeyStartsFromAnEnd() {
        Rows rows;
        QVERIFY(!rows.selection->has_selection());
        rows.key(Qt::Key_Down);
        QCOMPARE(rows.current(), std::optional<qint64>(10));
        Rows up;
        up.key(Qt::Key_Up);
        QCOMPARE(up.current(), std::optional<qint64>(14));
    }

    void theFirstArrowKeyStartsFromTheHoveredRow() {
        Rows rows;
        rows.selection->set_hovered_index(2);
        rows.selection->move_selection(1);
        QCOMPARE(rows.selection->selected_index(), 3);
        Rows up;
        up.selection->set_hovered_index(2);
        up.selection->move_selection(-1);
        QCOMPARE(up.selection->selected_index(), 1);
        // Once navigating, the mouse passing over rows does not move the selection.
        up.selection->set_hovered_index(4);
        up.selection->move_selection(1);
        QCOMPARE(up.selection->selected_index(), 2);
        up.selection->clear_hovered_index();
    }

    void navigationClampsWithoutWrapping() {
        Rows rows;
        for (int expected : {0, 1, 2, 3, 4, 4, 4}) {
            rows.selection->move_selection(1);
            QCOMPARE(rows.selection->selected_index(), expected);
        }
        for (int i = 0; i < 10; ++i) rows.selection->move_selection(-1);
        QCOMPARE(rows.selection->selected_index(), 0);
        rows.selection->move_selection(1);
        rows.selection->move_selection(-1);
        QCOMPARE(rows.selection->selected_index(), 0);
        Rows empty(0);
        empty.selection->move_selection(1);
        empty.selection->move_selection(-1);
        QVERIFY(!empty.selection->has_selection());
        empty.selection->select_first();
        empty.selection->select_last();
        QVERIFY(!empty.selection->has_selection());
    }

    void selectingByIdFirstAndLast() {
        Rows rows;
        QSignalSpy changed(rows.selection.get(), &SelectionManager::selection_changed);
        rows.selection->select_first();
        QCOMPARE(rows.current(), std::optional<qint64>(10));
        rows.selection->select_last();
        QCOMPARE(rows.current(), std::optional<qint64>(14));
        QVERIFY(rows.selection->select_item_id(12));
        QCOMPARE(rows.selection->selected_index(), 2);
        // Unknown and missing ids leave the selection.
        QVERIFY(!rows.selection->select_item_id(999));
        QVERIFY(!rows.selection->select_item_id(std::nullopt));
        QCOMPARE(rows.selection->selected_index(), 2);
        QCOMPARE(changed.size(), 3);
        QCOMPARE(changed.last().first().value<std::optional<qint64>>(), std::optional<qint64>(12));
        // Clicking a row in the list keeps the manager's index in step.
        rows.widget.setCurrentRow(4);
        QCOMPARE(rows.selection->selected_index(), 4);
    }

    void clearingAndResetting() {
        Rows rows;
        rows.selection->move_selection(1);
        rows.selection->clear_selection();
        QVERIFY(!rows.selection->has_selection());
        QVERIFY(!rows.current());
        // Navigation stays started: the next key continues instead of starting over.
        rows.selection->move_selection(-1);
        QCOMPARE(rows.selection->selected_index(), 0);
        rows.selection->clear_selection(true);
        rows.selection->move_selection(-1);
        QCOMPARE(rows.selection->selected_index(), 4);
        // After reset the window behaves like a fresh one.
        rows.selection->set_hovered_index(1);
        rows.selection->reset();
        rows.selection->move_selection(-1);
        QCOMPARE(rows.selection->selected_index(), 4);
    }

    void enterActivatesTheCurrentRow() {
        Rows rows;
        QSignalSpy activated(rows.selection.get(), &SelectionManager::item_activated);
        rows.key(Qt::Key_Return);
        QCOMPARE(activated.size(), 0);
        rows.key(Qt::Key_Down);
        rows.key(Qt::Key_Return);
        rows.key(Qt::Key_Enter);
        QCOMPARE(activated.size(), 2);
        QCOMPARE(activated.first().first().toLongLong(), qint64(10));
        // Clicking a row activates it directly.
        emit rows.widget.itemClicked(rows.widget.item(3));
        QCOMPARE(activated.last().first().toLongLong(), qint64(13));
        // Other keys pass through to the list.
        rows.key(Qt::Key_A);
        QCOMPARE(rows.current(), std::optional<qint64>(10));
    }

    void leftAndRightFollowTheGroupBarLayout() {
        Rows rows;
        QSignalSpy switched(rows.selection.get(), &SelectionManager::request_group_switch);
        QSignalSpy sidebar(rows.selection.get(), &SelectionManager::request_sidebar_focus);
        rows.selection->group_bar_position = "top";
        rows.key(Qt::Key_Right);
        rows.key(Qt::Key_Left);
        QCOMPARE(switched.size(), 2);
        QCOMPARE(switched.at(0).first().toInt(), 1);
        QCOMPARE(switched.at(1).first().toInt(), -1);
        rows.selection->group_bar_position = "right";
        rows.key(Qt::Key_Right);
        rows.key(Qt::Key_Left);
        rows.selection->group_bar_position = "left";
        rows.key(Qt::Key_Left);
        rows.key(Qt::Key_Right);
        QCOMPARE(sidebar.size(), 2);
        QCOMPARE(switched.size(), 2);
    }

    void escapeClearsTheSelectionFirst() {
        Rows rows;
        rows.selection->select_item_id(11);
        rows.selection->toggle_mark(11);
        rows.key(Qt::Key_Escape);
        QVERIFY(!rows.selection->has_selection());
        QVERIFY(!rows.selection->has_marks());
        // Nothing selected: Escape is left to the window.
        rows.key(Qt::Key_Escape);
        QVERIFY(!rows.selection->has_selection());
    }

    void theSearchBoxDrivesTheList() {
        Rows rows;
        QLineEdit search;
        rows.selection->set_search_input(&search);
        QSignalSpy activated(rows.selection.get(), &SelectionManager::item_activated);
        QSignalSpy switched(rows.selection.get(), &SelectionManager::request_group_switch);
        QTest::keyClick(&search, Qt::Key_Down);
        QTest::keyClick(&search, Qt::Key_Down);
        QCOMPARE(rows.current(), std::optional<qint64>(11));
        QTest::keyClick(&search, Qt::Key_Up);
        QCOMPARE(rows.current(), std::optional<qint64>(10));
        QTest::keyClick(&search, Qt::Key_Return);
        QCOMPARE(activated.size(), 1);
        QTest::keyClick(&search, Qt::Key_Down, Qt::ShiftModifier);
        QCOMPARE(rows.selection->marked_ids(), (QList<qint64>{10, 11}));
        // Left and right edit the text unless the group bar is on top.
        QTest::keyClick(&search, Qt::Key_Right);
        QCOMPARE(switched.size(), 0);
        rows.selection->group_bar_position = "top";
        QTest::keyClick(&search, Qt::Key_Right);
        QTest::keyClick(&search, Qt::Key_Left);
        QCOMPARE(switched.size(), 2);
        // Typing reaches the box.
        QTest::keyClicks(&search, "abc");
        QCOMPARE(search.text(), QString("abc"));
    }

    void marksKeepTheirSelectionOrder() {
        Rows rows;
        QSignalSpy changes(rows.selection.get(), &SelectionManager::marks_changed);
        rows.selection->toggle_mark(12);
        rows.selection->toggle_mark(10);
        QCOMPARE(rows.selection->marked_ids(true), (QList<qint64>{12, 10}));
        QCOMPARE(rows.selection->marked_ids(), (QList<qint64>{10, 12}));
        // Moving a row keeps the selection order; reselecting puts it last.
        rows.widget.addItem(rows.widget.takeItem(0));
        QCOMPARE(rows.selection->marked_ids(true), (QList<qint64>{12, 10}));
        QCOMPARE(rows.selection->marked_ids(), (QList<qint64>{12, 10}));
        rows.selection->toggle_mark(12);
        rows.selection->toggle_mark(12);
        QCOMPARE(rows.selection->marked_ids(true), (QList<qint64>{10, 12}));
        QCOMPARE(changes.size(), 4);
        QCOMPARE(changes.last().first().toInt(), 2);
    }

    void rangesRunFromTheAnchor() {
        Rows rows;
        rows.selection->toggle_mark(13);
        rows.selection->mark_range_to(10);
        QCOMPARE(rows.selection->marked_ids(true), (QList<qint64>{13, 12, 11, 10}));
        Rows replace;
        replace.selection->toggle_mark(11);
        replace.selection->mark_range_to(13);
        QCOMPARE(replace.selection->marked_ids(), (QList<qint64>{11, 12, 13}));
        replace.selection->mark_range_to(10);
        QCOMPARE(replace.selection->marked_ids(), (QList<qint64>{10, 11}));
        // Without an anchor: from the keyboard row, else just the clicked row.
        Rows fresh;
        fresh.selection->mark_range_to(11);
        QCOMPARE(fresh.selection->marked_ids(), QList<qint64>{11});
        fresh.selection->reset();
        fresh.selection->select_item_id(12);
        fresh.selection->mark_range_to(14);
        QCOMPARE(fresh.selection->marked_ids(), (QList<qint64>{12, 13, 14}));
        fresh.selection->mark_range_to(999);
        QVERIFY(fresh.selection->marked_ids().isEmpty());
    }

    void shiftArrowsAndSpace() {
        Rows rows;
        rows.selection->select_item_id(11);
        rows.key(Qt::Key_Down, Qt::ShiftModifier);
        rows.key(Qt::Key_Down, Qt::ShiftModifier);
        QCOMPARE(rows.selection->marked_ids(), (QList<qint64>{11, 12, 13}));
        rows.key(Qt::Key_Up, Qt::ShiftModifier);
        QCOMPARE(rows.selection->marked_ids(), (QList<qint64>{11, 12}));
        // A plain arrow moves the current row only; the next range starts there.
        rows.key(Qt::Key_Down);
        QCOMPARE(rows.selection->marked_ids(), (QList<qint64>{11, 12}));
        rows.key(Qt::Key_Down, Qt::ShiftModifier);
        QCOMPARE(rows.selection->marked_ids(), (QList<qint64>{13, 14}));
        // Clamped at the end.
        rows.key(Qt::Key_Down, Qt::ShiftModifier);
        QCOMPARE(rows.selection->marked_ids(), (QList<qint64>{13, 14}));

        Rows first;
        first.key(Qt::Key_Down, Qt::ShiftModifier);
        QCOMPARE(first.selection->marked_ids(), QList<qint64>{10});

        Rows space;
        space.selection->select_item_id(12);
        space.key(Qt::Key_Space);
        QCOMPARE(space.selection->marked_ids(), QList<qint64>{12});
        space.key(Qt::Key_Space);
        QVERIFY(space.selection->marked_ids().isEmpty());
        Rows none;
        none.key(Qt::Key_Space);
        QVERIFY(!none.selection->has_marks());
        Rows empty(0);
        empty.key(Qt::Key_Down, Qt::ShiftModifier);
        QVERIFY(!empty.selection->has_marks());
    }

    void clearingAndUnmarking() {
        Rows rows;
        QSignalSpy changes(rows.selection.get(), &SelectionManager::marks_changed);
        QVERIFY(!rows.selection->clear_marks());
        rows.selection->toggle_mark(10);
        rows.selection->toggle_mark(12);
        rows.selection->unmark(12);
        rows.selection->unmark(99);
        QCOMPARE(rows.selection->marked_ids(), QList<qint64>{10});
        QVERIFY(rows.selection->clear_marks());
        QCOMPARE(changes.last().first().toInt(), 0);
        rows.selection->toggle_mark(10);
        rows.selection->reset();
        QVERIFY(rows.selection->marked_ids().isEmpty());
    }

    void indicesFollowRowsTheWindowChanges() {
        Rows rows;
        rows.selection->select_item_id(12);
        rows.selection->shift_selection_after_insert(0);
        QCOMPARE(rows.selection->selected_index(), 3);
        rows.selection->shift_selection_after_insert(4);
        QCOMPARE(rows.selection->selected_index(), 3);
        // Moved itself, moved past from above and from below.
        rows.selection->shift_selection_after_move(3, 0);
        QCOMPARE(rows.selection->selected_index(), 0);
        rows.selection->shift_selection_after_move(2, 0);
        QCOMPARE(rows.selection->selected_index(), 1);
        rows.selection->shift_selection_after_move(0, 3);
        QCOMPARE(rows.selection->selected_index(), 0);
        rows.selection->shift_selection_after_move(4, 2);
        QCOMPARE(rows.selection->selected_index(), 0);
        rows.selection->select_item_id(13);
        rows.selection->shift_selection_after_remove(1);
        QCOMPARE(rows.selection->selected_index(), 2);
        rows.selection->shift_selection_after_remove(4);
        QCOMPARE(rows.selection->selected_index(), 2);
        rows.selection->shift_selection_after_remove(2);
        QVERIFY(!rows.selection->has_selection());
    }
};

QTEST_MAIN(ClipboardSelectionTests)
#include "test_clipboard_selection.moc"
