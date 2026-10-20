#include "clipboard/controllers/clipboard_controller.h"
#include "clipboard/core/manager.h"
#include "clipboard/services/manage_dialog_service.h"
#include "clipboard/ui/dialogs/manage_dialog.h"
#include "clipboard/ui/theme/themes.h"
#include "clipboard/ui/windows/clipboard_window.h"
#include "core/logger.h"
#include "settings/special_paste.h"
#include "settings/tool_settings.h"
#include "clipboard/ui/widgets/reorder_list.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/cards.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/segmented.h"
#include "ui/settings_ui/dialog.h"
#include "ui/settings_ui/pages.h"
#include "ui/settings_ui/special_paste_dialog.h"
#include "ui/welcome/pages.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace jietuba;
using namespace jietuba::clipboard;

namespace {
// Clicks "Reset This Page" and answers Yes to its question.
void reset_page(QWidget &dialog) {
    QTimer::singleShot(0, [] {
        auto *modal = QApplication::activeModalWidget();
        for (auto *box : modal ? modal->findChildren<QDialogButtonBox *>() : QList<QDialogButtonBox *>())
            for (auto *button : box->buttons())
                if (box->buttonRole(button) == QDialogButtonBox::YesRole) button->click();
    });
    for (auto *button : dialog.findChildren<QPushButton *>())
        if (button->text() == settings_tr("Reset This Page")) button->click();
}

QStringList item_contents(const QList<ClipboardItem> &items) {
    QStringList contents;
    for (const auto &item : items) contents << item.content;
    return contents;
}
}

// The clipboard backend, its windows and its settings, on a database in a throwaway folder.
class ClipboardUiTests : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    ToolSettingsManager *config_ = nullptr;

