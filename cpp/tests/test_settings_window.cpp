#include "core/elevation.h"
#include "core/i18n.h"
#include "core/resource_manager.h"
#include "ocr/ocr_manager.h"
#include "ocr/ppocr_models.h"
#include "settings/settings_transfer.h"
#include "settings/tool_settings.h"
#include "core/ui_scale.h"
#include "ui/color_picker_dialog.h"
#include "ui/fluent_lite/cards.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/labels.h"
#include "ui/fluent_lite/navigation.h"
#include "ui/hotkey_edit.h"
#include "ui/inapp_key_edit.h"
#include "ui/settings_ui/dialog.h"
#include "ui/settings_ui/mouse_binding_editor.h"
#include "ui/settings_ui/pages.h"
#include "ui/welcome/pages.h"
#include "ui/welcome/wizard.h"
#include <QApplication>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QMenu>
#include <QSignalSpy>
#include <QTimer>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QStandardItemModel>
#include <QElapsedTimer>
#include <QFile>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTest>

using namespace jietuba;

namespace {
// Answers the next modal question: yes clicks its Yes / OK button, no rejects it. Gives up
// after two seconds, so a question that never comes cannot catch a later one.
void answer_next(bool yes) {
    auto *timer = new QTimer;
    QElapsedTimer waited;
    waited.start();
    QObject::connect(timer, &QTimer::timeout, [timer, yes, waited] {
        auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!modal && waited.elapsed() < 2000) return;
        timer->stop();
        timer->deleteLater();
        if (!modal) return;
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
}

// Clicks "Reset This Page" and confirms its question.
void reset_page(QWidget &dialog) {
    for (auto *button : dialog.findChildren<QPushButton *>())
        if (button->text() == settings_tr("Reset This Page")) {
            answer_next(true);
            button->click();
            return;
        }
    QFAIL("no reset button");
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

    void sizeFollowsTheScreenAndKeepsAUserSize() {
        auto &scale = get_dialog_scale();
        const int before = scale.percent();
        scale.set_percent(100);
        const auto restore = qScopeGuard([&] { scale.set_percent(before); });
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.fit_to_available({1920, 1032});
        QCOMPARE(dialog.size(), QSize(1050, 750));
        dialog.fit_to_available({2560, 1392});
        QCOMPARE(dialog.size(), QSize(1150, 900));
        dialog.fit_to_available({1280, 672});
        QCOMPARE(dialog.size(), QSize(1050, 600));

        // A size the user dragged is kept and only shrunk to fit.
        dialog.resize(1100, 800);
        dialog.fit_to_available({2560, 1392});
        QCOMPARE(dialog.size(), QSize(1100, 800));
        dialog.fit_to_available({1280, 672});
        QCOMPARE(dialog.size(), QSize(1100, 672));

        // A visible dialog is left alone, even on a screen it does not fit.
        dialog.show();
        const QSize shown = dialog.size();
        dialog.fit_to_screen();
        QCOMPARE(dialog.size(), shown);
    }

    // Only a dialog with no part on any screen is moved back; its corner alone does not decide.
    void aDialogOffEveryScreenIsCentredAgain() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.fit_to_screen();
        const QPoint partly(-200, -100);
        dialog.move(partly);
        dialog.fit_to_screen();
        QCOMPARE(dialog.pos(), partly);
        dialog.move(-20000, -20000);
        dialog.fit_to_screen();
        const auto *screen = QGuiApplication::screenAt(QCursor::pos());
        if (!screen) screen = QGuiApplication::primaryScreen();
        QVERIFY(screen->availableGeometry().contains(dialog.frameGeometry().center()));
    }

    void navigationScrollsInsteadOfHoldingTheWindowTall() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        QCoreApplication::processEvents();
        // The whole navigation is over 600 px tall; scrolling, it no longer holds the window up.
        QVERIFY(dialog.minimumSizeHint().height() < 400);

        dialog.resize(1050, 420);
        QCoreApplication::processEvents();
        auto *nav = dialog.findChild<NavigationInterface *>("SettingsNavigation");
        QVERIFY(nav);
        auto *bar = nav->scrollArea()->verticalScrollBar();
        QVERIFY(bar->maximum() > 0);
        QCOMPARE(bar->value(), 0);

        // Selecting an item scrolled out of view brings it in; About sits outside the scroll area.
        nav->setCurrentItem("other");
        QCoreApplication::processEvents();
        QVERIFY(bar->value() > 0);
        auto *about = nav->widget("about");
        QVERIFY(about);
        QVERIFY(!nav->scrollArea()->isAncestorOf(about));
        QVERIFY(about->isVisible());
    }

