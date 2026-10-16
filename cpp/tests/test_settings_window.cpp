#include "core/i18n.h"
#include "settings/settings_transfer.h"
#include "settings/tool_settings.h"
#include "ui/fluent_lite/cards.h"
#include "ui/hotkey_edit.h"
#include "ui/inapp_key_edit.h"
#include "ui/settings_ui/dialog.h"
#include "ui/settings_ui/mouse_binding_editor.h"
#include "ui/settings_ui/pages.h"
#include "ui/welcome/pages.h"
#include "ui/welcome/wizard.h"
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QMenu>
#include <QSignalSpy>
#include <QTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTest>

using namespace jietuba;

namespace {
QPushButton *apply_button(SettingsDialog &dialog) {
    for (auto *button : dialog.findChildren<QPushButton *>())
        if (button->text() == settings_tr("Apply")) return button;
    return nullptr;
}

std::vector<HotkeyEdit *> slots_of(SettingsDialog &dialog, const QString &first_attr) {
    std::vector<HotkeyEdit *> edits;
    for (const auto &attr : global_hotkey_edit_attrs(first_attr)) edits.push_back(dialog.attr<HotkeyEdit>(attr));
    return edits;
}

QStringList texts_of(const std::vector<HotkeyEdit *> &edits) {
    QStringList texts;
    for (auto *edit : edits) texts.append(edit ? edit->text() : QString("<missing>"));
    return texts;
}
}

// Records the URLs the dialog asks the desktop to open.
class UrlCatcher : public QObject {
    Q_OBJECT
public:
    QList<QUrl> urls;
public slots:
    void open(const QUrl &url) { urls << url; }
};

class SettingsWindowTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir dir_;
    QSettings *settings_ = nullptr;
    ToolSettingsManager *config_ = nullptr;
