#include "clipboard_test_support.h"
#include "clipboard/controllers/clipboard_controller.h"
#include "clipboard/ui/dialogs/manage_dialog.h"
#include "core/ui_scale.h"
#include <QSignalSpy>
#include <QTest>

using namespace clipboard_test;

// The clipboard window's controller over a real database: paging, filters, groups,
// pasting and the list kept in step without reloads.
class ClipboardControllerTests : public QObject {
    Q_OBJECT
    Environment env_;
    ClipboardManager &manager() { return ClipboardManager::instance(); }

private slots:
    void initTestCase() { QVERIFY(env_.init(this)); }

    void init() {
        QVERIFY(env_.fresh_database());
        env_.config->set_clipboard_show_metadata(true);
        env_.config->set_clipboard_move_to_top_on_paste(true);
        env_.config->set_clipboard_auto_paste(true);
        env_.config->set_clipboard_multi_paste_order("oldest_first");
        env_.config->set_clipboard_multi_paste_separator("\n");
        env_.config->set_clipboard_multi_paste_keep_merged(false);
    }

    void cleanup() {
        // Delayed pastes fire 50 ms after the action.
        QTest::qWait(80);
        if (auto *dialog = get_existing_manage_dialog()) {
            dialog->hide();
            destroy_manage_dialog();
        }
    }

    void settingsAreReadAndWrittenThrough() {
        env_.config->set_clipboard_auto_paste(false);
        env_.config->set_app_setting("clipboard_paste_with_html", false);
        ClipboardController controller(manager());
        QVERIFY(!controller.auto_paste_enabled);
        QVERIFY(!controller.paste_with_html);
        controller.set_auto_paste(true);
        controller.set_paste_with_html(true);
        controller.set_move_to_top_on_paste(false);
        QVERIFY(env_.config->get_clipboard_auto_paste());
        QVERIFY(env_.config->get_app_setting("clipboard_paste_with_html").toBool());
        QVERIFY(!env_.config->get_clipboard_move_to_top_on_paste());
    }

    void historyLoadsPageByPageWhileScrollingDown() {
        QStringList texts;
        for (int i = 0; i < 80; ++i) texts << QString("entry %1").arg(i);
        add_texts(texts);
        ClipboardController controller(manager());
        QSignalSpy loaded(&controller, &ClipboardController::data_loaded);
        controller.load_history();
        // Newest first, one page of 38.
        QCOMPARE(controller.current_items.size(), 38);
        QCOMPARE(controller.current_items.first().content, QString("entry 79"));
        QVERIFY(controller.has_more_items());
        QCOMPARE(loaded.size(), 1);
        QVERIFY(loaded.first().at(1).toBool());

        // No scroll bar, scrolling up, or far from the bottom loads nothing.
        controller.check_scroll_load(0, 0);
        controller.check_scroll_load(500, 1000);
        controller.check_scroll_load(400, 1000);
        QCOMPARE(controller.current_items.size(), 38);
        controller.check_scroll_load(980, 1000);
        QCOMPARE(controller.current_items.size(), 76);
        QVERIFY(!loaded.last().at(1).toBool());
        controller.load_more_if_needed();
        QCOMPARE(controller.current_items.size(), 80);
        QVERIFY(!controller.has_more_items());
        controller.load_more_if_needed();
        QCOMPARE(controller.current_items.size(), 80);
        // No duplicates across page boundaries.
        const auto loaded_ids = ids_of(controller.current_items);
        QCOMPARE(QSet<qint64>(loaded_ids.begin(), loaded_ids.end()).size(), 80);

        // The time shares the text's line, so hiding it fits no more rows into a page.
        env_.config->set_clipboard_show_metadata(false);
        controller.load_history();
        QCOMPARE(controller.current_items.size(), 38);
    }

    void aReloadAskedForDuringALoadRunsAfterIt() {
        add_texts({"a", "b"});
        ClipboardController controller(manager());
        QSignalSpy completed(&controller, &ClipboardController::load_completed);
        bool asked = false;
        connect(&controller, &ClipboardController::data_loaded, this, [&] {
            if (asked) return;
            asked = true;
            controller.load_history();
        });
        controller.load_history();
        QCOMPARE(completed.size(), 1);
        QTRY_COMPARE(completed.size(), 2);
        QCOMPARE(controller.current_items.size(), 2);
    }