    // There is no Apply: an edit is saved once edits pause, and only what changed is written.
    void editsAreSavedOnceTheyPause() {
        config_->set_screenshot_save_enabled(false);
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        dialog.on_nav_changed(1, "capture");
        for (auto *button : dialog.findChildren<QPushButton *>()) QVERIFY(button->text() != QLatin1String("Apply"));
        auto *toggle = dialog.attr<SwitchSettingCard>("save_toggle");
        auto *grid = dialog.attr<SwitchSettingCard>("magnifier_grid_toggle");
        QVERIFY(toggle && grid);
        QVERIFY(!toggle->isChecked());
        QSignalSpy saved(&dialog, &SettingsDialog::settings_applied);
        toggle->setChecked(true);
        QVERIFY(dialog.has_unsaved_changes());
        QVERIFY(!config_->get_screenshot_save_enabled());
        QTRY_VERIFY(config_->get_screenshot_save_enabled());
        QCOMPARE(saved.size(), 1);
        QCOMPARE(dialog.saved_keys(), QSet<QString>{"save_toggle"});
        QVERIFY(!dialog.hotkeys_saved());
        QVERIFY(!dialog.has_unsaved_changes());

        // A run of edits is one save.
        toggle->setChecked(false);
        toggle->setChecked(true);
        toggle->setChecked(false);
        QTRY_COMPARE(saved.size(), 2);
        QVERIFY(!config_->get_screenshot_save_enabled());

        // A setting changed elsewhere while the window is open survives saves of other controls.
        const bool grid_shown = grid->isChecked();
        config_->set_app_setting("magnifier_grid", !grid_shown);
        toggle->setChecked(true);
        QVERIFY(dialog.apply_settings());
        QCOMPARE(config_->get_app_setting("magnifier_grid").toBool(), !grid_shown);
        config_->set_app_setting("magnifier_grid", grid_shown);

        // Hiding saves what is still waiting.
        toggle->setChecked(false);
        dialog.hide();
        QVERIFY(!config_->get_screenshot_save_enabled());
        QCOMPARE(saved.size(), 4);
    }

    // Run as administrator sits under autostart and hides with it. Without elevation it is the same
    // plain switch: applying goes through UAC, and turning it on hands off to the elevated instance.
    void runAsAdminCardFollowsElevation() {
        const auto restore = qScopeGuard([this] {
            elevation::set_environment_for_test(std::nullopt, std::nullopt, std::nullopt);
            elevation::set_schtasks_runner_for_test({});
            config_->set_app_setting("run_as_admin", false);
            config_->set_app_setting("autostart_enabled", true);
        });
        elevation::set_environment_for_test(std::nullopt, elevation::STANDARD, std::nullopt);
        {
            SettingsDialog dialog(config_, config_->get_hotkey());
            dialog.build_all_pages();
            QVERIFY(!dialog.has_attr("run_as_admin_toggle"));
        }
        QStringList calls;
        elevation::set_schtasks_runner_for_test([&calls](const QStringList &args) {
            calls << args.value(0);
            return std::pair<int, QByteArray>{0, {}};
        });
        elevation::set_environment_for_test(std::nullopt, elevation::ELEVATED, std::nullopt);
        config_->set_app_setting("autostart_enabled", false);
        {
            SettingsDialog dialog(config_, config_->get_hotkey());
            dialog.build_all_pages();
            auto *card = dialog.attr<SwitchSettingCard>("run_as_admin_toggle");
            QVERIFY(card);
            QVERIFY(card->isHidden());
            dialog.attr<SwitchSettingCard>("autostart_toggle")->setChecked(true);
            QVERIFY(!card->isHidden());
            QVERIFY(card->switchButton->isVisibleTo(card));
            card->setChecked(true);
            QVERIFY(dialog.apply_settings());
            QCOMPARE(calls, QStringList{"/Create"});
            QVERIFY(config_->get_app_setting("run_as_admin").toBool());
        }
        elevation::set_environment_for_test(std::nullopt, elevation::CAN_ELEVATE, std::nullopt);
        config_->set_app_setting("run_as_admin", false);
        calls.clear();
        QStringList elevated_calls;
        elevation::set_elevated_schtasks_runner_for_test([&elevated_calls](const QStringList &args) {
            elevated_calls << args.value(0);
            return std::pair<int, QByteArray>{0, {}};
        });
        const auto restore_elevated = qScopeGuard([] { elevation::set_elevated_schtasks_runner_for_test({}); });
        {
            SettingsDialog dialog(config_, config_->get_hotkey());
            dialog.build_all_pages();
            auto *card = dialog.attr<SwitchSettingCard>("run_as_admin_toggle");
            QVERIFY(card);
            QVERIFY(card->switchButton->isEnabled());
            QVERIFY(dialog.attr_widget("autostart_toggle")->isEnabled());
            QVERIFY(card->contentLabel->text().contains("administrator permission"));
            QSignalSpy handed_off(&dialog, &SettingsDialog::admin_mode_enabled);
            card->setChecked(true);
            QVERIFY(dialog.apply_settings());
            QCOMPARE(elevated_calls, QStringList{"/Create"});
            QVERIFY(config_->get_app_setting("run_as_admin").toBool());
            // Emitted after the save has finished.
            QCOMPARE(handed_off.count(), 0);
            QTRY_COMPARE(handed_off.count(), 1);
        }
    }

