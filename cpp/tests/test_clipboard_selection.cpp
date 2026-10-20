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
    std::optional<qint64> cursor() const { return selection->cursor_id(); }
    void key(int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) { QTest::keyClick(&widget, static_cast<Qt::Key>(key), modifiers); }
};
}

// The clipboard list's cursor, marks and range anchor, and the keys that drive them.
class ClipboardSelectionTests : public QObject {
    Q_OBJECT
    Environment env_;

private slots:
    void initTestCase() { QVERIFY(env_.init(this)); }

    void theFirstArrowKeyStartsFromAnEnd() {
        Rows rows;
        QVERIFY(!rows.cursor());
        rows.key(Qt::Key_Down);
        QCOMPARE(rows.cursor(), std::optional<qint64>(10));
        Rows up;
        up.key(Qt::Key_Up);
        QCOMPARE(up.cursor(), std::optional<qint64>(14));
    }

    void theMouseAndTheKeysMoveOneCursor() {
        Rows rows;
        rows.selection->point_at(2);
        rows.selection->move_cursor(1);
        QCOMPARE(rows.selection->cursor_row(), 3);
        rows.selection->move_cursor(-1);
        rows.selection->move_cursor(-1);
        QCOMPARE(rows.selection->cursor_row(), 1);
        // Pointing again takes the cursor wherever the keys left it.
        rows.selection->point_at(4);
        QCOMPARE(rows.selection->cursor_row(), 4);
    }

    void navigationClampsWithoutWrapping() {
        Rows rows;
        for (int expected : {0, 1, 2, 3, 4, 4, 4}) {
            rows.selection->move_cursor(1);
            QCOMPARE(rows.selection->cursor_row(), expected);
        }
        for (int i = 0; i < 10; ++i) rows.selection->move_cursor(-1);
        QCOMPARE(rows.selection->cursor_row(), 0);
        Rows empty(0);
        empty.selection->move_cursor(1);
        empty.selection->move_cursor(-1);
        QVERIFY(!empty.cursor());
    }

    void movingTheCursorToAnItem() {
        Rows rows;
        QSignalSpy changed(rows.selection.get(), &SelectionManager::cursor_changed);
        QVERIFY(rows.selection->move_cursor_to(12));
        QCOMPARE(rows.selection->cursor_row(), 2);
        // Unknown and missing ids leave the cursor.
        QVERIFY(!rows.selection->move_cursor_to(999));
        QVERIFY(!rows.selection->move_cursor_to(std::nullopt));
        QCOMPARE(rows.selection->cursor_row(), 2);
        // Pointing at the row it is on changes nothing.
        rows.selection->point_at(2);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(changed.last().first().value<std::optional<qint64>>(), std::optional<qint64>(12));
    }

    void qtsOwnCurrentItemIsIgnored() {
        Rows rows;
        rows.widget.setFocus();
        rows.widget.setCurrentRow(4);
        QVERIFY(!rows.cursor());
    }

    void clearingAndResetting() {
        Rows rows;
        rows.selection->move_cursor(1);
        rows.selection->clear_cursor();
        QVERIFY(!rows.cursor());
        // With no cursor the next key starts from an end again.
        rows.selection->move_cursor(-1);
        QCOMPARE(rows.selection->cursor_row(), 4);
        rows.selection->toggle_mark(11);
        rows.selection->reset();
        QVERIFY(!rows.cursor());
        QVERIFY(!rows.selection->has_marks());
        // The anchor went with it: a range is only the clicked row.
        rows.selection->mark_range_to(13);
        QCOMPARE(rows.selection->marked_ids(), QList<qint64>{13});
    }