private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        config_ = get_tool_settings_manager(new QSettings(dir_.filePath("settings.ini"), QSettings::IniFormat, this));
        config_->set_log_dir(dir_.filePath("logs"));
        config_->set_clipboard_db_path(QDir::toNativeSeparators(dir_.filePath("clipboard/clipboard.db")));
        // Saving checks global hotkeys against the system; the default may be taken here.
        config_->set_hotkey("");
        config_->set_clipboard_hotkey("");
        // Kept open after deactivation, which offscreen windows never get.
        config_->set_clipboard_close_after_paste(false);
        QVERIFY(ClipboardManager::instance().is_available());
    }

    void cleanup() {
        ClipboardManager::instance().clear_history(false);
        for (const auto &group : ClipboardManager::instance().get_groups()) ClipboardManager::instance().delete_group(group.id);
        QApplication::processEvents();
    }

    void itemsAndGroupsRoundTripThroughTheBackend() {
        auto &manager = ClipboardManager::instance();
        const auto first = manager.add_item("first text");
        const auto second = manager.add_item("second text", "text", QString("Title"));
        QVERIFY(first && second);
        QCOMPARE(manager.get_item(*second)->title.value_or(QString()), QString("Title"));
        QVERIFY(manager.update_item(*first, "first edited"));
        QCOMPARE(manager.get_item(*first)->content, QString("first edited"));
        QVERIFY(manager.toggle_pin(*first));
        QVERIFY(manager.get_item(*first)->is_pinned);
        // Pinned items come first.
        QCOMPARE(manager.get_history().first().id, *first);
        QCOMPARE(manager.get_total_count(), qint64(2));

        const auto group = manager.create_group("Work", std::nullopt, QString::fromUtf8("📁"));
        QVERIFY(group);
        QVERIFY(manager.rename_group(*group, "Office"));
        QCOMPARE(manager.get_groups().first().name, QString("Office"));
        QVERIFY(manager.move_to_group(*second, *group));
        QCOMPARE(item_contents(manager.get_by_group(*group)), QStringList{"second text"});
        // Clearing keeps grouped items when asked to.
        QVERIFY(manager.clear_history(true));
        QCOMPARE(item_contents(manager.get_by_group(*group)), QStringList{"second text"});
        QVERIFY(!manager.get_item(*first));
        QVERIFY(manager.delete_group(*group));
        QVERIFY(manager.get_groups().isEmpty());

        // The manage window's services save through the same backend.
        const auto saved_group = save_group(manager, std::nullopt, "Snippets", QString::fromUtf8("⭐"), 0);
        QVERIFY(saved_group.success);
        QCOMPARE(saved_group.action, QString("created"));
        QVERIFY(!save_group(manager, std::nullopt, "Snippets", std::nullopt, 0).success);
        const auto saved_text = save_text_content(manager, std::nullopt, *saved_group.entity_id, "hello", QString("greeting"));
        QVERIFY(saved_text.success);
        QCOMPARE(manager.get_by_group(*saved_group.entity_id).first().title.value_or(QString()), QString("greeting"));
    }

    void theControllerPagesSearchesAndBuildsMenus() {
        auto &manager = ClipboardManager::instance();
        for (int i = 0; i < 5; ++i) QVERIFY(manager.add_item(QString("entry %1").arg(i)));
        QVERIFY(manager.add_item("needle in text"));
        ClipboardController controller(manager);
        controller.load_history();
        QCOMPARE(controller.current_items.size(), 6);
        controller.set_search_text("needle");
        QCOMPARE(item_contents(controller.current_items), QStringList{"needle in text"});
        controller.set_search_text("");
        QCOMPARE(controller.current_items.size(), 6);
        const auto menu = controller.build_context_menu_data(controller.current_items.first().id);
        QVERIFY(menu);
        QCOMPARE(menu->actions.first().key, QString("paste"));
        QVERIFY(!controller.build_context_menu_data(999999));
    }

    void theWindowListsHistoryAndFollowsNewContent() {
        auto &manager = ClipboardManager::instance();
        QVERIFY(manager.add_item("older"));
        ClipboardWindow window;
        window.show();
        QTRY_COMPARE(window.list_widget->count(), 1);
        const auto added = manager.add_item("newer");
        QVERIFY(added);
        window.notify_new_content(manager.get_item(*added));
        QTRY_COMPARE(window.list_widget->count(), 2);
        // Searching narrows the list.
        window.search_input->setText("older");
        QTRY_COMPARE(window.list_widget->count(), 1);
        window.search_input->clear();
        QTRY_COMPARE(window.list_widget->count(), 2);
        window.close();
    }

    void theManageDialogOpensOnTheGroups() {
        auto &manager = ClipboardManager::instance();
        QVERIFY(manager.create_group("Alpha"));
        QVERIFY(manager.create_group("Beta"));
        auto *dialog = get_manage_dialog(&manager);
        QVERIFY(dialog);
        QCOMPARE(get_existing_manage_dialog(), dialog);
        dialog->show_and_activate();
        QTRY_COMPARE(dialog->group_list->count(), 2);
        dialog->switch_mode("import_export");
        QCOMPARE(dialog->current_mode, QString("import_export"));
        dialog->hide();
        destroy_manage_dialog();
        QVERIFY(!get_existing_manage_dialog());
    }

    void theSpecialPasteEditorEditsAndRestores() {
        SpecialPasteDialog editor(special_paste::default_entries());
        QCOMPARE(editor.entries(), special_paste::default_entries());
        auto checks = editor.findChildren<QCheckBox *>();
        QCOMPARE(checks.size(), special_paste::default_entries().size());
        checks.first()->setChecked(false);
        // Exactly one entry differs from the defaults (some options start off).
        const auto edited = editor.entries();
        int changed = 0;
        for (int i = 0; i < edited.size(); ++i) changed += edited[i] != special_paste::default_entries()[i] ? 1 : 0;
        QCOMPARE(changed, 1);
        editor.reset_button->click();
        QCOMPARE(editor.entries(), special_paste::default_entries());
    }

    void theSettingsPageAppliesAndResetsTheClipboard() {
        config_->set_clipboard_enabled(true);
        config_->set_clipboard_history_limit(100);
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        dialog.on_nav_changed(2, "clipboard");
        QVERIFY(!dialog.has_unsaved_changes());
        auto *enabled = dialog.attr<SwitchSettingCard>("clipboard_enabled_toggle");
        auto *limit = dialog.attr<QSpinBox>("clipboard_history_limit_spin");
        QVERIFY(enabled && limit);
        enabled->setChecked(false);
        limit->setValue(321);
        QVERIFY(dialog.has_unsaved_changes());
        // Special paste entries are edited elsewhere and saved like the controls.
        auto entries = special_paste::default_entries();
        entries.first().second = false;
        dialog.clipboard_special_paste_entries = entries;
        QVERIFY(dialog.apply_settings());
        QVERIFY(!config_->get_clipboard_enabled());
        QCOMPARE(config_->get_clipboard_history_limit(), qint64(321));
        QCOMPARE(special_paste::load(config_), entries);

        // The appearance page's clipboard section notifies the theme manager.
        dialog.on_nav_changed(3, "appearance");
        auto *font_combo = dialog.attr<QComboBox>("_clip_font_combo");
        QVERIFY(font_combo && font_combo->count() > 1);
        QSignalSpy font_changed(get_theme_manager(), &ThemeManager::font_size_changed);
        font_combo->setCurrentIndex(font_combo->currentIndex() == 0 ? 1 : 0);
        QVERIFY(dialog.apply_settings());
        QCOMPARE(font_changed.size(), 1);
        QCOMPARE(config_->get_clipboard_font_size(), font_combo->currentData().toLongLong());

        // Reset restores and saves the clipboard page's defaults.
        dialog.on_nav_changed(2, "clipboard");
        reset_page(dialog);
        QCOMPARE(enabled->isChecked(), config_->app_defaults().value("clipboard_enabled").toBool());
        QCOMPARE(limit->value(), config_->app_defaults().value("clipboard_history_limit").toInt());
        QCOMPARE(*dialog.clipboard_special_paste_entries, special_paste::default_entries());
        QCOMPARE(special_paste::load(config_), special_paste::default_entries());
        QCOMPARE(config_->get_clipboard_history_limit(), config_->app_defaults().value("clipboard_history_limit").toLongLong());
        dialog.hide();
    }

    void theWelcomePageAppliesAppearanceAtOnce() {
        ClipboardHotkeyPage page(config_);
        page.resize(900, 600);
        page.show();
        for (int ms : {0, 3000, 6500, 7300, 9500, 10300, 14000}) {
            page.set_animation_time(ms);
            QVERIFY(!page.grab().isNull());
        }
        auto *spin = page.findChild<QSpinBox *>();
        QVERIFY(spin);
        spin->setValue(77);
        QCOMPARE(config_->get_clipboard_history_limit(), qint64(77));
        QSignalSpy opacity_changed(get_theme_manager(), &ThemeManager::opacity_changed);
        QComboBox *opacity = nullptr;
        for (auto *combo : page.findChildren<QComboBox *>())
            if (combo->itemText(0) == settings_tr("Opaque")) opacity = combo;
        QVERIFY(opacity && opacity->count() > 1);
        opacity->setCurrentIndex((opacity->currentIndex() + 1) % opacity->count());
        QCOMPARE(opacity_changed.size(), 1);
        QCOMPARE(config_->get_clipboard_window_opacity(), opacity->currentData().toLongLong());
        page.save();
        QCOMPARE(config_->get_clipboard_history_limit(), qint64(77));
    }
};

QTEST_MAIN(ClipboardUiTests)
#include "test_clipboard_ui.moc"