    // The OCR list holds what can be chosen: Auto, the installed models in catalog order with ones
    // added by hand last, then the download entry. A chosen model that is gone stays listed,
    // disabled; engine and model are saved together.
    void ocrListOffersInstalledModelsAndKeepsAMissingChoice() {
        QTemporaryDir models;
        QVERIFY(models.isValid());
        const QDir root(models.path());
        const auto write = [&root](const QString &name, const QByteArray &data = "x") {
            QDir().mkpath(QFileInfo(root.filePath(name)).path());
            QFile file(root.filePath(name));
            return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
        };
        const auto manifest = [](const QString &name, const QString &catalog_id) {
            QJsonObject object{{"name", name}, {"det", "det.onnx"}, {"rec", "rec.onnx"}};
            if (!catalog_id.isEmpty()) object["catalog_id"] = catalog_id;
            return QJsonDocument(object).toJson();
        };
        QVERIFY(write("PP-OCRv6_det_small.onnx") && write("PP-OCRv6_rec_small.onnx"));
        for (const auto &[id, name, catalog_id] : {std::tuple<QString, QString, QString>{"local", "local", ""}, {"ppocr-v5-korean", "PP-OCRv5 Korean", "ppocr-v5-korean"}})
            QVERIFY(write(id + "/det.onnx") && write(id + "/rec.onnx") && write(id + "/model.json", manifest(name, catalog_id)));
        const QString runtime = QDir(QCoreApplication::applicationDirPath()).filePath("models/onnxruntime.dll");
        const bool runtime_ready = QFile::copy(runtime, root.filePath("onnxruntime.dll")) && ppocr_runtime_supported(root.filePath("onnxruntime.dll"));
        ppocr_models::set_models_root_for_test(root.path());
        set_oneocr_available_for_test(false);
        config_->set_ocr_engine("ppocr_rust");
        config_->set_ppocr_model("ppocr-v5-korean");
        const auto restore = qScopeGuard([this] {
            ppocr_models::set_models_root_for_test({});
            set_oneocr_available_for_test(std::nullopt);
            config_->set_ocr_engine("auto");
            config_->set_ppocr_model("");
            set_ppocr_model("");
            set_ocr_engine("auto");
        });

        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.on_nav_changed(1, "capture");
        auto *combo = dialog.attr<QComboBox>("ocr_engine_combo");
        QVERIFY(combo);
        auto *items = qobject_cast<QStandardItemModel *>(combo->model());
        QStringList catalog_order;
        for (const auto &value : ppocr_models::catalog().value("models").toArray()) catalog_order << value.toObject().value("id").toString();
        QStringList expected_models{"ppocr_rust:", "ppocr_rust:ppocr-v5-korean"};
        if (catalog_order.indexOf("ppocr-v5-korean") < catalog_order.indexOf("ppocr-v6-small")) expected_models.swapItemsAt(0, 1);
        QStringList data;
        for (int i = 0; i < combo->count(); ++i) data << combo->itemData(i).toString();
        QCOMPARE(data, (QStringList{"auto"} + expected_models + QStringList{"ppocr_rust:local", "", "manage_models"}));
        QCOMPARE(combo->itemText(combo->findData("ppocr_rust:")), QString("PP-OCRv6 small"));
        QCOMPARE(combo->itemText(combo->count() - 1), settings_tr("Download more models..."));
        for (const auto &model : expected_models) QCOMPARE(items->item(combo->findData(model))->isEnabled(), runtime_ready);
        QCOMPARE(items->item(0)->isEnabled(), runtime_ready);
        QCOMPARE(combo->currentData().toString(), QString("ppocr_rust:ppocr-v5-korean"));

        QVERIFY(write("new/det.onnx") && write("new/rec.onnx") && write("new/model.json", manifest("new", "")));
        emit model_downloads()->models_changed();
        QVERIFY(combo->findData("ppocr_rust:new") >= 0);
        QCOMPARE(combo->currentData().toString(), QString("ppocr_rust:ppocr-v5-korean"));

        combo->setCurrentIndex(combo->findData("ppocr_rust:local"));
        QVERIFY(dialog.apply_settings());
        QCOMPARE(config_->get_ocr_engine(), QString("ppocr_rust"));
        QCOMPARE(config_->get_ppocr_model(), QString("local"));

        QVERIFY(QDir(root.filePath("local")).removeRecursively());
        fill_ocr_engines(&dialog, "ppocr_rust", "local");
        QCOMPARE(combo->currentData().toString(), QString("ppocr_rust:local"));
        QCOMPARE(combo->currentText(), settings_tr("%1 (not installed)").replace("%1", "local"));
        QVERIFY(!items->item(combo->currentIndex())->isEnabled());
        QCOMPARE(ocr_choice("ppocr_rust:local"), (std::pair<QString, std::optional<QString>>{"ppocr_rust", QString("local")}));
        QCOMPARE(ocr_choice("oneocr"), (std::pair<QString, std::optional<QString>>{"oneocr", std::nullopt}));

        reset_page(dialog);
        QCOMPARE(combo->currentData().toString(), QString("auto"));
    }