    void searchAndFiltersNarrowTheHistory() {
        const auto ids = add_texts({"Apple pie", "banana", "APPLE juice"});
        QVERIFY(manager().add_item(file_payload({"C:\\docs\\apple.txt"}), "file"));
        QVERIFY(manager().add_item("[10x10]", "image"));
        ClipboardController controller(manager());
        controller.load_history();
        QCOMPARE(controller.current_items.size(), 5);

        controller.set_search_text("  apple  ");
        QVERIFY(contents_of(controller.current_items).contains("Apple pie"));
        QVERIFY(contents_of(controller.current_items).contains("APPLE juice"));
        QVERIFY(!contents_of(controller.current_items).contains("banana"));
        // Blank search shows everything again.
        controller.set_search_text("   ");
        QCOMPARE(controller.current_items.size(), 5);

        for (const QString type : {"text", "image", "file"}) {
            controller.set_content_type_filter(type);
            QVERIFY(!controller.current_items.isEmpty());
            for (const auto &item : controller.current_items) QCOMPARE(item.content_type, type);
        }
        controller.set_content_type_filter(std::nullopt);
        QCOMPARE(controller.current_items.size(), 5);

        const QDate today = QDate::currentDate();
        QVERIFY(controller.set_time_range_text(today.toString("yyyy/MM/dd") + "-" + today.toString("yyyy/MM/dd")));
        QCOMPARE(controller.current_items.size(), 5);
        QVERIFY(controller.set_time_range_text("2000/01/01-2000/01/02"));
        QVERIFY(controller.current_items.isEmpty());
        // Invalid text is refused and keeps the range; empty text clears it.
        QVERIFY(!controller.set_time_range_text("yesterday"));
        QVERIFY(controller.current_items.isEmpty());
        QVERIFY(controller.set_time_range_text(""));
        QCOMPARE(controller.current_items.size(), 5);
        controller.set_content_type_filter(QString("text"));
        QVERIFY(controller.set_time_range_text("2000/01/01-2000/01/02"));
        controller.clear_time_and_type_filters();
        QCOMPARE(controller.current_items.size(), 5);
        Q_UNUSED(ids);
    }

    void searchInsideAGroupLooksAtEveryPage() {
        const auto group = manager().create_group("Notes");
        QVERIFY(group);
        QStringList texts;
        for (int i = 0; i < 50; ++i) texts << QString("plain %1").arg(i);
        texts << "needle at the end";
        for (qint64 id : add_texts(texts)) QVERIFY(manager().move_to_group(id, *group));
        const auto titled = manager().add_item("body", "text", QString("Needle title"));
        QVERIFY(manager().move_to_group(*titled, *group));
        ClipboardController controller(manager());
        controller.switch_to_group(*group);
        QCOMPARE(controller.current_items.size(), 38);
        controller.set_search_text("NEEDLE");
        // The first page has no match, yet more pages remain to be searched.
        QVERIFY(controller.current_items.isEmpty());
        QVERIFY(controller.has_more_items());
        controller.load_more_if_needed();
        QCOMPARE(contents_of(controller.current_items), (QStringList{"needle at the end", "body"}));
    }

