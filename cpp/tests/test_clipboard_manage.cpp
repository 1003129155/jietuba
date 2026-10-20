#include "clipboard_test_support.h"
#include "clipboard/services/file_payload_service.h"
#include "clipboard/services/group_service.h"
#include "clipboard/services/import_export_service.h"
#include "clipboard/ui/dialogs/manage_dialog.h"
#include "clipboard/ui/forms/file_content_form.h"
#include "clipboard/ui/forms/form_widgets.h"
#include "clipboard/ui/forms/group_icon_picker.h"
#include "clipboard/ui/forms/image_content_form.h"
#include "clipboard/ui/group_kind_style.h"
#include "clipboard/ui/widgets/manage_rows.h"
#include "clipboard/ui/widgets/reorder_list.h"
#include "core/i18n.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/segmented.h"
#include <QDragEnterEvent>
#include <QEvent>
#include <QFile>
#include <QMimeData>
#include <QKeyEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTest>

using namespace clipboard_test;

// Records the windows that get shown, to catch widgets made visible before they have a parent.
class WindowShowSpy : public QObject {
public:
    QStringList shown;
    bool eventFilter(QObject *object, QEvent *event) override {
        if (event->type() != QEvent::Show) return false;
        if (auto *widget = qobject_cast<QWidget *>(object); widget && widget->isWindow()) shown << widget->metaObject()->className();
        return false;
    }
};

namespace {
// Picks the entry whose data is `value` (empty for "any").
void choose(ComboBox *combo, const QString &value) {
    const int index = combo->findData(value);
    QVERIFY2(index >= 0, qPrintable(value));
    combo->setCurrentIndex(index);
}
}

// The clipboard manage window on a real database, offscreen: groups and items created,
// edited, reordered, moved and deleted through its controls, and CSV import/export.
class ClipboardManageTests : public QObject {
    Q_OBJECT
    Environment env_;
    ClipboardManager &manager() { return ClipboardManager::instance(); }

    ManageDialog *open_dialog() {
        auto *dialog = get_manage_dialog(&manager());
        dialog->show_and_activate();
        QApplication::processEvents();
        return dialog;
    }

    static QStringList row_texts(QListWidget *list) {
        QStringList texts;
        for (int row = 0; row < list->count(); ++row) texts << list->item(row)->text();
        return texts;
    }

    static QPoint row_center(QListWidget *list, int row) { return list->visualItemRect(list->item(row)).center(); }

    static bool menu_pick(QListWidget *list, int row, const QString &entry, QStringList *seen = nullptr) {
        MenuPicker picker(entry);
        emit list->customContextMenuRequested(row_center(list, row));
        if (seen) *seen = picker.seen;
        return picker.picked;
    }

private slots:
    void initTestCase() { QVERIFY(env_.init(this)); }

    void init() { QVERIFY(env_.fresh_database()); }

    void cleanup() {
        if (auto *dialog = get_existing_manage_dialog()) {
            dialog->hide();
            destroy_manage_dialog();
        }
        QApplication::processEvents();
    }

    void withoutGroupsItAsksForOne() {
        auto *dialog = open_dialog();
        QCOMPARE(dialog->group_list->count(), 0);
        QVERIFY(!dialog->add_item_btn->isEnabled());
        QVERIFY(!dialog->save_btn->isVisible());
        QVERIFY(!dialog->grab().isNull());
        // Saving content without a group is refused.
        ModalAnswerer ok(true);
        dialog->switch_page(1);
        QVERIFY(!dialog->save_btn->isVisible());
    }

    void groupIconsReflowWhenWindowResizes() {
        auto *dialog = open_dialog();
        dialog->new_group_btn->click();
        dialog->resize(1000, 760);
        QTRY_VERIFY(dialog->emoji_scroll->widget());
        QApplication::processEvents();
        auto *scroll = dialog->emoji_scroll.data();
        auto *container = scroll->widget();
        QTRY_COMPARE(container->findChildren<QPushButton *>().size(), dialog->emoji_groups.value(dialog->emoji_group_order[0]).size());
        const auto buttons = container->findChildren<QPushButton *>();
        QVERIFY(!buttons.isEmpty());
        const auto columns = [&buttons] {
            int result = 0;
            for (auto *button : buttons)
                if (button->y() == buttons.first()->y()) ++result;
            return result;
        };
        const int narrow_columns = columns();
        const int narrow_height = container->height();
        dialog->resize(1500, 760);
        QTRY_VERIFY(columns() > narrow_columns);
        QCOMPARE(scroll->widget(), container);
        QCOMPARE(container->findChildren<QPushButton *>(), buttons);
        QVERIFY(container->height() < narrow_height);
        QCOMPARE(container->width(), scroll->viewport()->width());

        dialog->resize(1500, 1400);
        QTRY_COMPARE(scroll->verticalScrollBar()->maximum(), 0);
        QTRY_COMPARE(container->width(), scroll->viewport()->width());

        dialog->resize(1000, 760);
        QTRY_COMPARE(columns(), narrow_columns);
        for (auto *button : buttons) QVERIFY(container->rect().contains(button->geometry()));
        QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        scroll->ensureWidgetVisible(buttons.last());
        QTRY_VERIFY(scroll->verticalScrollBar()->value() > 0);
        buttons.last()->click();
        QCOMPARE(dialog->icon_input->text(), buttons.last()->text());
        QCOMPARE(dialog->icon_preview->text(), buttons.last()->text());
    }

    void groupIconsLoadInBatchesAndCancelOldGroups() {
        auto *dialog = open_dialog();
        dialog->new_group_btn->click();
        dialog->emoji_tab_buttons[1]->click();
        auto *scroll = dialog->emoji_scroll.data();
        auto *container = scroll->widget();
        auto expected = dialog->emoji_groups.value(dialog->emoji_group_order[1]);
        QVERIFY(dialog->emoji_tab_buttons[1]->isChecked());
        QVERIFY(container->findChildren<QPushButton *>().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!container->findChildren<QPushButton *>().isEmpty(), 1000);
        const auto buttons = container->findChildren<QPushButton *>();
        QVERIFY(buttons.size() < expected.size());
        for (int i = 0; i < buttons.size(); ++i) {
            QCOMPARE(buttons[i]->text(), expected[i]);
            QVERIFY(!buttons[i]->autoDefault());
        }
        const int reserved_height = container->layout()->heightForWidth(container->width());
        buttons.first()->click();
        QCOMPARE(dialog->icon_input->text(), expected.first());
        QTRY_VERIFY(container->findChildren<QPushButton *>().size() > buttons.size());
        QCOMPARE(container->layout()->heightForWidth(container->width()), reserved_height);

        dialog->emoji_tab_buttons[2]->click();
        container = scroll->widget();
        expected = dialog->emoji_groups.value(dialog->emoji_group_order[2]);
        QVERIFY(container->findChildren<QPushButton *>().isEmpty());
        QTRY_COMPARE(container->findChildren<QPushButton *>().size(), expected.size());
        const auto replacement_buttons = container->findChildren<QPushButton *>();
        for (int i = 0; i < expected.size(); ++i) QCOMPARE(replacement_buttons[i]->text(), expected[i]);
        QVERIFY(dialog->icon_input->text() != expected.first());

        dialog->emoji_tab_buttons[1]->click();
        dialog->new_group_btn->click();
        container = dialog->emoji_scroll->widget();
        expected = dialog->emoji_groups.value(dialog->emoji_group_order[0]);
        QTRY_COMPARE(container->findChildren<QPushButton *>().size(), expected.size());
        const auto rebuilt_buttons = container->findChildren<QPushButton *>();
        for (int i = 0; i < expected.size(); ++i) QCOMPARE(rebuilt_buttons[i]->text(), expected[i]);
        QCOMPARE(dialog->icon_input->text(), GENERAL_GROUP_ICON());
    }