    void enterActivatesTheCursorRow() {
        Rows rows;
        QSignalSpy activated(rows.selection.get(), &SelectionManager::item_activated);
        rows.key(Qt::Key_Return);
        QCOMPARE(activated.size(), 0);
        rows.key(Qt::Key_Down);
        rows.key(Qt::Key_Return);
        rows.key(Qt::Key_Enter);
        QCOMPARE(activated.size(), 2);
        QCOMPARE(activated.first().first().toLongLong(), qint64(10));
        // Other keys pass through to the list.
        rows.key(Qt::Key_A);
        QCOMPARE(rows.cursor(), std::optional<qint64>(10));
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

    void escapeIsLeftToTheWindow() {
        Rows rows;
        rows.selection->move_cursor_to(11);
        rows.selection->toggle_mark(11);
        rows.key(Qt::Key_Escape);
        QCOMPARE(rows.cursor(), std::optional<qint64>(11));
        QVERIFY(rows.selection->has_marks());
    }

    void theSearchBoxDrivesTheList() {
        Rows rows;
        QLineEdit search;
        rows.selection->set_search_input(&search);
        QSignalSpy activated(rows.selection.get(), &SelectionManager::item_activated);
        QSignalSpy switched(rows.selection.get(), &SelectionManager::request_group_switch);
        QTest::keyClick(&search, Qt::Key_Down);
        QTest::keyClick(&search, Qt::Key_Down);
        QCOMPARE(rows.cursor(), std::optional<qint64>(11));
        QTest::keyClick(&search, Qt::Key_Up);
        QCOMPARE(rows.cursor(), std::optional<qint64>(10));
        QTest::keyClick(&search, Qt::Key_Return);
        QCOMPARE(activated.size(), 1);
        QTest::keyClick(&search, Qt::Key_Down, Qt::ShiftModifier);
        QCOMPARE(rows.selection->marked_ids(), (QList<qint64>{10, 11}));
        // Space and left/right edit the text unless the group bar is on top.
        QTest::keyClick(&search, Qt::Key_Space);
        QCOMPARE(rows.selection->marked_ids(), (QList<qint64>{10, 11}));
        QTest::keyClick(&search, Qt::Key_Right);
        QCOMPARE(switched.size(), 0);
        rows.selection->group_bar_position = "top";
        QTest::keyClick(&search, Qt::Key_Right);
        QTest::keyClick(&search, Qt::Key_Left);
        QCOMPARE(switched.size(), 2);
        // Typing reaches the box.
        QTest::keyClicks(&search, "abc");
        QCOMPARE(search.text(), QString(" abc"));
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

    void markingMovesTheCursorToTheRow() {
        Rows rows;
        rows.selection->toggle_mark(13);
        QCOMPARE(rows.cursor(), std::optional<qint64>(13));
        rows.selection->toggle_mark(13);
        QVERIFY(!rows.selection->has_marks());
        QCOMPARE(rows.cursor(), std::optional<qint64>(13));
        rows.selection->mark_range_to(11);
        QCOMPARE(rows.cursor(), std::optional<qint64>(11));
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
        // Pointing never moves the anchor: the mouse reaches the target row before the Shift+click.
        Rows pointed;
        pointed.selection->set_anchor(11);
        pointed.selection->point_at(3);
        pointed.selection->mark_range_to(13);
        QCOMPARE(pointed.selection->marked_ids(), (QList<qint64>{11, 12, 13}));
        // A plain arrow key sets it.
        Rows keys;
        keys.selection->move_cursor_to(12);
        keys.key(Qt::Key_Down);
        keys.selection->mark_range_to(10);
        QCOMPARE(keys.selection->marked_ids(), (QList<qint64>{10, 11, 12, 13}));
        // Without an anchor: from the cursor, else just the row.
        Rows fresh;
        fresh.selection->mark_range_to(11);
        QCOMPARE(fresh.selection->marked_ids(), QList<qint64>{11});
        fresh.selection->reset();
        fresh.selection->move_cursor_to(12);
        fresh.selection->mark_range_to(14);
        QCOMPARE(fresh.selection->marked_ids(), (QList<qint64>{12, 13, 14}));
        fresh.selection->mark_range_to(999);
        QVERIFY(fresh.selection->marked_ids().isEmpty());
    }

    void shiftArrowsAndSpace() {
        Rows rows;
        rows.selection->move_cursor_to(11);
        rows.key(Qt::Key_Down, Qt::ShiftModifier);
        rows.key(Qt::Key_Down, Qt::ShiftModifier);
        QCOMPARE(rows.selection->marked_ids(), (QList<qint64>{11, 12, 13}));
        rows.key(Qt::Key_Up, Qt::ShiftModifier);
        QCOMPARE(rows.selection->marked_ids(), (QList<qint64>{11, 12}));
        // A plain arrow moves the cursor only; the next range starts there.
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
        space.selection->move_cursor_to(12);
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

    void theCursorFollowsRowsTheWindowChanges() {
        Rows rows;
        rows.selection->move_cursor_to(12);
        rows.selection->row_inserted(0);
        QCOMPARE(rows.selection->cursor_row(), 3);
        rows.selection->row_inserted(4);
        QCOMPARE(rows.selection->cursor_row(), 3);
        // Moved itself, moved past from above and from below.
        rows.selection->row_moved(3, 0);
        QCOMPARE(rows.selection->cursor_row(), 0);
        rows.selection->row_moved(2, 0);
        QCOMPARE(rows.selection->cursor_row(), 1);
        rows.selection->row_moved(0, 3);
        QCOMPARE(rows.selection->cursor_row(), 0);
        rows.selection->row_moved(4, 2);
        QCOMPARE(rows.selection->cursor_row(), 0);
        rows.selection->move_cursor_to(13);
        rows.selection->row_removed(1);
        QCOMPARE(rows.selection->cursor_row(), 2);
        rows.selection->row_removed(4);
        QCOMPARE(rows.selection->cursor_row(), 2);
        rows.selection->row_removed(2);
        QVERIFY(!rows.cursor());
    }
};

QTEST_MAIN(ClipboardSelectionTests)
#include "test_clipboard_selection.moc"