private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        // The settings singleton reads a throwaway file, never the registry. It binds once per
        // process, so a repeated run (check.ps1 -Mode Leak) keeps the first file.
        if (!settings_) settings_ = new QSettings(dir_.filePath("settings.ini"), QSettings::IniFormat, this);
        config_ = get_tool_settings_manager(settings_);
        QCOMPARE(&config_->settings(), settings_);
        // Applying checks global hotkeys against the system, where another program may hold
        // the defaults (Ctrl+1, Ctrl+2); a rarely used one keeps these tests independent of it.
        config_->set_hotkey("ctrl+alt+shift+f10");
        config_->set_clipboard_hotkey("");
    }

    void everyPageBuilds() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.build_all_pages();
        for (int index = 0; index < 12; ++index) QVERIFY2(qobject_cast<QScrollArea *>(dialog.content_stack->widget(index)), qPrintable(QString::number(index)));
        for (const char *name : {"hotkey_input", "save_toggle", "clipboard_hotkey_edit", "pin_clipboard_hotkey_edit", "clipboard_pick_combo", "clipboard_enabled_toggle",
                                 "clipboard_history_limit_spin", "special_paste_manage_btn", "_clip_theme_btn", "_clip_font_combo", "_clip_opacity_combo",
                                 "preload_clipboard_toggle"})
            QVERIFY2(dialog.has_attr(name), name);
    }

    void applyFollowsChangesAndWritesConfig() {
        config_->set_screenshot_save_enabled(false);
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        dialog.on_nav_changed(1, "capture");
        auto *apply = apply_button(dialog);
        QVERIFY(apply);
        QVERIFY(!apply->isEnabled());
        auto *toggle = dialog.attr<SwitchSettingCard>("save_toggle");
        QVERIFY(toggle);
        QVERIFY(!toggle->isChecked());
        toggle->setChecked(true);
        QVERIFY(apply->isEnabled());
        QVERIFY(dialog.apply_settings());
        QVERIFY(config_->get_screenshot_save_enabled());
        QVERIFY(!apply->isEnabled());
        dialog.hide();
    }

    void resetRestoresTheCurrentPageDefaults() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.on_nav_changed(1, "capture");
        auto *toggle = dialog.attr<SwitchSettingCard>("magnifier_grid_toggle");
        const bool default_value = config_->app_defaults().value("magnifier_grid").toBool();
        toggle->setChecked(!default_value);
        for (auto *button : dialog.findChildren<QPushButton *>())
            if (button->text() == settings_tr("Reset This Page")) button->click();
        QCOMPARE(toggle->isChecked(), default_value);
    }

    void captureEngineComboLoadsSavesAndResets() {
        config_->set_capture_engine("hdr");
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        dialog.on_nav_changed(1, "capture");
        auto *combo = dialog.attr<QComboBox>("capture_engine_combo");
        QVERIFY(combo);
        QStringList engines;
        for (int i = 0; i < combo->count(); ++i) engines << combo->itemData(i).toString();
        QCOMPARE(engines, CAPTURE_ENGINE_CHOICES);
        QCOMPARE(combo->currentData().toString(), QString("hdr"));
        combo->setCurrentIndex(combo->findData(QString("mss")));
        QVERIFY(dialog.apply_settings());
        QCOMPARE(config_->get_capture_engine(), QString("mss"));
        for (auto *button : dialog.findChildren<QPushButton *>())
            if (button->text() == settings_tr("Reset This Page")) button->click();
        QCOMPARE(combo->currentData().toString(), config_->app_defaults().value("capture_engine").toString());
        dialog.hide();
    }

    void duplicateGlobalHotkeysAreFlagged() {
        SettingsDialog dialog(config_, "ctrl+shift+a");
        auto *main = dialog.attr<HotkeyEdit>("hotkey_input");
        auto *alternate = dialog.attr<HotkeyEdit>("hotkey_input_2");
        main->setText("ctrl+shift+q");
        alternate->setText("ctrl+shift+q");
        QVERIFY(!validate_global_hotkey_edits(&dialog));
        QVERIFY(!alternate->validation_error().isEmpty());
        alternate->setText("");
        QVERIFY(validate_global_hotkey_edits(&dialog));
    }

    // Four slots per function; the old Win+V switch shows as the third clipboard slot.
    void globalHotkeysHaveFourSlotsInTwoColumns() {
        config_->settings().remove("clipboard/hotkey_3");
        config_->set_app_setting("clipboard_take_over_win_v", true);
        SettingsDialog dialog(config_, config_->get_hotkey());
        QCOMPARE(texts_of(slots_of(dialog, "clipboard_hotkey_edit")), QStringList({"", "", "win+v", ""}));
        for (const char *first : {"hotkey_input", "pin_clipboard_hotkey_edit", "translation_hotkey_edit"})
            QVERIFY2(!texts_of(slots_of(dialog, first)).contains("<missing>"), first);
        // Column by column, so the first two slots keep their old place.
        const auto edits = slots_of(dialog, "hotkey_input");
        QGridLayout *grid = nullptr;
        auto *row = edits[0]->parentWidget()->layout();
        for (int i = 0; i < row->count() && !grid; ++i) grid = qobject_cast<QGridLayout *>(row->itemAt(i)->layout());
        QVERIFY(grid);
        QList<QPair<int, int>> positions;
        for (auto *edit : edits) {
            int r = 0, c = 0, rs = 0, cs = 0;
            grid->getItemPosition(grid->indexOf(edit), &r, &c, &rs, &cs);
            positions.append({r, c});
        }
        QCOMPARE(positions, (QList<QPair<int, int>>{{0, 0}, {1, 0}, {0, 1}, {1, 1}}));
    }

    void applySavesEverySlotAndKeepsTheOldSwitchInStep() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        QVERIFY(!dialog.has_unsaved_changes());
        dialog.attr<HotkeyEdit>("translation_hotkey_edit_4")->setText("ctrl+alt+shift+f7");
        QVERIFY(dialog.has_unsaved_changes());
        dialog.attr<HotkeyEdit>("clipboard_hotkey_edit_3")->setText("");
        QVERIFY(dialog.apply_settings());
        QCOMPARE(config_->get_global_hotkeys("translation"), QStringList({"", "", "", "ctrl+alt+shift+f7"}));
        QCOMPARE(config_->get_global_hotkeys("clipboard")[2], QString());
        QVERIFY(!config_->get_app_setting("clipboard_take_over_win_v").toBool());
        // Reset puts Win+V back in the third clipboard slot.
        for (auto *button : dialog.findChildren<QPushButton *>())
            if (button->text() == settings_tr("Reset This Page")) button->click();
        QCOMPARE(dialog.attr<HotkeyEdit>("clipboard_hotkey_edit_3")->text(), QString("win+v"));
        config_->set_global_hotkeys("translation", {"", "", "", ""});
        config_->set_hotkey("ctrl+alt+shift+f10");
        config_->set_global_hotkeys("clipboard", {"", "", "", ""});
    }

    void aDuplicateInTheNewSlotsBlocksApply() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.attr<HotkeyEdit>("clipboard_hotkey_edit_3")->setText("win+v");
        dialog.attr<HotkeyEdit>("hotkey_input_4")->setText("win+v");
        QTimer::singleShot(0, &dialog, [] {
            if (auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget())) modal->reject();
        });
        QVERIFY(!dialog.apply_settings());
        QCOMPARE(config_->get_global_hotkeys("screenshot")[3], QString());
    }

    // The status icon tells how a value registers; single keys are recorded only when they bind alone.
    void hotkeyEditorShowsHowAValueRegisters() {
        const auto tip = [](const char *text) { return i18n_tr(text, "HotkeyEdit"); };
        HotkeyEdit edit;
        edit.setText("win+l");
        QCOMPARE(edit.status_state(), QString("error"));
        QCOMPARE(edit.status_icon->toolTip(), tip("Reserved by Windows. No app can use it."));
        QVERIFY(!edit.validate_now());
        edit.setText("ctrl+!");
        QCOMPARE(edit.status_icon->toolTip(), tip("This key can't be used as a global hotkey."));
        edit.setText("print");
        QCOMPARE(edit.status_state(), QString("ok"));
        QVERIFY(edit.taken_over());
        QCOMPARE(edit.edit->toolTip(), tip("Windows uses this shortcut too. This app takes it over while running; clear it to give it back."));
        edit.setText("ctrl+alt+shift+home");
        QVERIFY(!edit.taken_over());
        QKeyEvent unknown(QEvent::KeyPress, Qt::Key_unknown, Qt::MetaModifier);
        QApplication::sendEvent(edit.edit, &unknown);
        QCOMPARE(edit.text(), QString("ctrl+alt+shift+home"));
        QKeyEvent home(QEvent::KeyPress, Qt::Key_Home, Qt::NoModifier);
        QApplication::sendEvent(edit.edit, &home);
        QCOMPARE(edit.text(), QString("home"));
        for (int key : {Qt::Key_A, Qt::Key_Space, Qt::Key_Escape}) {
            QKeyEvent typing(QEvent::KeyPress, key, Qt::NoModifier);
            QApplication::sendEvent(edit.edit, &typing);
            QCOMPARE(edit.text(), QString("home"));
        }
    }

    // Until the third clipboard slot is saved it follows the old switch, without writing.
    void winVMigrationFollowsTheOldSwitch() {
        // A repeated run (check.ps1 -Mode Leak) would find the slots the first run saved.
        QFile::remove(dir_.filePath("migration.ini"));
        QSettings settings(dir_.filePath("migration.ini"), QSettings::IniFormat);
        ToolSettingsManager config(settings);
        QCOMPARE(config.get_global_hotkeys("clipboard"), QStringList({"ctrl+2", "", "win+v", ""}));
        QVERIFY(!settings.contains("clipboard/hotkey_3"));
        config.set_app_setting("clipboard_take_over_win_v", false);
        QCOMPARE(config.get_global_hotkeys("clipboard")[2], QString());
        // Win+V already in another slot is not doubled.
        config.set_app_setting("clipboard_take_over_win_v", true);
        settings.setValue("app/hotkey_2", "Win + V");
        QCOMPARE(config.get_global_hotkeys("clipboard")[2], QString());
        settings.remove("app/hotkey_2");
        // Once saved, the slot wins over the switch.
        config.set_global_hotkeys("clipboard", {"ctrl+2", "", "", ""});
        QVERIFY(!config.get_app_setting("clipboard_take_over_win_v").toBool());
        config.set_app_setting("clipboard_take_over_win_v", true);
        QCOMPARE(config.get_global_hotkeys("clipboard")[2], QString());
        config.set_global_hotkeys("clipboard", {"win+v", "", "", ""});
        QVERIFY(config.get_app_setting("clipboard_take_over_win_v").toBool());
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, config.set_global_hotkeys("clipboard", {"ctrl+2"}));
    }

    void duplicateMouseBindingsAreListed() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.on_nav_changed(10, "mouse");
        auto *first = qobject_cast<MouseBindingEditor *>(dialog.behavior_control("mouse_pin_zoom"));
        auto *second = qobject_cast<MouseBindingEditor *>(dialog.behavior_control("mouse_pin_opacity"));
        QVERIFY(first && second);
        first->setBinding("ctrl+wheel");
        second->setBinding("ctrl+wheel");
        const auto conflicts = mouse_binding_conflicts(&dialog);
        QCOMPARE(conflicts.size(), size_t(1));
        QCOMPARE(std::get<2>(conflicts.front()).size(), 2);
    }

    // Export writes what the pages read; import fills the controls without saving.
    void exportAndImportRoundTrip() {
        config_->set_log_level("ERROR");
        config_->set_screenshot_save_path(dir_.path());
        SettingsDialog dialog(config_, config_->get_hotkey());
        const auto keys = dialog.settings_keys();
        QVERIFY(keys.count("app/hotkey"));
        QVERIFY(keys.count("app/screenshot_toolbar_layout"));
        QVERIFY(keys.count("pin/toolbar_layout"));
        // Settings no page shows stay out, as in the reference: they could be exported but never imported.
        for (const char *key : {"clipboard/show_metadata", "clipboard/preserve_search", "gif/fps", "app/screenshot_quality"})
            QVERIFY2(!keys.count(key), key);
        {
            // A provider field reads its own setting only.
            settings_transfer::RecordingReads recording(config_);
            QVERIFY(config_->get_provider_field("deepl_api_key"));
            QCOMPARE(recording.keys.size(), 1);
        }
        const QString path = dir_.filePath("export.json");
        settings_transfer::export_settings(config_, QSet<QString>(keys.begin(), keys.end()), path, "test");
        auto values = settings_transfer::read_settings_file(path);
        QCOMPARE(values.value("app/log_level").toString(), QString("ERROR"));
        // Machine-local keys are never exported.
        QVERIFY(!values.contains("app/screenshot_save_path"));
        values["app/log_level"] = "DEBUG";
        dialog.load_imported_settings(values);
        QCOMPARE(dialog.attr<QComboBox>("log_level_combo")->currentText(), QString("DEBUG"));
        QVERIFY(dialog.has_unsaved_changes());
        // Nothing is written until Apply.
        QCOMPARE(config_->get_log_level(), QString("ERROR"));

        QFile broken(dir_.filePath("broken.json"));
        QVERIFY(broken.open(QIODevice::WriteOnly));
        broken.write("{\"format\": \"other\"}");
        broken.close();
        QVERIFY_THROWS_EXCEPTION(settings_transfer::SettingsFileError, settings_transfer::read_settings_file(broken.fileName()));
    }

    // No page has a control for the toolbar layouts; imported ones are written on Apply, each to its own key.
    void importedToolbarLayoutsWaitForApply() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        const QString screenshot_layout = R"([["pin", "show"], ["text", "hide"]])";
        const QString pin_layout = R"([["text", "show"], ["pen", "more"]])";
        // Keys missing from a file fall back to defaults, and another program may hold the default hotkeys.
        dialog.load_imported_settings({{"app/screenshot_toolbar_layout", screenshot_layout},
                                       {"pin/toolbar_layout", pin_layout},
                                       {"app/hotkey", config_->get_hotkey()},
                                       {"clipboard/hotkey", QString()}});
        QCOMPARE(config_->get_app_setting("pin_toolbar_layout").toString(), QString());
        QVERIFY(dialog.apply_settings());
        QCOMPARE(config_->get_app_setting("screenshot_toolbar_layout").toString(), screenshot_layout);
        QCOMPARE(config_->get_app_setting("pin_toolbar_layout").toString(), pin_layout);
        QVERIFY(config_->settings().contains("pin/toolbar_layout"));
    }

    // Test Connection and Fetch Models use the form's unsaved values.
    void pendingProviderValuesOverlayTheConfig() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.on_nav_changed(4, "translation");
        QVERIFY(dialog.provider_field_widgets);
        auto *edit = qobject_cast<QLineEdit *>(dialog.provider_field_widgets->value("deepl_api_key").data());
        QVERIFY(edit);
        edit->setText("  unsaved-key  ");
        const auto fields = provider_fields::provider_fields(*dialog.translation_registry, "deepl");
        const auto pending = provider_fields::pending_config(config_, fields, *dialog.provider_field_widgets);
        QCOMPARE(pending->get_translation_provider_config("deepl").value("api_key").toString(), QString("unsaved-key"));
        QVERIFY(config_->get_deepl_api_key() != "unsaved-key");
        // Custom service slots resolve by name too.
        QVERIFY(config_->set_provider_field("custom_llm_2_model", "m2"));
        QCOMPARE(config_->get_provider_field("custom_llm_2_model")->toString(), QString("m2"));
        QVERIFY(!config_->get_provider_field("not_a_field"));
    }

    // The monthly quota of a pending test is the saved one: checked against it and counted into it.
    void pendingConfigUsesTheSavedQuota() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.on_nav_changed(4, "translation");
        auto *limit = qobject_cast<QLineEdit *>(dialog.provider_field_widgets->value("volcengine_translate_quota_limit").data());
        QVERIFY(limit);
        limit->setText("5");
        const auto fields = provider_fields::provider_fields(*dialog.translation_registry, "volcengine");
        const auto pending = provider_fields::pending_config(config_, fields, *dialog.provider_field_widgets);
        QCOMPARE(pending->get_translation_quota_limit("volcengine"), config_->get_translation_quota_limit("volcengine"));
        QCOMPARE(pending->get_translation_quota_enabled("volcengine"), config_->get_translation_quota_enabled("volcengine"));
        const auto before = config_->get_translation_usage_chars("volcengine");
        pending->add_translation_usage_chars("volcengine", 3);
        QCOMPARE(pending->get_translation_usage_chars("volcengine"), before + 3);
        QCOMPARE(config_->get_translation_usage_chars("volcengine"), before + 3);
        config_->settings().remove("translation/usage");
    }

    void quotaFieldsResolveByName() {
        QVERIFY(config_->set_provider_field("volcengine_translate_quota_enabled", false));
        QCOMPARE(config_->get_provider_field("volcengine_translate_quota_enabled")->toBool(), false);
        QVERIFY(config_->set_provider_field("tencent_translate_quota_limit", " 77 "));
        QCOMPARE(config_->get_provider_field("tencent_translate_quota_limit")->toString(), QString("77"));
        QVERIFY(config_->set_provider_field("tencent_translate_secret_id", " id "));
        QCOMPARE(config_->get_provider_field("tencent_translate_secret_id")->toString(), QString("id"));
        config_->settings().remove("translation/providers/volcengine");
        config_->settings().remove("translation/providers/tencent");
    }

    void quotaProvidersShowTheirUsageUnderTheNotice() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.on_nav_changed(4, "translation");
        config_->settings().remove("translation/usage");
        config_->add_translation_usage_chars("volcengine", 12345);
        const auto guard_on = dialog.tr_text("Used this month: %1 / %2 characters (quota guard on)");
        const auto guard_off = dialog.tr_text("Used this month: %1 / %2 characters (quota guard off)");
        QCOMPARE(quota_usage_text(&dialog, "volcengine"), guard_on.arg("12,345", "1,990,000"));
        config_->set_volcengine_translate_quota_enabled(false);
        config_->set_volcengine_translate_quota_limit("20000");
        QCOMPARE(quota_usage_text(&dialog, "volcengine"), guard_off.arg("12,345", "20,000"));

        QSet<QString> shown;
        for (const auto &meta : dialog.translation_registry->available_providers())
            if (!quota_usage_text(&dialog, meta.provider_id).isEmpty()) shown << meta.provider_id;
        QCOMPARE(shown, (QSet<QString>{"volcengine", "tencent"}));
        QVERIFY(quota_usage_text(&dialog, "").isEmpty());
        QVERIFY(quota_usage_text(&dialog, "no-such-provider").isEmpty());

        auto *combo = dialog.attr<QComboBox>("translation_provider_combo");
        combo->setCurrentIndex(combo->findData("tencent"));
        const auto text = dialog.attr<QLabel>("translation_notice_label")->text();
        QVERIFY(text.contains("<br/>"));
        QVERIFY(text.endsWith(quota_usage_text(&dialog, "tencent")));
        config_->settings().remove("translation/usage");
        config_->settings().remove("translation/providers/volcengine");
    }

    void wizardNavigatesAcrossSixPages() {
        WelcomeWizard wizard(config_);
        QCOMPARE(wizard.page_count(), 6);
        QCOMPARE(wizard.current_page(), 0);
        wizard.go_to_page(3);
        QCOMPARE(wizard.current_page(), 3);
        wizard.go_to_page(9);
        QCOMPARE(wizard.current_page(), 3);
    }

    // A conflicting hotkey keeps its stored value; an empty main hotkey keeps the old one.
    void wizardHotkeyPageSkipsConflicts() {
        config_->set_hotkey("ctrl+1");
        config_->set_translation_hotkey("");
        HotkeyPage page(config_);
        auto edits = page.findChildren<HotkeyEdit *>();
        QCOMPARE(edits.size(), 6);
        // Order: screenshot, clipboard and translation, each main then alternate.
        edits[4]->setText("ctrl+1");
        page.save();
        QCOMPARE(config_->get_hotkey(), QString("ctrl+1"));
        QCOMPARE(config_->get_translation_hotkey(), QString(""));
        edits[4]->setText("ctrl+7");
        edits[0]->setText("");
        page.save();
        QCOMPARE(config_->get_hotkey(), QString("ctrl+1"));
        QCOMPARE(config_->get_translation_hotkey(), QString("ctrl+7"));
    }

    void wizardScreenshotPageSaves() {
        config_->set_screenshot_format("PNG");
        ScreenshotHotkeyPage page(config_);
        auto *combo = page.findChild<QComboBox *>();
        QVERIFY(combo);
        combo->setCurrentIndex(combo->findData("WEBP"));
        page.save();
        QCOMPARE(config_->get_screenshot_format(), QString("WEBP"));
    }

    // The wizard's steps, buttons and pages respond and paint; nothing here closes or finishes
    // it, since the finish page writes the Run key and the desktop shortcut.
    void wizardStepsButtonsAndPagesWork() {
        WelcomeWizard wizard(config_);
        wizard.show();
        QVERIFY(QTest::qWaitForWindowExposed(&wizard));
        auto steps = wizard.findChildren<StepItem *>();
        QCOMPARE(steps.size(), 6);
        QTest::mouseClick(steps[2], Qt::LeftButton);
        QCOMPARE(wizard.current_page(), 2);
        steps[1]->setFocus();
        QTest::keyClick(steps[1], Qt::Key_Space);
        QCOMPARE(wizard.current_page(), 1);
        QTest::keyClick(steps[3], Qt::Key_Return);
        QCOMPARE(wizard.current_page(), 3);
        QTest::keyClick(steps[3], Qt::Key_A);
        QCOMPARE(wizard.current_page(), 3);
        QEnterEvent enter(QPointF(2, 2), QPointF(2, 2), QPointF(2, 2));
        QApplication::sendEvent(steps[0], &enter);
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(steps[0], &leave);
        const auto button = [&wizard](const char *text) -> QPushButton * {
            for (auto *candidate : wizard.findChildren<QPushButton *>())
                if (candidate->text() == welcome_tr(text) && candidate->isVisible()) return candidate;
            return nullptr;
        };
        button("上一步")->click();
        QCOMPARE(wizard.current_page(), 2);
        button("下一步")->click();
        QCOMPARE(wizard.current_page(), 3);
        // Every page paints, the finish page through its whole completion animation.
        for (int page = 0; page < wizard.page_count(); ++page) {
            wizard.go_to_page(page);
            QApplication::processEvents();
            QVERIFY(!wizard.grab().isNull());
        }
        QVERIFY(button("完成"));
        QVERIFY(!button("跳过"));
        QElapsedTimer animation;
        animation.start();
        while (animation.elapsed() < 1400) {
            QTest::qWait(60);
            QVERIFY(!wizard.grab().isNull());
        }
        wizard.hide();
    }

    // Focusing a hotkey slot lights its keys on the keyboard map.
    void wizardHotkeyMapFollowsTheFocusedSlot() {
        config_->set_hotkey("ctrl+shift+a");
        WelcomeWizard wizard(config_);
        wizard.show();
        QVERIFY(QTest::qWaitForWindowExposed(&wizard));
        wizard.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&wizard));
        wizard.go_to_page(1);
        auto *page = wizard.findChild<HotkeyPage *>();
        QVERIFY(page);
        auto edits = page->findChildren<HotkeyEdit *>();
        QCOMPARE(edits.size(), 6);
        const QImage idle = page->grab().toImage();
        edits[0]->edit->setFocus();
        QApplication::processEvents();
        QVERIFY(edits[0]->edit->hasFocus());
        const QImage focused = page->grab().toImage();
        QVERIFY(focused != idle);
        // Typing into the slot changes the lit keys with it.
        edits[0]->setText("alt+x1");
        QApplication::processEvents();
        QVERIFY(page->grab().toImage() != focused);
        edits[1]->edit->setFocus();
        QApplication::processEvents();
        page->retranslate();
        wizard.hide();
    }

    // The first page switches the interface language and theme for the whole wizard.
    void wizardLanguageAndThemeApplyAtOnce() {
        const QString language = config_->get_app_setting("language", "zh").toString();
        const QString theme = config_->get_app_setting("ui_theme_mode", "system").toString();
        WelcomeWizard wizard(config_);
        wizard.show();
        QVERIFY(QTest::qWaitForWindowExposed(&wizard));
        auto *page = wizard.findChild<WelcomePage *>();
        QVERIFY(page);
        auto combos = page->findChildren<QComboBox *>();
        QComboBox *lang = nullptr, *mode = nullptr;
        for (auto *combo : combos) (combo->findData("dark") >= 0 ? mode : lang) = combo;
        QVERIFY(lang && mode);
        const int english = lang->findData("en");
        QVERIFY(english >= 0);
        lang->setCurrentIndex(english);
        QCOMPARE(config_->get_app_setting("language").toString(), QString("en"));
        QCOMPARE(wizard.windowTitle().isEmpty(), false);
        QVERIFY(!wizard.grab().isNull());
        mode->setCurrentIndex(mode->findData("dark"));
        QCOMPARE(config_->get_app_setting("ui_theme_mode").toString(), QString("dark"));
        for (int index = 0; index < wizard.page_count(); ++index) {
            wizard.go_to_page(index);
            QVERIFY(!wizard.grab().isNull());
        }
        mode->setCurrentIndex(mode->findData(theme));
        const int original = lang->findData(language);
        lang->setCurrentIndex(original >= 0 ? original : 0);
        wizard.hide();
        config_->set_app_setting("language", language);
    }

    // The translation page shows one credential form per provider and saves the choice.
    void wizardTranslationPageSwitchesProviders() {
        TranslationPage page(config_);
        page.show();
        auto *stack = page.findChild<QStackedWidget *>();
        QVERIFY(stack);
        auto combos = page.findChildren<QComboBox *>();
        QComboBox *providers = nullptr;
        for (auto *combo : combos)
            if (combo->count() == stack->count()) providers = combo;
        QVERIFY(providers);
        QSet<QWidget *> forms;
        for (int index = 0; index < providers->count(); ++index) {
            providers->setCurrentIndex(index);
            forms.insert(stack->currentWidget());
            QVERIFY(!page.grab().isNull());
        }
        QCOMPARE(forms.size(), providers->count());
        providers->setCurrentIndex(0);
        page.retranslate();
        page.save();
        QCOMPARE(config_->get_translation_provider(), providers->currentData().toString());
    }

    // "Reset This Page" puts the shown page back to the defaults; nothing is saved until Apply.
    void resettingAPageRestoresItsDefaults() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.build_all_pages();
        QPushButton *reset = nullptr;
        for (auto *button : dialog.findChildren<QPushButton *>())
            if (button->text() == settings_tr("Reset This Page")) reset = button;
        QVERIFY(reset);
        const auto &defaults = config_->app_defaults();
        dialog.on_nav_changed(0, "shortcuts");
        auto *hotkey = dialog.attr<HotkeyEdit>("hotkey_input");
        QVERIFY(hotkey);
        hotkey->setText("ctrl+alt+9");
        reset->click();
        QCOMPARE(hotkey->text(), defaults.value("hotkey").toString());
        dialog.on_nav_changed(5, "log");
        auto *level = dialog.attr<QComboBox>("log_level_combo");
        QVERIFY(level);
        level->setCurrentIndex(level->findText(defaults.value("log_level").toString()) == 0 ? 1 : 0);
        reset->click();
        QCOMPARE(level->currentText(), defaults.value("log_level").toString());
        dialog.on_nav_changed(1, "capture");
        auto *format = dialog.attr<QComboBox>("screenshot_format_combo");
        QVERIFY(format);
        format->setCurrentIndex(format->count() - 1);
        reset->click();
        QCOMPARE(format->currentText().toUpper(), defaults.value("screenshot_format").toString().toUpper());
        // Every other page resets too.
        const std::pair<int, const char *> pages[] = {{3, "appearance"}, {4, "translation"}, {6, "other"}, {9, "quick_actions"}, {10, "mouse"}, {11, "pin"}};
        for (const auto &[index, route] : pages) {
            dialog.on_nav_changed(index, route);
            QCOMPARE(dialog.content_stack->currentIndex(), index);
            reset->click();
        }
        // Unknown pages are ignored.
        dialog.on_nav_changed(42);
        QCOMPARE(dialog.content_stack->currentIndex(), 11);
    }

    // The logo's context menu opens the developer page; the pin page links to its shortcut tabs.
    void developerPageAndPinShortcutLinks() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        QWidget *logo = nullptr;
        for (auto *widget : dialog.findChildren<QWidget *>())
            if (widget->contextMenuPolicy() == Qt::CustomContextMenu) logo = widget;
        QVERIFY(logo);
        QTimer::singleShot(0, &dialog, [] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) return;
            menu->setActiveAction(menu->actions().value(0));
            QTest::keyClick(menu, Qt::Key_Return);
        });
        emit logo->customContextMenuRequested(QPoint(2, 2));
        // The developer page is reachable only this way, and resets like the others.
        QCOMPARE(dialog.content_stack->currentIndex(), 7);
        auto *engine = dialog.attr<QComboBox>("engine_combo");
        QVERIFY(engine);
        const auto engine_default = config_->app_defaults().value("long_stitch_engine");
        engine->setCurrentIndex(engine->findData(engine_default) == 0 ? engine->count() - 1 : 0);
        for (auto *button : dialog.findChildren<QPushButton *>())
            if (button->text() == settings_tr("Reset This Page")) button->click();
        QCOMPARE(engine->currentData(), engine_default);
        dialog.show_pin_shortcuts(true);
        QCOMPARE(dialog.content_stack->currentIndex(), 10);
        QVERIFY(dialog.mouse_inapp_tabs);
        dialog.show_pin_shortcuts(false);
        QCOMPARE(dialog.content_stack->currentIndex(), 0);
        QVERIFY(dialog.inapp_shortcut_tabs);
        QApplication::processEvents();
        QSignalSpy wizard(&dialog, &SettingsDialog::wizard_requested);
        dialog.open_welcome_wizard();
        QCOMPARE(wizard.size(), 1);
        dialog.hide();
    }

    // Folder choices go through a folder dialog; "open" hands the folder to the desktop.
    void saveAndLogFoldersAreChosenAndOpened() {
        QTemporaryDir chosen;
        // Opening creates the folders, so they point into the test's own directory.
        config_->set_screenshot_save_path(dir_.filePath("captures"));
        config_->set_log_dir(dir_.filePath("logs"));
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.build_all_pages();
        const auto answer = [&chosen](bool accept) {
            auto *timer = new QTimer;
            QObject::connect(timer, &QTimer::timeout, [timer, &chosen, accept] {
                auto *picker = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
                if (!picker || !picker->isVisible()) return;
                timer->stop();
                timer->deleteLater();
                if (!accept) {
                    picker->reject();
                    return;
                }
                picker->setDirectory(chosen.path());
                if (auto *name = picker->findChild<QLineEdit *>("fileNameEdit")) name->clear();
                static_cast<QDialog *>(picker)->accept();
            });
            timer->start(50);
            // Never left waiting for a person.
            QTimer::singleShot(10000, [] {
                if (auto *picker = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) picker->reject();
            });
        };
        const auto label_text = [&dialog](const char *name) { return QDir::cleanPath(dialog.attr_widget(name)->property("text").toString()); };
        const QString save_before = label_text("save_path_lbl");
        answer(false);
        dialog.change_save_dir();
        QCOMPARE(label_text("save_path_lbl"), save_before);
        answer(true);
        dialog.change_save_dir();
        QCOMPARE(label_text("save_path_lbl"), QDir::cleanPath(chosen.path()));
        answer(true);
        dialog.change_log_dir();
        QCOMPARE(label_text("path_lbl"), QDir::cleanPath(chosen.path()));
        UrlCatcher catcher;
        QDesktopServices::setUrlHandler("file", &catcher, "open");
        dialog.open_save_dir();
        dialog.open_log_dir();
        QDesktopServices::unsetUrlHandler("file");
        QCOMPARE(catcher.urls.size(), 2);
        QCOMPARE(QDir::cleanPath(catcher.urls[0].toLocalFile()), QDir::cleanPath(config_->get_screenshot_save_path()));
        QCOMPARE(QDir::cleanPath(catcher.urls[1].toLocalFile()), QDir::cleanPath(config_->get_log_dir()));
    }


    // An in-app shortcut taken by another one in the same window asks before moving it over.
    void inAppShortcutConflictsAskBeforeReplacing() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.build_all_pages();
        InAppKeyEdit *first = nullptr, *second = nullptr;
        for (const auto &[key, edit] : dialog.inapp_edits) {
            if (first || dialog.inapp_groups.value(key) == "clipboard") continue;
            for (const auto &[other_key, other] : dialog.inapp_edits) {
                if (other_key != key && dialog.inapp_groups.value(other_key) == dialog.inapp_groups.value(key)) {
                    first = edit;
                    second = other;
                    break;
                }
            }
        }
        QVERIFY(first && second);
        first->setText("ctrl+alt+k");
        // Declining keeps the other one and restores this one's saved value.
        const auto answer = [](bool yes) {
            auto *timer = new QTimer;
            QObject::connect(timer, &QTimer::timeout, [timer, yes] {
                auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!modal) return;
                timer->stop();
                timer->deleteLater();
                if (!yes) {
                    modal->reject();
                    return;
                }
                for (auto *box : modal->findChildren<QDialogButtonBox *>())
                    for (auto *button : box->buttons())
                        if (box->buttonRole(button) == QDialogButtonBox::YesRole || box->buttonRole(button) == QDialogButtonBox::AcceptRole) {
                            button->click();
                            return;
                        }
                modal->accept();
            });
            timer->start(20);
        };
        const QString saved = second->text();
        answer(false);
        second->setText("ctrl+alt+k");
        QCOMPARE(first->text(), QString("ctrl+alt+k"));
        QCOMPARE(second->text(), saved);
        // Accepting moves the shortcut: the other one is cleared.
        answer(true);
        second->setText("ctrl+alt+k");
        QCOMPARE(second->text(), QString("ctrl+alt+k"));
        QCOMPARE(first->text(), QString());
        // Unfinished prefixes and empty values never ask.
        second->setText("ctrl+");
        second->setText("");
    }

    // Choice settings fall back to their defaults for unknown values; free-form ones are parsed defensively.
    void choiceSettingsRejectUnknownValues() {
        const auto &defaults = config_->app_defaults();
        config_->set_capture_engine("HDR");
        QCOMPARE(config_->get_capture_engine(), QString("hdr"));
        config_->set_capture_engine("telepathy");
        QCOMPARE(config_->get_capture_engine(), defaults.value("capture_engine").toString());
        settings_->setValue("app/capture_engine", "bogus");
        QCOMPARE(config_->get_capture_engine(), defaults.value("capture_engine").toString());
        config_->set_ocr_engine("nonsense");
        QCOMPARE(config_->get_ocr_engine(), defaults.value("ocr_engine").toString());
        config_->set_ocr_engine("PPOCR_RUST");
        QCOMPARE(config_->get_ocr_engine(), QString("ppocr_rust"));
        config_->set_ocr_layout_mode("diagonal");
        QCOMPARE(config_->get_ocr_layout_mode(), defaults.value("ocr_layout_mode").toString());
        config_->set_ocr_punctuation("fancy");
        QCOMPARE(config_->get_ocr_punctuation(), defaults.value("ocr_punctuation").toString());
        // The OCR window's last centre: "x,y" or nothing.
        settings_->setValue("app/ocr_result_last_center", "12, 34");
        QCOMPARE(config_->get_ocr_result_last_center(), std::optional<QPoint>(QPoint(12, 34)));
        settings_->setValue("app/ocr_result_last_center", "12,abc");
        QCOMPARE(config_->get_ocr_result_last_center(), std::nullopt);
        settings_->setValue("app/ocr_result_last_center", "nothing");
        QCOMPARE(config_->get_ocr_result_last_center(), std::nullopt);
        // A relative save folder is replaced by the default.
        settings_->setValue("app/screenshot_save_path", "relative/folder");
        QCOMPARE(config_->get_screenshot_save_path(), defaults.value("screenshot_save_path").toString());
        QCOMPARE(settings_->value("app/screenshot_save_path").toString(), defaults.value("screenshot_save_path").toString());
        // An unknown action has no default binding.
        bool threw = false;
        try {
            get_quick_capture_binding(config_, "no-such-action");
        } catch (const std::out_of_range &) {
            threw = true;
        }
        QVERIFY(threw);
        // A first run opens the settings window; afterwards the switch decides.
        settings_->clear();
        QVERIFY(config_->should_show_main_window_on_start());
        config_->mark_as_run();
        config_->set_show_main_window(true);
        QVERIFY(config_->should_show_main_window_on_start());
        config_->set_show_main_window(false);
        QVERIFY(!config_->should_show_main_window_on_start());
    }

    // Each provider's configuration as the translation service reads it.
    void providerConfigsCarryTheirOwnFields() {
        config_->set_provider_field("amazon_translate_region", "eu-west-1");
        config_->set_provider_field("azure_translate_endpoint", "https://example.test");
        config_->set_provider_field("baidu_translate_appid", "app");
        config_->set_provider_field("deepseek_model", "deepseek-chat");
        config_->set_provider_field("custom_llm_2_model", "local");
        QCOMPARE(config_->get_translation_provider_config("amazon").value("region").toString(), QString("eu-west-1"));
        QCOMPARE(config_->get_translation_provider_config("azure").value("endpoint").toString(), QString("https://example.test"));
        QCOMPARE(config_->get_translation_provider_config("baidu").value("appid").toString(), QString("app"));
        QCOMPARE(config_->get_translation_provider_config("deepseek").value("model").toString(), QString("deepseek-chat"));
        QVERIFY(config_->get_translation_provider_config("google").contains("api_key"));
        const auto custom = config_->get_translation_provider_config("custom_llm_2");
        QCOMPARE(custom.value("model").toString(), QString("local"));
        QVERIFY(custom.contains("json_mode"));
        QVERIFY(config_->get_translation_provider_config("unknown").isEmpty());
        QVERIFY(!config_->set_provider_field("custom_llm_2_unknown_key", "x"));
        QVERIFY(config_->get_translation_params().contains("target_lang"));
        const auto exported = config_->export_settings();
        QVERIFY(exported.contains("pen"));
        QVERIFY(exported.contains("text"));
    }

    // Each provider's form tests its unsaved values and fetches model lists in the background;
    // nothing here reaches the network: DeepL lacks a key and the model list goes to a closed local port.
    void translationFormsTestAndFetchWithoutSaving() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.build_all_pages();
        dialog.on_nav_changed(4, "translation");
        const auto button_in = [&dialog](const QString &provider, const char *text) -> QPushButton * {
            auto section = dialog.provider_sections.value(provider);
            if (!section) return nullptr;
            for (auto *button : section->findChildren<QPushButton *>())
                if (button->text() == settings_tr(text)) return button;
            return nullptr;
        };
        // Secrets are hidden until asked for.
        auto *show = button_in("deepl", "Show");
        QVERIFY(show);
        show->click();
        QCOMPARE(show->text(), settings_tr("Hide"));
        show->click();
        QCOMPARE(show->text(), settings_tr("Show"));
        auto status = dialog.provider_status_labels.value("deepl");
        QVERIFY(status);
        auto *test = button_in("deepl", "Test Connection");
        QVERIFY(test);
        test->click();
        QTRY_VERIFY_WITH_TIMEOUT(status->text().startsWith(QString::fromUtf8("✗")), 10000);
        QVERIFY(test->isEnabled());
        // The custom service's model list, from an address nothing listens on.
        auto base_url = qobject_cast<QLineEdit *>(dialog.provider_field_widgets->value("custom_llm_base_url").data());
        QVERIFY(base_url);
        base_url->setText("http://127.0.0.1:9/v1");
        auto *fetch = button_in("custom_llm", "Fetch Models");
        QVERIFY(fetch);
        fetch->click();
        auto custom_status = dialog.provider_status_labels.value("custom_llm");
        QTRY_VERIFY_WITH_TIMEOUT(custom_status->text().startsWith(QString::fromUtf8("✗")), 15000);
        QVERIFY(fetch->isEnabled());
        // Nothing was saved.
        QVERIFY(config_->get_provider_field("custom_llm_base_url")->toString() != QString("http://127.0.0.1:9/v1"));
    }
    // Saves each page as PNG for visual review when JIETUBA_SETTINGS_SHOTS names a folder;
    // needs -platform windows for real fonts.
    void renderPagesForReview() {
        const QString out = qEnvironmentVariable("JIETUBA_SETTINGS_SHOTS");
        if (out.isEmpty()) QSKIP("JIETUBA_SETTINGS_SHOTS is not set.");
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.setAttribute(Qt::WA_DontShowOnScreen);
        dialog.show();
        const std::pair<int, const char *> pages[] = {{0, "shortcuts"}, {10, "mouse"}, {9, "quick_actions"}, {1, "capture"}, {11, "pin"},
                                                      {4, "translation"}, {3, "appearance"}, {5, "log"}, {6, "other"}, {8, "about"}};
        for (const auto &[index, route] : pages) {
            dialog.on_nav_changed(index, route);
            QTest::qWait(200);
            QVERIFY(dialog.grab().save(QDir(out).filePath(QString("%1_%2.png").arg(index, 2, 10, QChar('0')).arg(route))));
        }
        WelcomeWizard wizard(config_);
        wizard.setAttribute(Qt::WA_DontShowOnScreen);
        wizard.show();
        for (int page = 0; page < wizard.page_count(); ++page) {
            wizard.go_to_page(page);
            QTest::qWait(300);
            QVERIFY(wizard.grab().save(QDir(out).filePath(QString("wizard_%1.png").arg(page))));
        }
        // Closing saves every page; the finish page would write the Run key and the desktop.
        wizard.hide();
    }
};

QTEST_MAIN(SettingsWindowTests)
#include "test_settings_window.moc"