    void groupsAreCreatedEditedAndDeleted() {
        auto *dialog = open_dialog();
        QSignalSpy added(dialog, &ManageDialog::group_added);
        dialog->new_group_btn->click();
        QCOMPARE(dialog->current_mode, QString("group"));
        QVERIFY(dialog->group_name_input);
        {
            // A blank name is refused.
            ModalAnswerer ok(true);
            dialog->save_btn->click();
            QCOMPARE(ok.answered(), 1);
            QVERIFY(manager().get_groups().isEmpty());
        }
        dialog->group_name_input->setText("  Work  ");
        // The group type picks the default icon and the header.
        dialog->radio_file->click();
        QCOMPARE(dialog->icon_input->text(), QUICK_LAUNCH_GROUP_ICON());
        QVERIFY(dialog->detail_title->text().contains("Quick Launch"));
        dialog->radio_hidden->click();
        QCOMPARE(dialog->icon_input->text(), HIDDEN_GROUP_ICON());
        dialog->radio_normal->click();
        QCOMPARE(dialog->icon_input->text(), GENERAL_GROUP_ICON());
        // A custom icon survives switching to hidden and back.
        dialog->icon_input->setText(QString::fromUtf8("⭐"));
        dialog->radio_hidden->click();
        QCOMPARE(dialog->icon_input->text(), QString::fromUtf8("⭐"));
        dialog->radio_normal->click();
        QCOMPARE(dialog->icon_input->text(), QString::fromUtf8("⭐"));
        // Emoji tabs and preset icons fill the field; typed text keeps its first character.
        QVERIFY(dialog->emoji_tab_buttons.size() > 1);
        dialog->emoji_tab_buttons[1]->click();
        QVERIFY(dialog->emoji_tab_buttons[1]->isChecked());
        QTRY_VERIFY(!dialog->emoji_scroll->widget()->findChildren<QPushButton *>().isEmpty());
        QPushButton *preset = nullptr;
        for (auto *button : dialog->emoji_scroll->widget()->findChildren<QPushButton *>())
            if (!button->text().isEmpty()) preset = button;
        QVERIFY(preset);
        preset->click();
        QCOMPARE(dialog->icon_input->text(), preset->text());
        // Presets made of several code points stay whole, also past four UTF-16 units (🧔‍♂️, 👩‍❤️‍💋‍👨).
        QPushButton *joined = nullptr;
        for (int tab = 0; tab < dialog->emoji_tab_buttons.size() && !joined; ++tab) {
            dialog->emoji_tab_buttons[tab]->click();
            QTRY_COMPARE(dialog->emoji_scroll->widget()->findChildren<QPushButton *>().size(), dialog->emoji_groups.value(dialog->emoji_group_order[tab]).size());
            for (auto *button : dialog->emoji_scroll->widget()->findChildren<QPushButton *>())
                if (!joined && button->text().size() > 4) joined = button;
        }
        QVERIFY(joined);
        joined->click();
        QCOMPARE(dialog->icon_input->text(), joined->text());
        QCOMPARE(dialog->icon_preview->text(), joined->text());
        dialog->icon_input->setText(QString::fromUtf8("🧔‍♂️ extra"));
        QCOMPARE(dialog->icon_input->text(), QString::fromUtf8("🧔‍♂️"));
        dialog->save_btn->click();
        QCOMPARE(added.size(), 1);
        auto groups = manager().get_groups();
        QCOMPARE(groups.size(), 1);
        QCOMPARE(groups.first().name, QString("Work"));
        QCOMPARE(groups.first().icon.value_or(QString()), QString::fromUtf8("🧔‍♂️"));
        // The new group is selected with its new item form.
        QCOMPARE(dialog->selected_group_id, std::optional<qint64>(groups.first().id));
        QCOMPARE(dialog->current_mode, QString("content"));

        // A second group with the same name is refused.
        dialog->new_group_btn->click();
        dialog->group_name_input->setText("Work");
        {
            ModalAnswerer ok(true);
            dialog->save_btn->click();
            QCOMPARE(ok.answered(), 1);
            QVERIFY(ok.texts.first().contains("already exists"));
        }
        dialog->group_name_input->setText("Apps");
        dialog->radio_file->click();
        dialog->save_btn->click();
        groups = manager().get_groups();
        QCOMPARE(groups.size(), 2);
        QCOMPARE(groups.last().group_type, static_cast<int>(GroupType::FILE));
        QCOMPARE(row_texts(dialog->group_list), (QStringList{"Work", "Apps"}));

        // Editing renames and changes the type.
        dialog->group_list->setCurrentRow(0);
        dialog->edit_group_btn->click();
        QCOMPARE(dialog->editing_group_id, std::optional<qint64>(groups.first().id));
        dialog->group_name_input->setText("Office");
        dialog->radio_hidden->click();
        dialog->save_btn->click();
        QCOMPARE(manager().get_groups().first().name, QString("Office"));
        QCOMPARE(manager().get_groups().first().group_type, static_cast<int>(GroupType::HIDDEN));

        // The group menu edits and moves.
        QVERIFY(menu_pick(dialog->group_list, 1, "Edit Group"));
        QCOMPARE(dialog->editing_group_id, std::optional<qint64>(groups.last().id));
        QVERIFY(menu_pick(dialog->group_list, 1, "Move to Top"));
        QCOMPARE(manager().get_groups().first().id, groups.last().id);
        QVERIFY(menu_pick(dialog->group_list, 0, "Move to Bottom"));
        QCOMPARE(manager().get_groups().last().id, groups.last().id);
        // Alt+Up moves the current group too.
        dialog->group_list->setCurrentRow(1);
        QTest::keyClick(dialog->group_list, Qt::Key_Up, Qt::AltModifier);
        QCOMPARE(manager().get_groups().first().id, groups.last().id);
        emit dialog->group_list->itemDoubleClicked(dialog->group_list->item(0));
        QCOMPARE(dialog->editing_group_id, std::optional<qint64>(groups.last().id));

        // Delete asks first.
        {
            ModalAnswerer no(false);
            dialog->delete_btn->click();
            QCOMPARE(manager().get_groups().size(), 2);
            QVERIFY(no.texts.join(" ").contains("moved back to clipboard history"));
        }
        {
            ModalAnswerer yes(true);
            QVERIFY(menu_pick(dialog->group_list, 0, "Delete Group"));
            QCOMPARE(yes.answered(), 1);
        }
        QCOMPARE(manager().get_groups().size(), 1);
        QCOMPARE(dialog->group_list->count(), 1);
        QVERIFY(!dialog->grab().isNull());
    }