    void groupsSwitchMoveAndDelete() {
        const auto a = manager().create_group("A"), b = manager().create_group("B"), c = manager().create_group("C");
        QVERIFY(a && b && c);
        ClipboardController controller(manager());
        QCOMPARE(controller.get_group_move_state(*a), std::make_pair(false, true));
        QCOMPARE(controller.get_group_move_state(*b), std::make_pair(true, true));
        QCOMPARE(controller.get_group_move_state(*c), std::make_pair(true, false));
        QCOMPARE(controller.get_group_move_state(999), std::make_pair(false, false));
        QVERIFY(controller.move_group_order(*c, -1));
        QVERIFY(controller.move_group_order(*c, -1));
        QList<qint64> order;
        for (const auto &group : controller.get_groups()) order << group.id;
        QCOMPARE(order, (QList<qint64>{*c, *a, *b}));
        QVERIFY(!controller.move_group_order(*c, -1));
        QVERIFY(!controller.move_group_order(*b, 1));
        QVERIFY(!controller.move_group_order(999, 1));

        const auto item = add_texts({"in A"}).first();
        QVERIFY(manager().move_to_group(item, *a));
        controller.switch_to_group(*a);
        QCOMPARE(contents_of(controller.current_items), QStringList{"in A"});
        QSignalSpy reload(&controller, &ClipboardController::reload_required);
        QVERIFY(controller.delete_group(*a));
        QVERIFY(!controller.current_group_id);
        QCOMPARE(reload.size(), 1);
        // The group's items go back to the history rather than being deleted.
        QVERIFY(manager().get_item(item));
        QVERIFY(ids_of(manager().get_by_group(std::nullopt)).contains(item));
    }

    void aFilterGroupShowsOneTypeOfTheHistory() {
        const auto texts = add_texts({"apple", "banana"});
        const auto file = manager().add_item(file_payload({"C:\\f.txt"}), "file");
        QVERIFY(file && manager().add_item("[10x10]", "image"));
        const auto only_text = manager().create_group("Text", std::nullopt, std::nullopt, static_cast<int>(GroupType::FILTER), FilterRule::of_kind("text"));
        const auto plain = manager().create_group("Plain");
        ClipboardController controller(manager());
        QVERIFY(controller.shows_history());
        // The type button says file; a filter group's own type wins.
        controller.set_content_type_filter(QString("file"));
        controller.switch_to_group(*only_text);
        QCOMPARE(controller.current_filter_rule(), std::optional(FilterRule::of_kind("text")));
        QVERIFY(controller.shows_history());
        QCOMPARE(contents_of(controller.current_items), (QStringList{"banana", "apple"}));
        // Search and the time range still narrow it.
        controller.set_search_text("apple");
        QCOMPARE(contents_of(controller.current_items), QStringList{"apple"});
        controller.set_search_text("");
        // Pasting there moves the row up like in the history.
        QVERIFY(controller.paste_item(texts[0]));
        QCOMPARE(contents_of(controller.current_items), (QStringList{"apple", "banana"}));
        // Moving an item into a group keeps it listed.
        QVERIFY(controller.move_to_group(texts[1], *plain));
        QCOMPARE(controller.current_items.size(), 2);
        // A real group is not the history.
        controller.switch_to_group(*plain);
        QVERIFY(!controller.shows_history());
        QVERIFY(!controller.current_filter_rule());
        QCOMPARE(contents_of(controller.current_items), QStringList{"banana"});
    }

    void groupMenusLeaveOutGroupsThatTakeNothing() {
        const auto text = add_texts({"text"}).first();
        const auto file = manager().add_item(file_payload({"C:\\f.txt"}), "file");
        const auto normal = manager().create_group("N");
        const auto quick = manager().create_group("Q", std::nullopt, std::nullopt, static_cast<int>(GroupType::FILE));
        const auto filter = manager().create_group("F", std::nullopt, std::nullopt, static_cast<int>(GroupType::FILTER), FilterRule::of_kind("file"));
        QVERIFY(normal && quick && filter && file);
        ClipboardController controller(manager());
        controller.load_history();
        const auto targets = [](const std::optional<ContextMenuData> &data) {
            QStringList keys;
            for (const auto &action : data->actions)
                if (action.key == "move_group_menu")
                    for (const auto &child : action.children) keys << child.key;
            return keys;
        };
        QCOMPARE(targets(controller.build_context_menu_data(text)), QStringList{QString("move_to_group_%1").arg(*normal)});
        QCOMPARE(targets(controller.build_context_menu_data(*file)), (QStringList{QString("move_to_group_%1").arg(*normal), QString("move_to_group_%1").arg(*quick)}));
        QCOMPARE(targets(controller.build_marked_context_menu_data({text, *file})), QStringList{QString("move_to_group_%1").arg(*normal)});
        // Items shown in a filter group get the history menu, not the group one.
        controller.switch_to_group(*filter);
        // Keep the optional alive: a range-for over a member of a temporary would iterate freed memory.
        const auto data = controller.build_context_menu_data(*file);
        QVERIFY(data);
        QStringList keys;
        for (const auto &action : data->actions) keys << action.key;
        QVERIFY(!keys.contains("edit_item") && !keys.contains("move_item_up") && !keys.contains("move_item_down"));
    }