    void resetRestoresTheCurrentPageDefaults() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.on_nav_changed(1, "capture");
        auto *toggle = dialog.attr<SwitchSettingCard>("magnifier_grid_toggle");
        const bool default_value = config_->app_defaults().value("magnifier_grid").toBool();
        toggle->setChecked(!default_value);
        reset_page(dialog);
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
        reset_page(dialog);
        QCOMPARE(combo->currentData().toString(), config_->app_defaults().value("capture_engine").toString());
        dialog.hide();
    }

    // The picker style is an appearance choice; the theme and mask colours open the picker it names.
    void colorPickerStyleLoadsSavesAndPicksAppearanceColours() {
        const auto restore = qScopeGuard([this] { config_->set_app_setting("color_picker_style", "modern"); });
        config_->set_app_setting("color_picker_style", "system");
        SettingsDialog dialog(config_, config_->get_hotkey());
        QVERIFY(dialog.settings_keys().count("app/color_picker_style"));
        dialog.show();
        dialog.on_nav_changed(3, "appearance");
        auto *combo = dialog.attr<QComboBox>("_color_picker_style_combo");
        QVERIFY(combo);
        QCOMPARE(combo->currentData().toString(), QString("system"));
        combo->setCurrentIndex(combo->findData(QString("modern")));
        QVERIFY(dialog.has_unsaved_changes());
        QVERIFY(dialog.apply_settings());
        QCOMPARE(config_->get_app_setting("color_picker_style").toString(), QString("modern"));
        const auto answer = [](std::function<void(QWidget *)> action) {
            auto *timer = new QTimer;
            QObject::connect(timer, &QTimer::timeout, [timer, action] {
                auto *modal = QApplication::activeModalWidget();
                if (!modal) return;
                timer->stop();
                timer->deleteLater();
                action(modal);
            });
            timer->start(10);
        };
        // Opaque either way: the modern picker without its opacity controls, the system one without alpha.
        answer([](QWidget *modal) {
            auto *picker = qobject_cast<ColorPickerDialog *>(modal);
            QVERIFY(picker);
            QVERIFY(picker->alpha_edit->isHidden());
            picker->setCurrentColor(QColor("#445566"));
            picker->accept();
        });
        qobject_cast<QPushButton *>(dialog.attr_widget("_mask_color_btn"))->click();
        QCOMPARE(*dialog.appearance_mask_color, QColor("#445566"));
        config_->set_app_setting("color_picker_style", "system");
        answer([](QWidget *modal) {
            auto *picker = qobject_cast<QColorDialog *>(modal);
            QVERIFY(picker);
            QVERIFY(!picker->testOption(QColorDialog::ShowAlphaChannel));
            picker->setCurrentColor(QColor(17, 34, 51, 90));
            picker->accept();
        });
        qobject_cast<QPushButton *>(dialog.attr_widget("_theme_color_btn"))->click();
        QCOMPARE(*dialog.appearance_theme_color, QColor(17, 34, 51));
        combo->setCurrentIndex(combo->findData(QString("system")));
        reset_page(dialog);
        QCOMPARE(combo->currentData().toString(), QString("modern"));
        dialog.hide();
    }

    // Duplicates already in the settings are flagged; filling them in asks nothing.
    void duplicateGlobalHotkeysAreFlagged() {
        config_->set_global_hotkeys("translation", {"ctrl+alt+shift+f6", "ctrl+alt+shift+f6", "", ""});
        const auto restore = qScopeGuard([this] { config_->set_global_hotkeys("translation", {"", "", "", ""}); });
        SettingsDialog dialog(config_, config_->get_hotkey());
        auto *alternate = dialog.attr<HotkeyEdit>("translation_hotkey_edit_2");
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
        reset_page(dialog);
        QCOMPARE(dialog.attr<HotkeyEdit>("clipboard_hotkey_edit_3")->text(), QString("win+v"));
        config_->set_global_hotkeys("translation", {"", "", "", ""});
        config_->set_hotkey("ctrl+alt+shift+f10");
        config_->set_global_hotkeys("clipboard", {"", "", "", ""});
    }

    // A global hotkey another one already uses asks before moving it; declining puts back the saved value.
    void aGlobalHotkeyInUseAsksBeforeMoving() {
        const auto restore = qScopeGuard([this] {
            config_->set_global_hotkeys("translation", {"", "", "", ""});
            config_->set_global_hotkeys("pin_clipboard", {"", "", "", ""});
        });
        config_->set_global_hotkeys("translation", {"ctrl+alt+shift+f6", "", "", ""});
        config_->set_global_hotkeys("pin_clipboard", {"", "", "", ""});
        SettingsDialog dialog(config_, config_->get_hotkey());
        auto *taken = dialog.attr<HotkeyEdit>("translation_hotkey_edit");
        auto *edit = dialog.attr<HotkeyEdit>("pin_clipboard_hotkey_edit_2");
        answer_next(false);
        edit->setText("ctrl+alt+shift+f6");
        QCOMPARE(edit->text(), QString());
        QCOMPARE(taken->text(), QString("ctrl+alt+shift+f6"));
        answer_next(true);
        edit->setText("ctrl+alt+shift+f6");
        QCOMPARE(taken->text(), QString());
        QVERIFY(dialog.apply_settings());
        QCOMPARE(config_->get_global_hotkeys("pin_clipboard")[1], QString("ctrl+alt+shift+f6"));
        QCOMPARE(config_->get_global_hotkeys("translation")[0], QString());
        QVERIFY(dialog.hotkeys_saved());
    }

    // An unfinished combination is never saved, and letting go of a lone modifier puts back the
    // value from before it; a duplicate that slipped in stays unsaved until the other side changes.
    void unfinishedAndDuplicateHotkeysStayUnsaved() {
        const auto restore = qScopeGuard([this] { config_->set_global_hotkeys("translation", {"", "", "", ""}); });
        config_->set_global_hotkeys("translation", {"ctrl+alt+shift+f8", "", "", ""});
        SettingsDialog dialog(config_, config_->get_hotkey());
        auto *edit = dialog.attr<HotkeyEdit>("translation_hotkey_edit");
        const auto send = [edit](QEvent::Type type, int key, Qt::KeyboardModifiers modifiers) {
            QKeyEvent event(type, key, modifiers);
            QApplication::sendEvent(edit->edit, &event);
        };
        QSignalSpy saved(&dialog, &SettingsDialog::settings_applied);
        send(QEvent::KeyPress, Qt::Key_Control, Qt::NoModifier);
        QCOMPARE(edit->text(), QString("ctrl+"));
        QVERIFY(dialog.apply_settings());
        QCOMPARE(config_->get_global_hotkeys("translation")[0], QString("ctrl+alt+shift+f8"));
        send(QEvent::KeyRelease, Qt::Key_Control, Qt::ControlModifier);
        QCOMPARE(edit->text(), QString("ctrl+alt+shift+f8"));
        // Nothing was written, so nothing is reported: the hotkeys are not registered again.
        QVERIFY(dialog.apply_settings());
        QCOMPARE(saved.size(), 0);
        QVERIFY(!dialog.hotkeys_saved());
        // An unfinished in-app shortcut writes nothing either.
        auto *inapp = dialog.inapp_edits.front().second;
        const QString inapp_saved = inapp->text();
        inapp->setText("ctrl+");
        QVERIFY(dialog.apply_settings());
        QCOMPARE(saved.size(), 0);
        inapp->setText(inapp_saved);
        QVERIFY(dialog.apply_settings());
        QCOMPARE(saved.size(), 0);

        auto *second = dialog.attr<HotkeyEdit>("translation_hotkey_edit_2");
        config_->set_global_hotkeys("translation", {"ctrl+alt+shift+f8", "ctrl+alt+shift+f8", "", ""});
        dialog.refresh_settings();
        config_->set_global_hotkeys("translation", {"ctrl+alt+shift+f8", "", "", ""});
        QVERIFY(dialog.apply_settings());
        QCOMPARE(config_->get_global_hotkeys("translation"), QStringList({"ctrl+alt+shift+f8", "", "", ""}));
        edit->setText("");
        QVERIFY(dialog.apply_settings());
        QCOMPARE(config_->get_global_hotkeys("translation"), QStringList({"", "ctrl+alt+shift+f8", "", ""}));
        QCOMPARE(saved.size(), 1);
        QVERIFY(dialog.hotkeys_saved());
        QCOMPARE(second->text(), QString("ctrl+alt+shift+f8"));
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

    // A mouse binding that collides with another is put back on its saved value, with a warning.
    void aCollidingMouseBindingIsPutBack() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.on_nav_changed(10, "mouse");
        auto *first = qobject_cast<MouseBindingEditor *>(dialog.behavior_control("mouse_pin_zoom"));
        auto *second = qobject_cast<MouseBindingEditor *>(dialog.behavior_control("mouse_pin_opacity"));
        QVERIFY(first && second);
        QVERIFY(!first->currentData().isEmpty());
        const QString saved = second->currentData();
        QVERIFY(saved != first->currentData());
        answer_next(true);
        second->setBinding(first->currentData());
        QVERIFY(dialog.apply_settings());
        QCOMPARE(second->currentData(), saved);
        QVERIFY(!dialog.saved_keys().contains("mouse_pin_opacity"));
        QVERIFY(!dialog.has_unsaved_changes());
    }

    // A window rebuilt for a language or scale change opens on the page and place the old one showed.
    void aRebuiltWindowKeepsThePage() {
        SettingsDialog::ViewState state;
        {
            SettingsDialog old_window(config_, config_->get_hotkey());
            old_window.show();
            old_window.on_nav_changed(5, "log");
            state = old_window.view_state();
            old_window.hide();
        }
        QCOMPARE(state.stack_index, 5);
        QCOMPARE(state.route_key, QString("log"));
        QVERIFY(state.position);
        SettingsDialog window(config_, config_->get_hotkey());
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.restore_view_state(state);
        // A hidden window opens the page when it shows.
        QCOMPARE(window.pos(), *state.position);
        QCOMPARE(window.content_stack->currentIndex(), 0);
        window.show();
        QCOMPARE(window.content_stack->currentIndex(), 5);
        state.stack_index = 7;
        window.restore_view_state(state);
        QCOMPARE(window.content_stack->currentIndex(), 7);
        window.hide();
    }

    // A window rebuilt on a page (language or scale change) paints nothing until that page is laid
    // out: show() paints at once, before the queued relayouts that size the page's groups.
    void aRestoredPageIsNotPaintedHalfLaidOut() {
        SettingsDialog window(config_, config_->get_hotkey());
        window.setAttribute(Qt::WA_DontShowOnScreen);
        SettingsDialog::ViewState state;
        state.stack_index = 6;
        state.route_key = "other";
        window.restore_view_state(state);
        const auto geometries = [&window] {
            QList<QRect> rects;
            for (auto *group : window.content_stack->currentWidget()->findChildren<SettingCardGroup *>())
                rects.append(QRect(group->mapTo(&window, QPoint(0, 0)), group->size()));
            return rects;
        };
        window.show();
        QCOMPARE(window.content_stack->currentIndex(), 6);
        QVERIFY(!window.updatesEnabled());
        QTRY_VERIFY(window.updatesEnabled());
        const auto at_first_paint = geometries();
        QVERIFY(!at_first_paint.isEmpty());
        QTest::qWait(300);
        QCOMPARE(at_first_paint, geometries());
        window.hide();
    }

    // Export writes what the pages read; import fills the controls and saves them.
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
        QVERIFY(!dialog.has_unsaved_changes());
        QCOMPARE(config_->get_log_level(), QString("DEBUG"));
        config_->set_log_level("ERROR");

        QFile broken(dir_.filePath("broken.json"));
        QVERIFY(broken.open(QIODevice::WriteOnly));
        broken.write("{\"format\": \"other\"}");
        broken.close();
        QVERIFY_THROWS_EXCEPTION(settings_transfer::SettingsFileError, settings_transfer::read_settings_file(broken.fileName()));
    }

    // No page has a control for the toolbar layouts; imported ones are written each to its own key.
    void importedToolbarLayoutsAreSaved() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        const QString screenshot_layout = R"([["pin", "show"], ["text", "hide"]])";
        const QString pin_layout = R"([["text", "show"], ["pen", "more"]])";
        // Keys missing from a file fall back to defaults, and another program may hold the default hotkeys.
        dialog.load_imported_settings({{"app/screenshot_toolbar_layout", screenshot_layout},
                                       {"pin/toolbar_layout", pin_layout},
                                       {"app/hotkey", config_->get_hotkey()},
                                       {"clipboard/hotkey", QString()}});
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

    void youdaoCredentialsReachTheProvider() {
        QVERIFY(config_->set_provider_field("youdao_translate_app_key", " app "));
        QVERIFY(config_->set_provider_field("youdao_translate_app_secret", " secret "));
        QCOMPARE(config_->get_provider_field("youdao_translate_app_key")->toString(), QString("app"));
        QCOMPARE(config_->get_translation_provider_config("youdao"), (QVariantMap{{"app_key", "app"}, {"app_secret", "secret"}}));
        config_->settings().remove("translation/providers/youdao");
        QCOMPARE(config_->get_translation_provider_config("youdao"), (QVariantMap{{"app_key", ""}, {"app_secret", ""}}));
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

    // "Reset This Page" asks, then puts the shown page back to the defaults and saves them.
    void resettingAPageRestoresItsDefaults() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.build_all_pages();
        QPushButton *reset = nullptr;
        for (auto *button : dialog.findChildren<QPushButton *>())
            if (button->text() == settings_tr("Reset This Page")) reset = button;
        QVERIFY(reset);
        // The information page has nothing to reset, so the button leaves the title row there.
        dialog.on_nav_changed(8, "about");
        QVERIFY(reset->isHidden());
        const auto &defaults = config_->app_defaults();
        dialog.on_nav_changed(0, "shortcuts");
        auto *hotkey = dialog.attr<HotkeyEdit>("hotkey_input");
        QVERIFY(hotkey);
        hotkey->setText("ctrl+alt+9");
        // Declining changes nothing.
        answer_next(false);
        reset->click();
        QCOMPARE(hotkey->text(), QString("ctrl+alt+9"));
        answer_next(true);
        reset->click();
        QCOMPARE(hotkey->text(), defaults.value("hotkey").toString());
        dialog.on_nav_changed(5, "log");
        auto *level = dialog.attr<QComboBox>("log_level_combo");
        QVERIFY(level);
        level->setCurrentIndex(level->findText(defaults.value("log_level").toString()) == 0 ? 1 : 0);
        answer_next(true);
        reset->click();
        QCOMPARE(level->currentText(), defaults.value("log_level").toString());
        QCOMPARE(config_->get_log_level(), defaults.value("log_level").toString());
        dialog.on_nav_changed(1, "capture");
        auto *format = dialog.attr<QComboBox>("screenshot_format_combo");
        QVERIFY(format);
        format->setCurrentIndex(format->count() - 1);
        answer_next(true);
        reset->click();
        QCOMPARE(format->currentText().toUpper(), defaults.value("screenshot_format").toString().toUpper());
        // Every other page resets too.
        const std::pair<int, const char *> pages[] = {{3, "appearance"}, {4, "translation"}, {6, "other"}, {9, "quick_actions"}, {10, "mouse"}, {11, "pin"},
                                                      {12, "screen_annotation"}};
        for (const auto &[index, route] : pages) {
            dialog.on_nav_changed(index, route);
            QCOMPARE(dialog.content_stack->currentIndex(), index);
            answer_next(true);
            reset->click();
        }
        // Unknown pages are ignored.
        dialog.on_nav_changed(42);
        QCOMPARE(dialog.content_stack->currentIndex(), 12);
    }

    // The screen annotation page: start, toolbar and cursor highlight settings saved and reset like the
    // others; keeping the drawings moved here from Quick Actions; the hotkey is shown and edited on the
    // shortcut page.
    void screenAnnotationPage() {
        config_->set_global_hotkeys("screen_annotation", {"ctrl+alt+f9", "", "", ""});
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        dialog.on_nav_changed(9, "quick_actions");
        QVERIFY(!dialog.behavior_control("screen_annotation_keep_drawings"));
        dialog.on_nav_changed(12, "screen_annotation");
        QCOMPARE(dialog.content_stack->currentIndex(), 12);
        const QStringList keys = {"screen_annotation_start_tool", "screen_annotation_keep_drawings", "screen_annotation_toolbar_position",
                                  "screen_annotation_cursor_highlight", "screen_annotation_highlight_size", "screen_annotation_click_ripples"};
        for (const auto &key : keys) QVERIFY2(dialog.behavior_control(key), qPrintable(key));
        auto *start = qobject_cast<QComboBox *>(dialog.behavior_control("screen_annotation_start_tool"));
        auto *size = qobject_cast<QComboBox *>(dialog.behavior_control("screen_annotation_highlight_size"));
        QVERIFY(start && size);
        QCOMPARE(start->count(), 3);
        start->setCurrentIndex(start->findData("last"));
        size->setCurrentIndex(size->findData("large"));
        QVERIFY(dialog.apply_settings());
        QCOMPARE(config_->get_app_setting("screen_annotation_start_tool").toString(), QString("last"));
        QCOMPARE(config_->get_app_setting("screen_annotation_highlight_size").toString(), QString("large"));
        reset_page(dialog);
        QCOMPARE(start->currentData().toString(), QString("cursor"));
        QCOMPARE(size->currentData().toString(), QString("medium"));
        QVERIFY(dialog.apply_settings());
        QCOMPARE(config_->get_app_setting("screen_annotation_start_tool").toString(), QString("cursor"));
        // The hotkey as set, then its row on the shortcut page.
        auto *hotkey_card = dialog.attr<SettingCard>("screen_annotation_hotkey_card");
        QVERIFY(hotkey_card);
        QVERIFY2(hotkey_card->findChildren<QLabel *>().size() > 0, "card labels");
        bool shown = false;
        for (auto *label : hotkey_card->findChildren<QLabel *>()) shown = shown || label->text().contains("Ctrl + Alt + F9");
        QVERIFY(shown);
        dialog.show_hotkey_row("screen_annotation_hotkey_edit");
        QCOMPARE(dialog.content_stack->currentIndex(), 0);
        QVERIFY(dialog.attr<HotkeyEdit>("screen_annotation_hotkey_edit"));
        config_->set_global_hotkeys("screen_annotation", {"", "", "", ""});
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
        reset_page(dialog);
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


    void inAppShortcutIconsRenderAtRowSize() {
        for (const auto &key : INAPP_KEYS()) {
            const auto source = inapp_icon(key.cfg_key);
            QIcon icon;
            if (const auto *name = std::get_if<QString>(&source)) icon = ResourceManager::get_icon_by_name(*name, 19);
            else if (const auto *fluent = std::get_if<FluentIcon>(&source)) icon = fluent_icon(*fluent);
            QVERIFY2(!icon.isNull(), qPrintable(key.cfg_key));
            const auto image = icon.pixmap(19, 19).toImage();
            QVERIFY2(!image.isNull(), qPrintable(key.cfg_key));
            bool visible = false;
            for (int y = 0; y < image.height() && !visible; ++y)
                for (int x = 0; x < image.width(); ++x)
                    if (qAlpha(image.pixel(x, y)) > 0) {
                        visible = true;
                        break;
                    }
            QVERIFY2(visible, qPrintable(key.cfg_key));
        }
    }

    void undoAndRedoBelongToAnnotationTools() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        auto *stack = dialog.findChild<QStackedWidget *>("InAppShortcutStack");
        QVERIFY(stack);
        auto *screenshot_page = stack->widget(0);
        auto *tools_page = stack->widget(1);
        QVERIFY(screenshot_page && tools_page);
        for (const char *key : {"inapp_undo", "inapp_redo"}) {
            auto *edit = dialog.inapp_edit(key);
            QVERIFY(edit);
            QVERIFY(tools_page->isAncestorOf(edit));
            QVERIFY(!screenshot_page->isAncestorOf(edit));
            QCOMPARE(edit->text(), config_->get_inapp_shortcut(key));
            QCOMPARE(dialog.inapp_groups.value(key), QString("screenshot"));
            int count = 0;
            for (const auto &entry : INAPP_KEYS())
                if (entry.cfg_key == key) ++count;
            QCOMPARE(count, 1);
        }
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
        const QString saved = second->text();
        answer_next(false);
        second->setText("ctrl+alt+k");
        QCOMPARE(first->text(), QString("ctrl+alt+k"));
        QCOMPARE(second->text(), saved);
        // Accepting moves the shortcut: the other one is cleared.
        answer_next(true);
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

    // Each provider's form tests the values it shows and fetches model lists in the background;
    // nothing here reaches the network: DeepL lacks a key and the model list goes to a closed local port.
    void translationFormsTestAndFetchTheShownValues() {
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
        // Like any edit, the address is saved once typing pauses.
        QTRY_COMPARE(config_->get_provider_field("custom_llm_base_url")->toString(), QString("http://127.0.0.1:9/v1"));
    }
    // Saves each page as PNG for visual review when JIETUBA_SETTINGS_SHOTS names a folder;
    // needs -platform windows for real fonts.
    void renderPagesForReview() {
        const QString out = qEnvironmentVariable("JIETUBA_SETTINGS_SHOTS");
        if (out.isEmpty()) QSKIP("JIETUBA_SETTINGS_SHOTS is not set.");
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.setAttribute(Qt::WA_DontShowOnScreen);
        dialog.show();
        const std::pair<int, const char *> pages[] = {{0, "shortcuts"}, {10, "mouse"}, {9, "quick_actions"}, {1, "capture"}, {11, "pin"}, {12, "screen_annotation"},
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