    void filterGroupsAreCreatedAndTakeNoContent() {
        auto *dialog = open_dialog();
        dialog->new_group_btn->click();
        dialog->group_name_input->setText("Images");
        QVERIFY(dialog->radio_filter->isEnabled());
        QVERIFY(dialog->filter_kind_box->isHidden());
        dialog->radio_filter->click();
        QVERIFY(!dialog->filter_kind_box->isHidden());
        QVERIFY(dialog->detail_title->text().contains("Filter"));
        const int filter_type = static_cast<int>(GroupType::FILTER);
        QCOMPARE(dialog->icon_input->text(), get_default_group_icon(filter_type, FilterRule::of_kind("text")));
        // The filtered type picks the default icon; a custom icon stays.
        choose(dialog->filter_kind_combo, "image");
        QCOMPARE(dialog->icon_input->text(), get_default_group_icon(filter_type, FilterRule::of_kind("image")));
        dialog->icon_input->setText(QString::fromUtf8("⭐"));
        choose(dialog->filter_kind_combo, "file");
        QCOMPARE(dialog->icon_input->text(), QString::fromUtf8("⭐"));
        choose(dialog->filter_kind_combo, "image");
        dialog->save_btn->click();
        QCOMPARE(manager().get_groups().size(), 1);
        QCOMPARE(manager().get_groups().first().group_type, filter_type);
        QCOMPARE(manager().get_groups().first().filter_rule, std::optional(FilterRule::of_kind("image")));
        // Its settings stay open after creation, with no content controls.
        QVERIFY(!dialog->add_item_btn->isEnabled());
        QVERIFY(!dialog->search_input->isEnabled());
        QVERIFY(dialog->add_item_btn->isHidden());
        QVERIFY(dialog->search_input->isHidden());
        QVERIFY(dialog->save_btn->isVisible());
        QCOMPARE(dialog->editing_group_id, dialog->selected_group_id);
        QVERIFY(dialog->filter_kind_box->isVisible());
        QCOMPARE(dialog->item_list->count(), 0);

        // A group with content cannot become a filter group; a filter group can be edited into any other.
        const auto full = manager().create_group("Full");
        QVERIFY(full);
        QVERIFY(manager().move_to_group(add_texts({"x"}).first(), *full));
        // A reopened window lists both groups.
        dialog->hide();
        destroy_manage_dialog();
        dialog = open_dialog();
        dialog->group_list->setCurrentRow(1);
        dialog->edit_group_btn->click();
        QCOMPARE(dialog->editing_group_id, full);
        // Not offered at all: the filter card is not shown, with no extra hint line.
        QVERIFY(!dialog->radio_filter->isEnabled());
        QVERIFY(dialog->radio_filter->isHidden());
        QVERIFY(!dialog->radio_normal->isHidden());
        dialog->group_list->setCurrentRow(0);
        dialog->edit_group_btn->click();
        QVERIFY(dialog->radio_filter->isChecked());
        QCOMPARE(dialog->filter_kind_combo->currentData().toString(), QString("image"));
        QCOMPARE(dialog->filter_app_combo->currentData().toString(), QString());
        dialog->radio_normal->click();
        dialog->save_btn->click();
        QCOMPARE(manager().get_groups().first().group_type, static_cast<int>(GroupType::NORMAL));
        QVERIFY(!dialog->grab().isNull());
    }

    void filterRulesPickAContentTypeAndASourceApp() {
        // The app has no items left in the history; the rule keeps it rather than becoming "any app".
        const FilterRule links_from_chrome{SUB_KIND_URL, QString("chrome.exe")};
        const auto group = manager().create_group("Links", std::nullopt, std::nullopt, static_cast<int>(GroupType::FILTER), links_from_chrome);
        QVERIFY(group);
        auto *dialog = open_dialog();
        dialog->group_list->setCurrentRow(0);
        QCOMPARE(dialog->current_mode, QString("group"));
        QCOMPARE(dialog->editing_group_id, group);
        QVERIFY(dialog->radio_filter->isChecked());
        QCOMPARE(dialog->filter_kind_combo->currentData().toString(), SUB_KIND_URL);
        QCOMPARE(dialog->filter_app_combo->currentData().toString(), QString("chrome.exe"));
        QCOMPARE(dialog->filter_app_combo->currentText(), QString("chrome"));
        QCOMPARE(dialog->icon_input->text(), QString::fromUtf8("🔗"));
        dialog->group_name_input->setText("Chrome links");
        dialog->save_btn->click();
        QCOMPARE(manager().get_groups().first().name, QString("Chrome links"));
        QCOMPARE(manager().get_groups().first().filter_rule, std::optional(links_from_chrome));

        // Each part changes on its own.
        dialog->edit_group_btn->click();
        choose(dialog->filter_kind_combo, "image");
        dialog->save_btn->click();
        QCOMPARE(manager().get_groups().first().filter_rule, std::optional(FilterRule{QString("image"), QString("chrome.exe")}));
        dialog->edit_group_btn->click();
        choose(dialog->filter_kind_combo, QString());
        QVERIFY(dialog->save_btn->isEnabled());
        // Neither part set would be the whole history: saving is off until one is chosen.
        choose(dialog->filter_app_combo, QString());
        QVERIFY(!dialog->save_btn->isEnabled());
        QVERIFY(!dialog->save_btn->toolTip().isEmpty());
        // Another group type does not need a rule.
        dialog->radio_normal->click();
        QVERIFY(dialog->save_btn->isEnabled());
        dialog->radio_filter->click();
        QVERIFY(!dialog->save_btn->isEnabled());
        choose(dialog->filter_app_combo, "chrome.exe");
        QVERIFY(dialog->save_btn->isEnabled());
        QCOMPARE(dialog->icon_input->text(), QString::fromUtf8("🧩"));
        dialog->save_btn->click();
        QCOMPARE(manager().get_groups().first().filter_rule, std::optional(FilterRule{std::nullopt, QString("chrome.exe")}));
        QVERIFY(!dialog->grab().isNull());
    }