    void textSubKindsAddTheirMenuEntries() {
        const QString existing = QDir::toNativeSeparators(env_.dir.filePath("doc.txt"));
        QFile file(existing);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        const auto ids = add_texts({"\"" + existing + "\"", QDir::toNativeSeparators(env_.dir.filePath("gone.txt")), "www.example.com", "#1a2b3c",
                                    "rgb(26, 43, 60)", "plain"});
        ClipboardController controller(manager());
        controller.load_history();
        struct Menu {
            QStringList keys;
            QStringList special;
            QStringList special_labels;
            bool as_files_enabled = false;
        };
        const auto menu_of = [&controller](qint64 id) {
            Menu menu;
            const auto data = controller.build_context_menu_data(id);
            for (const auto &action : data->actions) {
                menu.keys << action.key;
                if (action.key != "special_paste_menu") continue;
                for (const auto &child : action.children) {
                    menu.special << child.key;
                    if (child.key.startsWith("color_paste_")) menu.special_labels << child.label;
                    if (child.key == "path_paste_as_files") menu.as_files_enabled = child.enabled;
                }
            }
            return menu;
        };
        const Menu path = menu_of(ids[0]), missing = menu_of(ids[1]), link = menu_of(ids[2]), hex = menu_of(ids[3]), css = menu_of(ids[4]), plain = menu_of(ids[5]);
        QVERIFY(path.keys.contains("open_file_location") && !path.keys.contains("open_link"));
        QCOMPARE(path.special.first(), QString("path_paste_as_files"));
        QVERIFY(path.as_files_enabled);
        QVERIFY(missing.special.contains("path_paste_as_files") && !missing.as_files_enabled);
        QVERIFY(link.keys.contains("open_link") && !link.keys.contains("open_file_location"));
        // The color picker's default formats are RGB and HEX; the one the text is written in is left out.
        QCOMPARE(hex.special_labels, QStringList{"26, 43, 60"});
        QCOMPARE(css.special_labels, (QStringList{"26, 43, 60", "#1A2B3C"}));
        QVERIFY(!plain.keys.contains("open_file_location") && !plain.keys.contains("open_link"));
        QVERIFY(plain.special_labels.isEmpty() && !plain.special.contains("path_paste_as_files"));
        // Picking a conversion pastes that text.
        QVERIFY(controller.paste_color_conversion(ids[4], 1));
        QTRY_COMPARE(env_.pastes.size(), 1);
        QVERIFY(!controller.paste_color_conversion(ids[4], 2));
        QVERIFY(!controller.paste_color_conversion(ids[5], 0));
    }

    void pastingMovesTheRowToTheTopOfTheHistoryOnly() {
        const auto ids = add_texts({"first", "second", "third"});
        ClipboardController controller(manager());
        controller.load_history();
        QCOMPARE(ids_of(controller.current_items), (QList<qint64>{ids[2], ids[1], ids[0]}));
        QSignalSpy moved(&controller, &ClipboardController::item_moved_to_top);
        int closed = 0;
        QVERIFY(controller.paste_item(ids[0], [&closed] { ++closed; }));
        QCOMPARE(closed, 1);
        QCOMPARE(ids_of(controller.current_items), (QList<qint64>{ids[0], ids[2], ids[1]}));
        QCOMPARE(moved.size(), 1);
        QTRY_COMPARE(env_.pastes.size(), 1);
        // The database agrees with the list.
        QCOMPARE(ids_of(manager().get_history()), ids_of(controller.current_items));

        // Already at the top: nothing moves.
        QVERIFY(controller.paste_item(ids[0]));
        QCOMPARE(moved.size(), 1);
        QTRY_COMPARE(env_.pastes.size(), 2);

        // With auto-paste off only an explicit paste presses keys.
        controller.auto_paste_enabled = false;
        env_.pastes.clear();
        QVERIFY(controller.paste_item(ids[1]));
        QTest::qWait(80);
        QVERIFY(env_.pastes.isEmpty());
        QVERIFY(controller.paste_item(ids[1], {}, true));
        QTRY_COMPARE(env_.pastes.size(), 1);

        // Inside a group the order stays.
        const auto group = manager().create_group("G");
        for (qint64 id : ids) QVERIFY(manager().move_to_group(id, *group));
        controller.switch_to_group(*group);
        const auto before = ids_of(controller.current_items);
        QVERIFY(controller.paste_item(before.last()));
        QCOMPARE(ids_of(controller.current_items), before);

        QVERIFY(!controller.paste_item(999999));
    }