    void selectingFilterGroupsOpensSettingsAndRestoresContentControlsElsewhere() {
        const auto notes = manager().create_group("Notes");
        const auto filter = manager().create_group("Colors", std::nullopt, std::nullopt, static_cast<int>(GroupType::FILTER), FilterRule::of_kind(SUB_KIND_COLOR));
        QVERIFY(notes && filter);
        auto *dialog = open_dialog();
        QCOMPARE(dialog->current_mode, QString("content"));
        QVERIFY(dialog->add_item_btn->isVisible());
        dialog->group_list->setCurrentRow(1);
        QCOMPARE(dialog->current_mode, QString("group"));
        QCOMPARE(dialog->editing_group_id, filter);
        QCOMPARE(dialog->group_name_input->text(), QString("Colors"));
        QCOMPARE(dialog->filter_kind_combo->currentData().toString(), SUB_KIND_COLOR);
        QVERIFY(dialog->filter_kind_box->isVisible());
        QVERIFY(dialog->add_item_btn->isHidden());
        QVERIFY(dialog->search_input->isHidden());
        choose(dialog->filter_kind_combo, SUB_KIND_URL);
        dialog->save_btn->click();
        QCOMPARE(manager().get_groups().last().filter_rule, std::optional(FilterRule::of_kind(SUB_KIND_URL)));
        QCOMPARE(dialog->editing_group_id, filter);
        dialog->switch_mode("content");
        QCOMPARE(dialog->current_mode, QString("group"));
        QCOMPARE(dialog->editing_group_id, filter);

        dialog->group_list->setCurrentRow(0);
        QCOMPARE(dialog->current_mode, QString("content"));
        QVERIFY(dialog->content_edit);
        QVERIFY(dialog->add_item_btn->isVisible() && dialog->add_item_btn->isEnabled());
        QVERIFY(dialog->search_input->isVisible() && dialog->search_input->isEnabled());
        dialog->group_list->setCurrentRow(1);
        dialog->new_group_btn->click();
        QVERIFY(!dialog->editing_group_id);
        QVERIFY(dialog->add_item_btn->isVisible() && !dialog->add_item_btn->isEnabled());
        QVERIFY(dialog->search_input->isVisible() && !dialog->search_input->isEnabled());
        dialog->group_list->setCurrentRow(1);
        QCOMPARE(dialog->editing_group_id, filter);
        QVERIFY(dialog->filter_kind_box->isVisible());
        QVERIFY(!dialog->grab().isNull());
    }

    void buildingTheGroupFormNeverFlashesAStrayWindow() {
        QVERIFY(manager().create_group("Images", std::nullopt, std::nullopt, static_cast<int>(GroupType::FILTER), FilterRule::of_kind("image")));
        auto *dialog = open_dialog();
        WindowShowSpy spy;
        qApp->installEventFilter(&spy);
        // Selecting and editing a filter group, then a new group's type cards.
        dialog->group_list->setCurrentRow(0);
        dialog->edit_group_btn->click();
        dialog->new_group_btn->click();
        dialog->radio_filter->click();
        dialog->radio_file->click();
        QApplication::processEvents();
        qApp->removeEventFilter(&spy);
        QVERIFY2(spy.shown.isEmpty(), qPrintable(spy.shown.join(", ")));
    }

    void textItemsAreAddedEditedSearchedAndDeleted() {
        const auto notes = manager().create_group("Notes");
        const auto other = manager().create_group("Other");
        QVERIFY(notes && other);
        auto *dialog = open_dialog();
        QCOMPARE(dialog->selected_group_id, std::optional<qint64>(*notes));
        QSignalSpy content_added(dialog, &ManageDialog::content_added);
        QVERIFY(dialog->content_edit);
        {
            ModalAnswerer ok(true);
            dialog->save_btn->click();
            QCOMPARE(ok.answered(), 1);
        }
        dialog->title_input->setText("greeting");
        dialog->content_edit->setPlainText("  hello world  ");
        dialog->save_btn->click();
        QVERIFY(!content_added.isEmpty());
        auto items = manager().get_by_group(*notes);
        QCOMPARE(items.size(), 1);
        QCOMPARE(items.first().content, QString("hello world"));
        QCOMPARE(items.first().title.value_or(QString()), QString("greeting"));
        // The form stays for the next entry, emptied.
        QVERIFY(dialog->content_edit->toPlainText().isEmpty());
        dialog->content_edit->setPlainText("second entry");
        dialog->save_btn->click();
        QCOMPARE(dialog->item_list->count(), 2);
        QVERIFY(dialog->list_count->text().contains("2"));

        // A click opens the editor; saving updates in place.
        emit dialog->item_list->itemClicked(dialog->item_list->item(0));
        QCOMPARE(dialog->editing_item_id, std::optional<qint64>(items.first().id));
        QVERIFY(dialog->detail_meta->text().contains(QString::number(QDate::currentDate().year())));
        dialog->content_edit->setPlainText("hello edited");
        dialog->save_btn->click();
        QCOMPARE(manager().get_item(items.first().id)->content, QString("hello edited"));

        // Searching filters rows and stops reordering.
        dialog->search_input->setText("SECOND");
        QVERIFY(dialog->item_list->isRowHidden(0));
        QVERIFY(!dialog->item_list->isRowHidden(1));
        QVERIFY(!dialog->item_list->reorder_enabled());
        dialog->search_input->setText("nothing like it");
        QCOMPARE(dialog->list_stack->currentWidget(), static_cast<QWidget *>(dialog->empty_label));
        dialog->search_input->clear();
        QVERIFY(dialog->item_list->reorder_enabled());

        // Reordering by keys and by the item menu.
        const auto before = ids_of(manager().get_by_group(*notes));
        dialog->item_list->setCurrentRow(0);
        QTest::keyClick(dialog->item_list, Qt::Key_Down, Qt::AltModifier);
        QCOMPARE(ids_of(manager().get_by_group(*notes)), (QList<qint64>{before[1], before[0]}));
        QVERIFY(menu_pick(dialog->item_list, 1, "Move to Top"));
        QCOMPARE(ids_of(manager().get_by_group(*notes)), before);
        QVERIFY(menu_pick(dialog->item_list, 0, "Move to Bottom"));
        QCOMPARE(ids_of(manager().get_by_group(*notes)), (QList<qint64>{before[1], before[0]}));
        QStringList seen;
        QVERIFY(menu_pick(dialog->item_list, 0, display_text_for_group("Other"), &seen));
        QCOMPARE(manager().get_by_group(*other).size(), 1);
        QCOMPARE(dialog->item_list->count(), 1);

        // Deleting from the editor asks first.
        emit dialog->item_list->itemClicked(dialog->item_list->item(0));
        {
            ModalAnswerer no(false);
            dialog->delete_btn->click();
            QCOMPARE(dialog->item_list->count(), 1);
        }
        {
            ModalAnswerer yes(true);
            QVERIFY(menu_pick(dialog->item_list, 0, "Delete"));
            QCOMPARE(yes.answered(), 1);
        }
        QCOMPARE(dialog->item_list->count(), 0);
        QCOMPARE(dialog->list_stack->currentWidget(), static_cast<QWidget *>(dialog->empty_label));
    }