    void pinnedRowsStayAboveMovedOnes() {
        const auto ids = add_texts({"old", "pinned", "new"});
        QVERIFY(manager().toggle_pin(ids[1]));
        ClipboardController controller(manager());
        controller.load_history();
        QCOMPARE(controller.current_items.first().id, ids[1]);
        QVERIFY(controller.paste_item(ids[0]));
        QCOMPARE(ids_of(controller.current_items), (QList<qint64>{ids[1], ids[0], ids[2]}));
        // A pinned row is not moved.
        QSignalSpy moved(&controller, &ClipboardController::item_moved_to_top);
        QVERIFY(controller.paste_item(ids[1]));
        QCOMPARE(moved.size(), 0);
        controller.toggle_pin(ids[1]);
        QVERIFY(!manager().get_item(ids[1])->is_pinned);
    }

    void multiPasteFollowsTheSettings() {
        const auto ids = add_texts({"one", "two", "three", "four"});
        ClipboardController controller(manager());
        controller.load_history();
        QCOMPARE(controller.multi_paste_order({ids[2], ids[0]}), (QList<qint64>{ids[2], ids[0]}));
        env_.config->set_clipboard_multi_paste_order("newest_first");
        QCOMPARE(controller.multi_paste_order({ids[2], ids[0]}), (QList<qint64>{ids[0], ids[2]}));
        env_.config->set_clipboard_multi_paste_order("oldest_first");

        int closed = 0;
        QVERIFY(controller.paste_items({ids[0], ids[2]}, [&closed] { ++closed; }));
        QCOMPARE(closed, 1);
        // Both move up together, keeping their relative order.
        QCOMPARE(ids_of(controller.current_items).mid(0, 2), (QList<qint64>{ids[2], ids[0]}));
        QTRY_COMPARE(env_.pastes.size(), 1);
        QCOMPARE(ids_of(manager().get_history()), ids_of(controller.current_items));

        // In a group nothing moves; plain text and a custom separator still paste.
        env_.config->set_clipboard_multi_paste_separator(" | ");
        const auto group = manager().create_group("G");
        for (qint64 id : ids) QVERIFY(manager().move_to_group(id, *group));
        controller.switch_to_group(*group);
        const auto before = ids_of(controller.current_items);
        QVERIFY(controller.paste_items({ids[3], ids[1]}, {}, true, true));
        QCOMPARE(ids_of(controller.current_items), before);

        // Nothing usable: nothing written, the window stays.
        closed = 0;
        QVERIFY(!controller.paste_items({999998, 999999}, [&closed] { ++closed; }));
        QCOMPARE(closed, 0);
    }

    void imagesAreStitchedInTheBackground() {
        // Image records without stored pixels: the stitch runs and writes nothing. Titled, so
        // equal contents are not merged into one record.
        const auto a = manager().add_item("[4000x3000]", "image", QString("a")), b = manager().add_item("[4000x3000]", "image", QString("b"));
        QVERIFY(a && b);
        ClipboardController controller(manager());
        controller.load_history();
        int closed = 0;
        QVERIFY(controller.paste_items({*a, *b}, [&closed] { ++closed; }));
        // The window closes at once; the result arrives later.
        QCOMPARE(closed, 1);
        QTest::qWait(300);
        QVERIFY(env_.pastes.isEmpty());

        QVERIFY(controller.stitched_image_fits({*a, *b}));
        QVERIFY(controller.stitched_image_fits({*a, *b}, "horizontal"));
        QList<qint64> big;
        for (int i = 0; i < 6; ++i) big << manager().add_item("[4000x3000]", "image", QString("t%1").arg(i)).value_or(-1);
        controller.load_history();
        QVERIFY(!controller.stitched_image_fits(big));
        QVERIFY(!controller.stitched_image_fits(big, "horizontal"));
        QVERIFY(controller.stitched_image_fits(big.mid(0, 5)));
        // Unknown sizes and rows that are not loaded are left to the backend.
        const auto unknown = manager().add_item("[PNG 3.1 MB]", "image");
        controller.load_history();
        QVERIFY(controller.stitched_image_fits({*unknown, big[0]}));
        QVERIFY(controller.stitched_image_fits({999999}));
        QVERIFY(controller.stitched_image_fits({}));
    }

    void specialPasteWritesTransformedText() {
        const auto text = manager().add_item("Hello World");
        const auto file = manager().add_item(file_payload({"C:\\docs\\a.txt", "C:/docs/sub/b.pdf"}), "file");
        const auto broken = manager().add_item("not json", "file");
        const auto empty = manager().add_item("{\"files\": []}", "file");
        QVERIFY(text && file && broken && empty);
        ClipboardController controller(manager());
        controller.load_history();
        int closed = 0;
        const auto close = [&closed] { ++closed; };

        QVERIFY(controller.paste_transformed_text(*text, "transform_uppercase", close));
        QCOMPARE(closed, 1);
        QVERIFY(controller.paste_transformed_text(*text, "special_paste_plain_text", close));
        QVERIFY(controller.paste_transformed_text(*text, "special_paste_in_order", close));
        QCOMPARE(closed, 3);
        QVERIFY(!controller.paste_transformed_text(*text, "no_such_transform"));
        QVERIFY(!controller.paste_transformed_text(*file, "transform_uppercase"));
        QVERIFY(!controller.paste_transformed_text(*file, "special_paste_plain_text"));
        QVERIFY(!controller.paste_transformed_text(999999, "special_paste_in_order"));

        QVERIFY(controller.paste_file_text(*file, "file_paste_names", close));
        QVERIFY(controller.paste_file_text(*file, "file_paste_links", close));
        QCOMPARE(closed, 5);
        QVERIFY(!controller.paste_file_text(*file, "file_paste_unknown"));
        QVERIFY(!controller.paste_file_text(*text, "file_paste_names"));
        QVERIFY(!controller.paste_file_text(*broken, "file_paste_names"));
        QVERIFY(!controller.paste_file_text(*empty, "file_paste_names"));
        QTRY_COMPARE(env_.pastes.size(), 5);
    }

    void editsKeepTheLoadedListInStep() {
        const auto ids = add_texts({"a", "b", "c", "d"});
        const auto group = manager().create_group("G");
        ClipboardController controller(manager());
        controller.load_history();
        QSignalSpy removed(&controller, &ClipboardController::item_removed);
        QSignalSpy updated(&controller, &ClipboardController::item_updated);

        QVERIFY(controller.delete_item(ids[3]));
        QCOMPARE(removed.size(), 1);
        QCOMPARE(removed.first().first().toInt(), 0);

        // Moving to a group keeps the row in the history view.
        QVERIFY(controller.move_to_group(ids[2], *group));
        QCOMPARE(controller.current_items.size(), 3);

        // Editing keeps the title and refreshes the row in place.
        QVERIFY(manager().update_item(ids[1], "b", QString("kept")));
        QVERIFY(controller.update_item_content(ids[1], "b edited"));
        QCOMPARE(updated.size(), 1);
        const auto row = controller.loaded_item(ids[1]);
        QVERIFY(row);
        QCOMPARE(row->content, QString("b edited"));
        QCOMPARE(row->title.value_or(QString()), QString("kept"));
        QVERIFY(!controller.update_item_content(999999, "x"));

        QCOMPARE(controller.delete_items({ids[0]}), 1);
        QCOMPARE(ids_of(controller.current_items), (QList<qint64>{ids[2], ids[1]}));

        // In a group view, moving out removes the row; moving into the same group keeps it.
        controller.switch_to_group(*group);
        QCOMPARE(ids_of(controller.current_items), QList<qint64>{ids[2]});
        QVERIFY(controller.move_to_group(ids[2], *group));
        QCOMPARE(controller.current_items.size(), 1);
        QCOMPARE(controller.move_items_to_group({ids[2]}, std::nullopt), 1);
        QVERIFY(controller.current_items.isEmpty());

        QVERIFY(controller.clear_history());
        QVERIFY(controller.current_items.isEmpty());
    }