    void fileAndImageItemsUseTheirForms() {
        const auto notes = manager().create_group("Notes");
        const auto apps = manager().create_group("Apps", std::nullopt, std::nullopt, static_cast<int>(GroupType::FILE));
        const QString path = QDir::toNativeSeparators(env_.dir.filePath("report.txt"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        auto *dialog = open_dialog();
        QCOMPARE(dialog->selected_group_id, std::optional<qint64>(*notes));
        // Switching the kind keeps the typed title.
        dialog->title_input->setText("kept title");
        dialog->content_kind_switch->setCurrentItem("file");
        QVERIFY(dialog->file_path_input);
        QCOMPARE(dialog->title_input->text(), QString("kept title"));
        {
            ModalAnswerer ok(true);
            dialog->save_btn->click();
            QCOMPARE(ok.answered(), 1);
        }
        {
            FileDialogAnswerer pick(path);
            dialog->on_browse_file();
            QVERIFY(pick.answered);
        }
        QCOMPARE(dialog->file_path_input->text(), path);
        dialog->save_btn->click();
        const auto files = manager().get_by_group(*notes);
        QCOMPARE(files.size(), 1);
        QCOMPARE(files.first().content_type, QString("file"));
        QCOMPARE(extract_first_file_path_from_content(files.first().content), path);
        QVERIFY(dialog->file_path_input->text().isEmpty());
        {
            FileDialogAnswerer pick(env_.dir.path());
            dialog->on_browse_folder();
            QVERIFY(pick.answered);
        }
        QCOMPARE(QDir::cleanPath(dialog->file_path_input->text()), QDir::cleanPath(env_.dir.path()));
        // Editing a file item shows its path.
        emit dialog->item_list->itemClicked(dialog->item_list->item(0));
        QCOMPARE(dialog->file_path_input->text(), path);
        dialog->set_selected_file_path(QDir::toNativeSeparators(env_.dir.filePath("other.txt")));
        dialog->save_btn->click();
        QVERIFY(extract_first_file_path_from_content(manager().get_by_group(*notes).first().content).endsWith("other.txt"));

        // The image kind needs an image first.
        dialog->add_item_btn->click();
        dialog->content_kind_switch->setCurrentItem("image");
        {
            ModalAnswerer ok(true);
            dialog->save_btn->click();
            QCOMPARE(ok.answered(), 1);
            QVERIFY(ok.texts.first().contains("image"));
        }
        {
            ModalAnswerer ok(true);
            dialog->use_clipboard_image();
        }
        QImage image(20, 10, QImage::Format_RGB32);
        image.fill(Qt::red);
        dialog->set_pending_image(image);
        QVERIFY(!dialog->grab().isNull());
        {
            FileDialogAnswerer cancel({});
            dialog->browse_image_file();
            QVERIFY(cancel.answered);
        }
        dialog->set_pending_image(QImage());

        // A titled image record: only its title is editable.
        const auto picture = manager().add_item("[20x10]", "image", QString("old title"));
        QVERIFY(manager().move_to_group(*picture, *notes));
        dialog->refresh_after_external_change();
        dialog->open_item_editor(*picture, *notes);
        QVERIFY(dialog->title_input);
        dialog->title_input->setText("new title");
        dialog->save_btn->click();
        QCOMPARE(manager().get_item(*picture)->title.value_or(QString()), QString("new title"));
        // Without stored pixels the image actions do nothing.
        dialog->copy_image_item();
        dialog->pin_image_item();
        dialog->save_image_item_as();

        // Quick launch provides file and link forms; images cannot be moved into it.
        dialog->group_list->setCurrentRow(dialog->group_list->row_of_id(*apps));
        QVERIFY(dialog->file_path_input);
        QVERIFY(dialog->content_kind_switch);
        dialog->group_list->setCurrentRow(dialog->group_list->row_of_id(*notes));
        QStringList seen;
        const int text_row = dialog->item_list->row_of_id(*picture);
        MenuPicker picker(display_text_for_group("Apps", true));
        emit dialog->item_list->customContextMenuRequested(row_center(dialog->item_list, text_row));
        QVERIFY(!picker.picked);
        QVERIFY(manager().get_by_group(*apps).isEmpty());
    }

    void quickLaunchCanCreateAndEditLinks() {
        const auto apps = manager().create_group("Launch", std::nullopt, std::nullopt, static_cast<int>(GroupType::FILE));
        QVERIFY(apps);
        auto *dialog = open_dialog();
        QVERIFY(dialog->file_path_input);
        QVERIFY(dialog->content_kind_switch);
        dialog->title_input->setText("Website");
        dialog->content_kind_switch->setCurrentItem("link");
        QVERIFY(dialog->content_edit);
        QCOMPARE(dialog->title_input->text(), QString("Website"));
        dialog->content_edit->setPlainText("ordinary text");
        {
            ModalAnswerer ok(true);
            dialog->save_btn->click();
            QCOMPARE(ok.answered(), 1);
        }
        QVERIFY(manager().get_by_group(*apps).isEmpty());
        QVERIFY(manager().get_history().isEmpty());
        dialog->content_edit->setPlainText("https://example.com");
        dialog->save_btn->click();
        const auto items = manager().get_by_group(*apps);
        QCOMPARE(items.size(), 1);
        QCOMPARE(items.first().sub_kind, SUB_KIND_URL);
        QCOMPARE(items.first().title, std::optional<QString>("Website"));
        dialog->open_item_editor(items.first().id, *apps);
        QVERIFY(dialog->content_edit);
        dialog->content_edit->setPlainText("www.example.com/updated");
        dialog->save_btn->click();
        QCOMPARE(manager().get_item(items.first().id)->content, QString("www.example.com/updated"));
        dialog->add_item_btn->click();
        dialog->content_kind_switch->setCurrentItem("file");
        QVERIFY(dialog->file_path_input);
    }

    void csvRoundTripsThroughTheWindow() {
        const auto notes = manager().create_group("Notes");
        const auto ids = add_texts({"alpha", "beta, with comma", "gamma\nlines"});
        for (qint64 id : ids) QVERIFY(manager().move_to_group(id, *notes));
        QVERIFY(manager().update_item(ids[0], "alpha", QString("A")));
        QVERIFY(manager().add_item(file_payload({"C:\\skip.txt"}), "file"));
        auto *dialog = open_dialog();
        dialog->import_export_btn->click();
        QCOMPARE(dialog->current_mode, QString("import_export"));
        QVERIFY(dialog->export_encoding_combo);
        QVERIFY(!dialog->grab().isNull());

        const QString csv = QDir::toNativeSeparators(env_.dir.filePath("export.csv"));
        // A repeated run (check.ps1 -Mode Leak) would be asked to replace the first run's file.
        QFile::remove(csv);
        {
            FileDialogAnswerer save(csv);
            ModalAnswerer ok(true);
            dialog->export_to_csv();
            QVERIFY(save.answered);
            QCOMPARE(ok.answered(), 1);
            QVERIFY(ok.texts.first().contains("3"));
        }
        const auto rows = read_import_rows(csv);
        QCOMPARE(rows.size(), 3);
        // Cancelling writes nothing.
        {
            FileDialogAnswerer cancel({});
            dialog->export_to_csv();
            QVERIFY(cancel.answered);
        }

        // Importing into a fresh library recreates the group and its texts.
        QVERIFY(env_.fresh_database());
        dialog->refresh_after_external_change();
        QSignalSpy added(dialog, &ManageDialog::group_added);
        {
            FileDialogAnswerer open(csv);
            ModalAnswerer ok(true);
            dialog->import_from_csv();
            QCOMPARE(ok.answered(), 1);
        }
        QCOMPARE(added.size(), 1);
        const auto groups = manager().get_groups();
        QCOMPARE(groups.size(), 1);
        QCOMPARE(groups.first().name, QString("Notes"));
        QCOMPARE(manager().get_by_group(groups.first().id).size(), 3);

        // A file without rows is refused.
        const QString empty = QDir::toNativeSeparators(env_.dir.filePath("empty.csv"));
        QFile empty_file(empty);
        QVERIFY(empty_file.open(QIODevice::WriteOnly));
        empty_file.write("group,content,title\n");
        empty_file.close();
        {
            FileDialogAnswerer open(empty);
            ModalAnswerer ok(true);
            dialog->import_from_csv();
            QCOMPARE(ok.answered(), 1);
            QVERIFY(ok.texts.first().contains("No valid data"));
        }
    }

    void hidingReleasesTheListsAndShowingReloadsThem() {
        const auto notes = manager().create_group("Notes");
        const auto ids = add_texts({"one"});
        QVERIFY(manager().move_to_group(ids[0], *notes));
        auto *dialog = open_dialog();
        QCOMPARE(dialog->item_list->count(), 1);
        dialog->hide();
        // Changed while hidden: the next show reloads.
        const auto more = add_texts({"two"});
        QVERIFY(manager().move_to_group(more[0], *notes));
        dialog->show_and_activate();
        QTRY_COMPARE(dialog->item_list->count(), 2);
        // A group deleted elsewhere leaves the editor.
        dialog->open_group_editor(*notes);
        QVERIFY(manager().delete_group(*notes));
        dialog->refresh_after_external_change(*notes);
        QVERIFY(!dialog->editing_group_id);
        QCOMPARE(dialog->group_list->count(), 0);
        // The cached window is reused, and destroying reports whether it was visible.
        QCOMPARE(get_manage_dialog(), dialog);
        QVERIFY(destroy_manage_dialog());
        QVERIFY(!get_existing_manage_dialog());
        QVERIFY(!destroy_manage_dialog());
    }

    void dropZonesTakeFilesAndImages() {
        const auto notes = manager().create_group("Notes");
        QVERIFY(notes);
        const QString text_file = QDir::toNativeSeparators(env_.dir.filePath("dropped.txt"));
        const QString png_file = QDir::toNativeSeparators(env_.dir.filePath("dropped.png"));
        QFile text(text_file);
        QVERIFY(text.open(QIODevice::WriteOnly));
        text.close();
        QImage picture(12, 9, QImage::Format_RGB32);
        picture.fill(Qt::blue);
        QVERIFY(picture.save(png_file));
        auto *dialog = open_dialog();
        // Qt hands a drop and a leave to the widget that accepted the enter; a leave ends the drag.
        auto send_drop = [](QWidget *zone, QMimeData *mime) {
            QDragEnterEvent passing(QPoint(5, 5), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(zone, &passing);
            QDragLeaveEvent leave;
            QApplication::sendEvent(zone, &leave);
            QDragEnterEvent enter(QPoint(5, 5), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(zone, &enter);
            const bool accepted = enter.isAccepted();
            QDragMoveEvent move(QPoint(6, 6), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(zone, &move);
            QDropEvent drop(QPointF(6, 6), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(zone, &drop);
            QDragLeaveEvent done;
            QApplication::sendEvent(zone, &done);
            return accepted;
        };

        dialog->content_kind_switch->setCurrentItem("file");
        QVERIFY(dialog->file_drop_zone);
        QMimeData not_files;
        not_files.setText("plain text");
        QVERIFY(!send_drop(dialog->file_drop_zone, &not_files));
        QMimeData files;
        files.setUrls({QUrl("https://example.com/x"), QUrl::fromLocalFile(text_file)});
        QVERIFY(send_drop(dialog->file_drop_zone, &files));
        QCOMPARE(dialog->file_path_input->text(), text_file);

        // New images go through clipboard history, so the zone takes nothing while recording is off.
        dialog->content_kind_switch->setCurrentItem("image");
        QVERIFY(dialog->image_drop_zone);
        QMimeData image_file;
        image_file.setUrls({QUrl::fromLocalFile(png_file)});
        QVERIFY(!send_drop(dialog->image_drop_zone, &image_file));
        manager().start_monitoring({});
        dialog->content_kind_switch->setCurrentItem("text");
        dialog->content_kind_switch->setCurrentItem("image");
        QVERIFY(dialog->image_drop_zone->isEnabled());
        QMimeData no_image;
        no_image.setUrls({QUrl::fromLocalFile(text_file)});
        QVERIFY(!send_drop(dialog->image_drop_zone, &no_image));
        QVERIFY(send_drop(dialog->image_drop_zone, &image_file));
        QVERIFY(!dialog->grab().isNull());
        QMimeData image_data;
        image_data.setImageData(picture);
        QVERIFY(send_drop(dialog->image_drop_zone, &image_data));
        QVERIFY(dialog->image_drop_zone->findChild<ImagePreview *>());
        manager().stop_monitoring();
    }

    void itemsDroppedOnAGroupMoveThere() {
        const auto notes = manager().create_group("Notes");
        const auto other = manager().create_group("Other");
        const auto apps = manager().create_group("Apps", std::nullopt, std::nullopt, static_cast<int>(GroupType::FILE));
        const auto ids = add_texts({"to move", "stays"});
        for (qint64 id : ids) QVERIFY(manager().move_to_group(id, *notes));
        auto *dialog = open_dialog();
        QCOMPARE(dialog->item_list->count(), 2);
        // While an item is dragged, the group list shows where it may go.
        dialog->item_list->setCurrentRow(0);
        emit dialog->item_list->drag_started();
        QVERIFY(dialog->group_list->is_row_droppable(dialog->group_list->row_of_id(*other)));
        QVERIFY(!dialog->group_list->is_row_droppable(dialog->group_list->row_of_id(*apps)));
        QVERIFY(!dialog->group_list->is_row_droppable(dialog->group_list->row_of_id(*notes)));
        QVERIFY(!dialog->grab().isNull());
        emit dialog->item_list->drag_finished();
        QVERIFY(dialog->group_list->is_row_droppable(0));
        // Text cannot go into a quick launch group; elsewhere it moves.
        emit dialog->group_list->item_dropped_on(ids[0], *apps);
        QTest::qWait(50);
        QVERIFY(manager().get_by_group(*apps).isEmpty());
        emit dialog->group_list->item_dropped_on(ids[0], *other);
        QTRY_COMPARE(ids_of(manager().get_by_group(*other)), QList<qint64>{ids[0]});
        QCOMPARE(dialog->item_list->count(), 1);
        QVERIFY(!dialog->group_list->item_id(dialog->group_list->count()));
        QVERIFY(!dialog->group_list->move_row(0, 0));
        QVERIFY(!dialog->group_list->move_row(5, 0));
    }

    void theDeleteGroupTextIsTranslated() {
        const char *moved = "Items in the group will be moved back to clipboard history.";
        QVERIFY(I18nManager::load_language("zh"));
        QCOMPARE(QCoreApplication::translate("ManageDialog", moved), QString::fromUtf8("分组内的内容会移回剪贴板历史。"));
        QCOMPARE(QCoreApplication::translate("ManageDialog", "Are you sure you want to delete this group?"), QString::fromUtf8("确定要删除这个分组吗？"));
        QVERIFY(I18nManager::load_language("ja"));
        QCOMPARE(QCoreApplication::translate("GroupBar", moved), QString::fromUtf8("グループ内のアイテムはクリップボード履歴に戻ります。"));
        QVERIFY(I18nManager::load_language("en"));
        QCOMPARE(QCoreApplication::translate("ClipboardWindow", moved), QString::fromUtf8(moved));
    }

    void theInterfaceThemeRestylesTheWindow() {
        manager().create_group("Notes");
        auto *dialog = open_dialog();
        auto *ui_theme = get_ui_theme();
        const QVariant original = ui_theme_mode_value(ui_theme->mode());
        for (const char *mode : {"dark", "light"}) {
            ui_theme->set_mode(QString(mode), false);
            QVERIFY(!dialog->grab().isNull());
        }
        ui_theme->set_mode(original, false);
        dialog->switch_mode("group");
        dialog->switch_mode("import_export");
        dialog->switch_mode("content");
        QVERIFY(!dialog->grab().isNull());
    }

    void typeCardsShareOneRowOnlyWhenEveryDescriptionFits() {
        QList<OptionCard *> cards = {new OptionCard(QString::fromUtf8("通用"), QString::fromUtf8("粘贴文字或文件"), KIND_NORMAL()),
                                     new OptionCard(QString::fromUtf8("快速启动"), QString::fromUtf8("打开文件或文件夹"), KIND_FILE()),
                                     new OptionCard(QString::fromUtf8("隐藏"), QString::fromUtf8("不在面板上显示"), KIND_HIDDEN()),
                                     new OptionCard(QString::fromUtf8("筛选"), QString::fromUtf8("只显示一类"), KIND_FILTER())};
        CardGrid grid(cards);
        // A widget that is not shown gets no resize events.
        grid.show();
        const int spacing = dialog_scaled(10);
        int need = 0;
        for (auto *card : cards) need = std::max(need, card->required_width());
        const int wide = 4 * need + 3 * spacing;
        const int card_height = cards[0]->sizeHint().height();

        grid.resize(wide, 10);
        QCOMPARE(grid.columns(grid.width()), 4);
        for (auto *card : cards) QCOMPARE(card->y(), 0);
        QCOMPARE(grid.height(), card_height);

        grid.resize(wide - 1, 10);
        QCOMPARE(grid.columns(grid.width()), 2);
        QSet<int> rows;
        for (auto *card : cards) {
            rows.insert(card->y());
            QVERIFY(card->x() + card->width() <= grid.width());
        }
        QCOMPARE(rows, (QSet<int>{0, card_height + spacing}));
        QCOMPARE(grid.height(), 2 * card_height + spacing);

        grid.resize(wide, 10);
        QCOMPARE(grid.height(), card_height);

        for (auto *card : cards) {
            QVERIFY(!card->grab().isNull());
            card->setChecked(true);
            QVERIFY(!card->grab().isNull());
            card->setEnabled(false);
            QVERIFY(!card->grab().isNull());
        }
    }

    void recommendedIconsAndPreviewFollowTheGroupType() {
        auto *dialog = open_dialog();
        dialog->new_group_btn->click();
        const auto recommended = [dialog] {
            QStringList icons;
            for (auto *button : dialog->recommended_row->findChildren<QPushButton *>()) icons << button->text();
            return icons;
        };
        auto *preview = qobject_cast<IconPreview *>(dialog->icon_preview.data());
        QVERIFY(preview);
        QCOMPARE(recommended(), get_recommended_group_icons(static_cast<int>(GroupType::NORMAL)));
        QCOMPARE(preview->kind(), KIND_NORMAL());

        dialog->radio_file->setChecked(true);
        QCOMPARE(recommended(), get_recommended_group_icons(static_cast<int>(GroupType::FILE)));
        QCOMPARE(preview->kind(), KIND_FILE());

        dialog->radio_filter->setChecked(true);
        choose(dialog->filter_kind_combo, "file");
        QCOMPARE(recommended(), get_recommended_group_icons(static_cast<int>(GroupType::FILTER), FilterRule::of_kind("file")));
        QCOMPARE(preview->kind(), KIND_FILTER());

        dialog->recommended_row->findChildren<QPushButton *>()[1]->click();
        QCOMPARE(dialog->icon_input->text(), get_recommended_group_icons(static_cast<int>(GroupType::FILTER), FilterRule::of_kind("file"))[1]);
        QCOMPARE(dialog->icon_preview->text(), dialog->icon_input->text());
    }

    void recommendedIconsHideInsteadOfOverflowing() {
        auto *dialog = open_dialog();
        dialog->new_group_btn->click();
        auto *row = dialog->recommended_row.data();
        const auto buttons = row->findChildren<QPushButton *>();
        QVERIFY(!buttons.isEmpty());

        row->setFixedWidth(10000);
        QApplication::processEvents();
        for (auto *button : buttons) QVERIFY(button->isVisibleTo(row));
        row->setFixedWidth(buttons.first()->width() * 3);
        QApplication::processEvents();
        int visible = 0;
        for (auto *button : buttons) {
            if (!button->isVisibleTo(row)) continue;
            ++visible;
            QVERIFY(button->geometry().right() <= row->width());
        }
        QVERIFY(visible > 0 && visible < buttons.size());
    }

    void groupRowsCarryTheGroupKindAndTheNewGroupButtonIsAnAddRow() {
        QVERIFY(manager().create_group("Plain"));
        QVERIFY(manager().create_group("Launch", std::nullopt, std::nullopt, static_cast<int>(GroupType::FILE)));
        QVERIFY(manager().create_group("Secret", std::nullopt, std::nullopt, static_cast<int>(GroupType::HIDDEN)));
        QVERIFY(manager().create_group("Pictures", std::nullopt, std::nullopt, static_cast<int>(GroupType::FILTER), FilterRule::of_kind("image")));
        auto *dialog = open_dialog();
        QStringList kinds;
        for (int row = 0; row < dialog->group_list->count(); ++row) kinds << dialog->group_list->item(row)->data(KIND_ROLE).toString();
        QCOMPARE(kinds, (QStringList{"normal", "file", "hidden", "filter"}));
        QVERIFY(!dialog->group_list->viewport()->grab().isNull());

        QCOMPARE(dialog->new_group_btn->objectName(), QString("ManageAddButton"));
        QCOMPARE(dialog->new_group_btn->objectName(), dialog->add_item_btn->objectName());
        QCOMPARE(dialog->new_group_btn->text(), "+  " + ManageDialog::tr_text("New Group"));
        QVERIFY(dialog->new_group_btn->width() > dialog->sidebar->width() / 2);
    }

    void newGroupFormHighlightsNoGroupUntilOneIsChosen() {
        const auto notes = manager().create_group("Notes");
        const auto other = manager().create_group("Other");
        QVERIFY(notes && other);
        auto *dialog = open_dialog();
        dialog->new_group_btn->click();
        QCOMPARE(dialog->group_list->currentRow(), -1);
        QVERIFY(dialog->group_list->selectedItems().isEmpty());
        QVERIFY(dialog->new_group_btn->isChecked());
        QCOMPARE(dialog->list_title->text(), ManageDialog::tr_text("New Group"));
        QCOMPARE(dialog->empty_label->text(), ManageDialog::tr_text("Save the new group first, then add content to it"));
        QVERIFY(!dialog->add_item_btn->isEnabled());
        QVERIFY(!dialog->search_input->isEnabled());
        QVERIFY(!dialog->edit_group_btn->isVisible());
        // A checkable button toggles itself on click first; clicking it again keeps it checked.
        dialog->new_group_btn->click();
        QVERIFY(dialog->new_group_btn->isChecked());

        dialog->group_list->setCurrentRow(1);
        QCOMPARE(dialog->selected_group_id, other);
        QCOMPARE(dialog->current_mode, QString("content"));
        QVERIFY(!dialog->new_group_btn->isChecked());
        QVERIFY(dialog->list_title->text().endsWith("Other"));

        // Switching forms from outside brings the previous group back.
        dialog->new_group_btn->click();
        dialog->switch_mode("content");
        QCOMPARE(dialog->group_list->currentRow(), 1);
        QVERIFY(dialog->list_title->text().endsWith("Other"));

        // Saving selects the new group.
        dialog->new_group_btn->click();
        dialog->group_name_input->setText("Fresh");
        dialog->save_btn->click();
        QVERIFY(dialog->selected_group_id);
        QCOMPARE(dialog->group_list->currentItem()->text(), QString("Fresh"));
        QVERIFY(!dialog->new_group_btn->isChecked());
        QVERIFY(dialog->list_title->text().endsWith("Fresh"));
    }

    void editFormOfAGroupWithContentIsAsWideAsTheNewForm() {
        // Wrapped text in the form must not make the scroll area lay it out at preferred heights and squeeze it.
        const auto full = manager().create_group("Full");
        QVERIFY(full);
        QVERIFY(manager().move_to_group(add_texts({"x"}).first(), *full));
        auto *dialog = open_dialog();
        dialog->resize(1208, 799);
        QApplication::processEvents();
        auto *scroll = qobject_cast<QScrollArea *>(dialog->detail_content->parentWidget()->parentWidget());
        QVERIFY(scroll);
        dialog->new_group_btn->click();
        QApplication::processEvents();
        const int new_width = dialog->detail_content->width();
        dialog->open_group_editor(*full);
        QApplication::processEvents();
        QCOMPARE(dialog->detail_content->width(), new_width);
        QVERIFY(!scroll->verticalScrollBar()->isVisible());
    }

    void hintLabelsDoNotReportHeightForWidth() {
        QWidget parent;
        auto *layout = new QVBoxLayout(&parent);
        QLabel *label = field_hint(QString::fromUtf8("一段会换行的说明文字").repeated(8));
        layout->addWidget(label);
        layout->addStretch();
        QVERIFY(!label->hasHeightForWidth());
        parent.resize(160, 600);
        parent.show();
        QTRY_COMPARE(label->height(), label->heightForWidth(label->width()));
        const int narrow = label->height();
        parent.resize(1200, 600);
        QTRY_VERIFY(label->width() > 1000 && label->height() == label->heightForWidth(label->width()));
        QVERIFY(label->height() < narrow);
        parent.resize(160, 600);
        QTRY_COMPARE(label->height(), narrow);
    }

    void aHiddenCardTakesNoCell() {
        QList<OptionCard *> cards = {new OptionCard(QString::fromUtf8("通用"), QString::fromUtf8("粘贴文字或文件"), KIND_NORMAL()),
                                     new OptionCard(QString::fromUtf8("快速启动"), QString::fromUtf8("打开文件或文件夹"), KIND_FILE()),
                                     new OptionCard(QString::fromUtf8("隐藏"), QString::fromUtf8("不在面板上显示"), KIND_HIDDEN()),
                                     new OptionCard(QString::fromUtf8("筛选"), QString::fromUtf8("只显示一类"), KIND_FILTER())};
        cards[3]->hide();
        CardGrid grid(cards);
        grid.show();
        const int spacing = dialog_scaled(10);
        int need = 0;
        for (int index = 0; index < 3; ++index) need = std::max(need, cards[index]->required_width());
        const int wide = 3 * need + 2 * spacing;
        grid.resize(wide, 10);
        QCOMPARE(grid.columns(grid.width()), 3);
        for (int index = 0; index < 3; ++index) QCOMPARE(cards[index]->y(), 0);
        QCOMPARE(cards[2]->x() + cards[2]->width(), grid.width());
        grid.resize(wide - 1, 10);
        QCOMPARE(grid.columns(grid.width()), 2);
        QCOMPARE(grid.height(), 2 * cards[0]->sizeHint().height() + spacing);
    }

    void defaultSizeFollowsTheScreenAndTheUsersSizeIsKept() {
        get_dialog_scale().set_percent(100);
        auto *dialog = get_manage_dialog(&manager());
        // Opened first on a screen too small for the minimum: raising the minimum on a larger screen is not a drag.
        dialog->fit_to_available(QSize(800, 500));
        QCOMPARE(dialog->size(), QSize(800, 500));
        dialog->fit_to_available(QSize(1920, 1032));
        QCOMPARE(dialog->size(), QSize(1200, 760));
        dialog->fit_to_available(QSize(2560, 1392));
        QCOMPARE(dialog->size(), QSize(1280, 900));
        dialog->fit_to_available(QSize(1366, 728));
        QCOMPARE(dialog->size(), QSize(1200, 641));

        // Neither the window nor its minimum may exceed a small screen; the columns scroll the rest.
        dialog->fit_to_available(QSize(1093, 574));
        QCOMPARE(dialog->size(), QSize(1093, 574));
        QCOMPARE(dialog->minimumSize(), QSize(1000, 420));
        dialog->fit_to_available(QSize(800, 500));
        QCOMPARE(dialog->size(), QSize(800, 500));
        QCOMPARE(dialog->minimumSize(), QSize(800, 420));

        dialog->fit_to_available(QSize(1920, 1032));
        dialog->resize(1300, 800);
        dialog->fit_to_available(QSize(2560, 1392));
        QCOMPARE(dialog->size(), QSize(1300, 800));
        dialog->fit_to_available(QSize(1280, 672));
        QCOMPARE(dialog->size(), QSize(1280, 672));
    }


private:
    // The item menu lists groups as their icon, two spaces and the name.
    QString display_text_for_group(const QString &name, bool file_group = false) const {
        return (file_group ? QUICK_LAUNCH_GROUP_ICON() : GENERAL_GROUP_ICON()) + "  " + name;
    }
};

QTEST_MAIN(ClipboardManageTests)
#include "test_clipboard_manage.moc"