    void clearingKeepsPinnedAndGroupedItems() {
        const auto group = manager().create_group("Kept");
        const auto ids = add_texts({"loose", "grouped", "pinned"});
        QVERIFY(manager().move_to_group(ids[1], *group));
        QVERIFY(manager().toggle_pin(ids[2]));
        ClipboardController controller(manager());
        controller.load_history();
        QCOMPARE(controller.current_items.size(), 3);

        QVERIFY(controller.clear_history());
        QCOMPARE(ids_of(controller.current_items), (QList<qint64>{ids[2], ids[1]}));
        QVERIFY(!manager().get_item(ids[0]));
    }

    void reorderingInsideAGroupSwapsLoadedRows() {
        const auto group = manager().create_group("G");
        const auto ids = add_texts({"a", "b", "c"});
        for (qint64 id : ids) QVERIFY(manager().move_to_group(id, *group));
        ClipboardController controller(manager());
        controller.switch_to_group(*group);
        QCOMPARE(ids_of(controller.current_items), ids);
        QCOMPARE(controller.get_item_move_state(ids[0], *group), std::make_pair(false, true));
        QCOMPARE(controller.get_item_move_state(ids[1], *group), std::make_pair(true, true));
        QCOMPARE(controller.get_item_move_state(ids[2], *group), std::make_pair(true, false));
        QCOMPARE(controller.get_item_move_state(ids[0], std::nullopt), std::make_pair(false, false));
        QCOMPARE(controller.get_item_move_state(999999, *group), std::make_pair(false, false));

        QSignalSpy row_moved(&controller, &ClipboardController::item_row_moved);
        QVERIFY(controller.move_item_order(ids[0], *group, 1));
        QCOMPARE(row_moved.size(), 1);
        QCOMPARE(ids_of(controller.current_items), (QList<qint64>{ids[1], ids[0], ids[2]}));
        QCOMPARE(ids_of(manager().get_by_group(*group)), ids_of(controller.current_items));
        QVERIFY(controller.move_item_order(ids[2], *group, -1));
        QCOMPARE(ids_of(controller.current_items), (QList<qint64>{ids[1], ids[2], ids[0]}));
        QVERIFY(!controller.move_item_order(ids[1], *group, -1));
        QVERIFY(!controller.move_item_order(ids[0], *group, 1));
        QVERIFY(!controller.move_item_order(ids[0], std::nullopt, 1));
        QVERIFY(!controller.move_item_order(999999, *group, 1));

        // While searching, neighbours in the list need not be neighbours: the list reloads.
        controller.set_search_text("a");
        QSignalSpy loaded(&controller, &ClipboardController::data_loaded);
        QVERIFY(controller.move_item_order(ids[0], *group, -1));
        QCOMPARE(loaded.size(), 1);
        QCOMPARE(row_moved.size(), 2);

        // Crossing the pinned boundary changes the database order only.
        controller.set_search_text("");
        QVERIFY(manager().toggle_pin(ids[1]));
        controller.load_history();
        const auto shown = ids_of(controller.current_items);
        QVERIFY(controller.move_item_order(shown[1], *group, -1));
        QCOMPARE(ids_of(controller.current_items), shown);
    }

    void newContentIsInsertedWithoutAReload() {
        const auto ids = add_texts({"a", "b"});
        QVERIFY(manager().toggle_pin(ids[0]));
        ClipboardController controller(manager());
        controller.load_history();
        QSignalSpy inserted(&controller, &ClipboardController::item_inserted);
        QSignalSpy loaded(&controller, &ClipboardController::data_loaded);

        // A hidden window ignores it.
        const auto c = manager().add_item("c");
        controller.on_new_content(false, manager().get_item(*c));
        QCOMPARE(inserted.size(), 0);
        QCOMPARE(loaded.size(), 0);

        // Below the pinned block, without a reload.
        controller.on_new_content(true, manager().get_item(*c));
        QCOMPARE(inserted.size(), 1);
        QCOMPARE(inserted.first().at(1).toInt(), 1);
        QCOMPARE(loaded.size(), 0);
        QCOMPARE(ids_of(controller.current_items), (QList<qint64>{ids[0], *c, ids[1]}));

        // A repeated copy is already listed: reload.
        controller.on_new_content(true, manager().get_item(ids[1]));
        QCOMPARE(loaded.size(), 1);
        // No item, or a filtered view: reload.
        controller.on_new_content(true);
        QCOMPARE(loaded.size(), 2);
        controller.set_search_text("x");
        const auto d = manager().add_item("d");
        const int loads = loaded.size();
        controller.on_new_content(true, manager().get_item(*d));
        QCOMPARE(loaded.size(), loads + 1);
        QCOMPARE(inserted.size(), 1);

        // The next page continues after the inserted row instead of repeating one.
        controller.set_search_text("");
        QStringList more;
        for (int i = 0; i < 40; ++i) more << QString("more %1").arg(i);
        add_texts(more);
        controller.load_history();
        const auto fresh = manager().add_item("fresh");
        controller.on_new_content(true, manager().get_item(*fresh));
        controller.load_more_if_needed();
        const auto all = ids_of(controller.current_items);
        QCOMPARE(QSet<qint64>(all.begin(), all.end()).size(), all.size());
        QCOMPARE(all.size(), static_cast<int>(manager().get_total_count()));
    }

    void theSideBarKeepsTheCurrentGroupVisible() {
        get_ui_scale().set_percent(100);
        QList<qint64> groups;
        for (int i = 0; i < 6; ++i) groups << manager().create_group(QString("G%1").arg(i)).value_or(-1);
        QVERIFY(manager().create_group("Hidden", std::nullopt, std::nullopt, static_cast<int>(GroupType::HIDDEN)));
        ClipboardController controller(manager());
        // Before layout everything counts as visible; hidden groups never show.
        auto [visible, hidden] = controller.get_sidebar_overflow(0);
        QCOMPARE(visible.size(), 6);
        QVERIFY(hidden.isEmpty());
        // Room for two buttons in the side bar.
        const int bar = 197 + 2 * 38;
        std::tie(visible, hidden) = controller.get_sidebar_overflow(bar);
        QCOMPARE(visible.size(), 2);
        QCOMPARE(hidden.size(), 4);
        controller.switch_to_group(groups[4]);
        std::tie(visible, hidden) = controller.get_sidebar_overflow(bar);
        QCOMPARE(visible.last().id, groups[4]);
        QCOMPARE(hidden.first().id, groups[1]);
        // The top bar counts its width instead.
        std::tie(visible, hidden) = controller.get_sidebar_overflow(10000, true);
        QCOMPARE(visible.size(), 6);
    }

    void theManageWindowOpensOnAGroupOrItem() {
        const auto group = manager().create_group("G");
        const auto item = add_texts({"x"}).first();
        QVERIFY(manager().move_to_group(item, *group));
        ClipboardController controller(manager());
        bool connected = false;
        auto *dialog = controller.open_manage_dialog_for_group(*group, [&connected](ManageDialog *) { connected = true; });
        QVERIFY(connected);
        QCOMPARE(dialog->editing_group_id, std::optional<qint64>(*group));
        auto *same = controller.open_manage_dialog_for_item(item, *group);
        QCOMPARE(same, dialog);
        QCOMPARE(dialog->editing_item_id, std::optional<qint64>(item));
    }

    void showingTheWindowReloads() {
        add_texts({"a"});
        ClipboardController controller(manager());
        controller.on_window_show();
        QCOMPARE(controller.current_items.size(), 1);
        controller.on_window_hide();
    }
};

QTEST_MAIN(ClipboardControllerTests)
#include "test_clipboard_controller.moc"
