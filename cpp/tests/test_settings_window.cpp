#include "core/data_location.h"
#include "core/file_names.h"
#include "core/save.h"
#include "ui/settings_ui/file_name_dialog.h"
#include "settings/data_folder.h"
#include "core/elevation.h"
#include "core/i18n.h"
#include "core/package_identity.h"
#include "core/resource_manager.h"
#include "ocr/ocr_manager.h"
#include "ocr/ppocr_models.h"
#include "settings/settings_transfer.h"
#include "settings/tool_settings.h"
#include "translation/speech_service.h"
#include "core/ui_scale.h"
#include "core/updater_process.h"
#include "ui/color_picker_dialog.h"
#include "ui/dialogs.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/cards.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/labels.h"
#include "ui/fluent_lite/navigation.h"
#include "ui/fluent_lite/segmented.h"
#include "ui/fluent_lite/titlebar.h"
#include "ui/hotkey_edit.h"
#include "ui/inapp_key_edit.h"
#include "ui/settings_ui/dialog.h"
#include "ui/settings_ui/mouse_binding_editor.h"
#include "ui/settings_ui/page_outline.h"
#include "ui/settings_ui/components.h"
#include "ui/settings_ui/pages.h"
#include "ui/settings_ui/search.h"
#include "ui/settings_ui/color_format_dialog.h"
#include "ui/settings_ui/ppocr_model_dialog.h"
#include "ui/settings_ui/special_paste_dialog.h"
#include "ui/settings_ui/upload_target_dialog.h"
#include "ui/toolbar_layout_dialog.h"
#include "upload/s3_target.h"
#include "ui/toolbar_layout.h"
#include "ui/welcome/demo_paint.h"
#include "ui/welcome/pages.h"
#include "ui/welcome/wizard.h"
#include "caption_check.h"
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
#include <QInputMethodEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSemaphore>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTest>
#include <algorithm>
#include <atomic>

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
        for (auto *box : modal->findChildren<jietuba::DialogButtonRow *>())
            for (auto *button : box->buttons())
                if (box->buttonRole(button) == QDialogButtonBox::YesRole || box->buttonRole(button) == QDialogButtonBox::AcceptRole) {
                    button->click();
                    return;
                }
        modal->accept();
    });
    timer->start(20);
}

// Runs act on the upload target dialog once it opens; the dialog is closed afterwards if act left it open.
void drive_upload_dialog(std::function<void(UploadTargetDialog *)> act) {
    auto *timer = new QTimer;
    QElapsedTimer waited;
    waited.start();
    QObject::connect(timer, &QTimer::timeout, [timer, act, waited] {
        auto *dialog = qobject_cast<UploadTargetDialog *>(QApplication::activeModalWidget());
        if (!dialog && waited.elapsed() < 3000) return;
        timer->stop();
        timer->deleteLater();
        if (!dialog) return;
        act(dialog);
        if (dialog->isVisible()) dialog->reject();
    });
    timer->start(20);
}

void fill_upload_target(UploadTargetDialog *dialog, const QString &name, const QString &bucket) {
    dialog->name_edit->setText(name);
    dialog->bucket_edit->setText(bucket);
    dialog->access_key_edit->setText("AKID");
    dialog->secret_edit->setText("secret");
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

// The search outline laid over target, if any.
QWidget *flash_over(QWidget *target) {
    for (auto *flash : target->parentWidget()->findChildren<QWidget *>("SettingsSearchFlash", Qt::FindDirectChildrenOnly))
        if (flash->geometry().contains(target->geometry())) return flash;
    return nullptr;
}

// A provider's chip in the translation page's API services group.
QAbstractButton *provider_chip(SettingsDialog &dialog, const QString &provider_id) {
    for (auto *button : dialog.attr_widget("translation_provider_picker")->findChildren<QAbstractButton *>())
        if (button->property("provider_id").toString() == provider_id) return button;
    return nullptr;
}

// The button captioned text in a provider's card.
QPushButton *card_button(SettingsDialog &dialog, const QString &provider_id, const QString &text) {
    for (auto *button : dialog.provider_sections.value(provider_id)->findChildren<QPushButton *>())
        if (button->text() == text) return button;
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

// Reads nothing aloud; records what it was asked to read.
class RecordingSpeech : public SpeechBackend {
public:
    bool voices(std::vector<VoiceInfo> &found, QString &) override { return found = voice_list, true; }
    bool speak(const VoiceInfo &voice, const QString &text, const SpeechStyle &style, QString &) override {
        spoken.push_back({voice.name, text, style});
        done = false;
        return true;
    }
    bool purge(QString &) override { return done = true; }
    bool poll(SpeechStatus &status, QString &) override { return status = {done, 0, 0}, true; }
    bool close(QString &error) override { return purge(error); }
    struct Spoken {
        QString voice, text;
        SpeechStyle style;
    };
    std::vector<VoiceInfo> voice_list;
    std::vector<Spoken> spoken;
    bool done = true;
};

// Records the URLs the dialog asks the desktop to open.
// The groups a page lists in its outline: visible card groups and section cards, top to bottom.
void collect_page_groups(QWidget *root, std::vector<QWidget *> &out) {
    for (auto *child : root->findChildren<QWidget *>(Qt::FindDirectChildrenOnly)) {
        if (child->isHidden() && child->testAttribute(Qt::WA_WState_ExplicitShowHide)) continue;
        if (qobject_cast<SettingCardGroup *>(child) || qobject_cast<SectionCard *>(child)) out.push_back(child);
        else collect_page_groups(child, out);
    }
}

std::vector<QWidget *> page_groups(QScrollArea *scroll) {
    std::vector<QWidget *> groups;
    collect_page_groups(scroll->widget(), groups);
    std::stable_sort(groups.begin(), groups.end(), [scroll](QWidget *a, QWidget *b) {
        return a->mapTo(scroll->widget(), QPoint()).y() < b->mapTo(scroll->widget(), QPoint()).y();
    });
    return groups;
}

QString group_title_of(QWidget *group) {
    if (auto *card_group = qobject_cast<SettingCardGroup *>(group)) return card_group->titleLabel->text();
    return qobject_cast<SectionCard *>(group)->titleLabel->text();
}

int group_top_of(QScrollArea *scroll, QWidget *group) { return group->mapTo(scroll->widget(), QPoint()).y(); }

// The card group of the dialog's pages with this title, or null.
SettingCardGroup *group_titled(QWidget &dialog, const QString &title) {
    for (auto *group : dialog.findChildren<SettingCardGroup *>())
        if (group->titleLabel->text() == title) return group;
    return nullptr;
}

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

    // The image quality slider on the screenshot page: it shows the stored quality, a change is stored by
    // the autosave, and resetting the page brings the default back.
    void imageQualityCardStoresAndResets() {
        config_->set_screenshot_quality(70);
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.build_all_pages();
        auto *slider = dialog.attr<ValueSlider>("screenshot_quality_slider");
        QVERIFY(slider);
        QCOMPARE(slider->slider()->minimum(), 1);
        QCOMPARE(slider->slider()->maximum(), 100);
        QCOMPARE(slider->value(), 70);
        QCOMPARE(slider->valueLabel()->text(), QString("70"));
        slider->setValue(42);
        QCOMPARE(slider->valueLabel()->text(), QString("42"));
        QVERIFY(dialog.apply_settings());
        QCOMPARE(config_->get_screenshot_quality(), 42);
        dialog.on_nav_changed(1, "capture");
        reset_page(dialog);
        QCOMPARE(slider->value(), config_->app_defaults().value("screenshot_quality").toInt());
        QVERIFY(dialog.apply_settings());
        QCOMPARE(config_->get_screenshot_quality(), 85);
    }

    // The PNG compression and PDF page size cards on the screenshot page: their choices, stored by the autosave and reset with the page.
    void pngAndPdfCardsStoreAndReset() {
        config_->set_png_compression("medium");
        config_->set_pdf_page_size("image");
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.build_all_pages();
        auto *png = dialog.attr<QComboBox>("png_compression_combo");
        auto *pdf = dialog.attr<QComboBox>("pdf_page_size_combo");
        QVERIFY(png && pdf);
        QCOMPARE(png->count(), 3);
        QCOMPARE(pdf->count(), 3);
        QCOMPARE(png->currentData().toString(), QString("medium"));
        QCOMPARE(pdf->currentData().toString(), QString("image"));
        png->setCurrentIndex(png->findData("high"));
        pdf->setCurrentIndex(pdf->findData("a4_landscape"));
        QVERIFY(dialog.apply_settings());
        QCOMPARE(config_->get_png_compression(), QString("high"));
        QCOMPARE(config_->get_pdf_page_size(), QString("a4_landscape"));
        // A new window shows what was stored.
        {
            SettingsDialog again(config_, config_->get_hotkey());
            again.build_all_pages();
            QCOMPARE(again.attr<QComboBox>("png_compression_combo")->currentData().toString(), QString("high"));
            QCOMPARE(again.attr<QComboBox>("pdf_page_size_combo")->currentData().toString(), QString("a4_landscape"));
        }
        dialog.on_nav_changed(1, "capture");
        reset_page(dialog);
        QCOMPARE(png->currentData().toString(), QString("medium"));
        QCOMPARE(pdf->currentData().toString(), QString("image"));
        QVERIFY(dialog.apply_settings());
        QCOMPARE(config_->get_png_compression(), QString("medium"));
        QCOMPARE(config_->get_pdf_page_size(), QString("image"));
    }

    void everyPageBuilds() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.build_all_pages();
        for (int index = 0; index < 12; ++index)
            if (index != 9) QVERIFY2(qobject_cast<QScrollArea *>(dialog.content_stack->widget(index)), qPrintable(QString::number(index)));
        for (const char *name : {"hotkey_input", "save_toggle", "clipboard_hotkey_edit", "pin_clipboard_hotkey_edit", "clipboard_pick_combo", "clipboard_enabled_toggle",
                                 "clipboard_history_limit_spin", "special_paste_manage_btn", "_clip_theme_btn", "_clip_font_combo", "_clip_opacity_combo",
                                 "preload_clipboard_toggle"})
            QVERIFY2(dialog.has_attr(name), name);
    }

    void externalRefreshRunsInBackgroundAndSupersedesOldResults() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        struct QueryState {
            QSemaphore started, release;
            std::atomic_int calls{0};
            std::atomic_bool on_gui{false};
        };
        auto state = std::make_shared<QueryState>();
        const auto unblock = qScopeGuard([state] { state->release.release(); });
        auto *gui = QThread::currentThread();
        QList<int> results;
        bool applied_on_gui = false;
        const auto refresh = dialog.add_external_refresh([state, gui] {
            state->on_gui = QThread::currentThread() == gui;
            const int call = ++state->calls;
            state->started.release();
            if (call == 1) state->release.acquire();
            return call;
        }, [&](int result) {
            applied_on_gui = QThread::currentThread() == gui;
            results << result;
        });
        // Registering a page refresh starts its first query without another show or activation.
        QTRY_VERIFY(state->started.available() > 0);
        bool responsive = false;
        QTimer::singleShot(0, &dialog, [&] { responsive = true; });
        QTRY_VERIFY(responsive);
        for (int i = 0; i < 10; ++i) refresh();
        state->release.release();
        QTRY_COMPARE(results, QList<int>{2});
        QCOMPARE(state->calls.load(), 2);
        QVERIFY(!state->on_gui.load());
        QVERIFY(applied_on_gui);
        QVERIFY(!dialog.has_unsaved_changes());
    }

    void externalRefreshOutlivesAClosedWindowWithoutDeliveringToIt() {
        struct QueryState {
            QSemaphore started, release;
            std::atomic_bool finished{false};
        };
        auto state = std::make_shared<QueryState>();
        const auto unblock = qScopeGuard([state] { state->release.release(); });
        bool applied = false;
        auto dialog = std::make_unique<SettingsDialog>(config_, config_->get_hotkey());
        dialog->add_external_refresh([state] {
            state->started.release();
            state->release.acquire();
            state->finished = true;
            return true;
        }, [&](bool) { applied = true; });
        QTRY_VERIFY(state->started.available() > 0);
        dialog.reset();
        state->release.release();
        QTRY_VERIFY(state->finished.load());
        QCoreApplication::processEvents();
        QVERIFY(!applied);
    }

    void externalRefreshCanRetryAfterAQueryFailure() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        auto calls = std::make_shared<std::atomic_int>(0);
        int result = 0;
        const auto refresh = dialog.add_external_refresh([calls] {
            if (++*calls == 1) throw std::runtime_error("query unavailable");
            return 42;
        }, [&](int value) { result = value; });
        QTRY_COMPARE(calls->load(), 1);
        QTest::qWait(30);
        QCOMPARE(result, 0);
        refresh();
        QTRY_COMPARE(result, 42);
    }

    // Showing the window also activates it; the two must not run the query twice. Coming back
    // from another window reads again.
    void showingTheWindowQueriesOnce() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        auto calls = std::make_shared<std::atomic_int>(0);
        int applied = 0;
        dialog.add_external_refresh([calls] { return ++*calls; }, [&](int) { ++applied; });
        QTRY_COMPARE(applied, 1);

        dialog.show();
        QTRY_VERIFY(dialog.isActiveWindow());
        QTRY_COMPARE(applied, 2);
        QTest::qWait(100);
        QCOMPARE(calls->load(), 2);
        QCOMPARE(applied, 2);

        QWidget other;
        other.show();
        QTRY_VERIFY(!dialog.isActiveWindow());
        QTest::qWait(100);
        QCOMPARE(calls->load(), 2);
        dialog.activateWindow();
        QTRY_COMPARE(applied, 3);
        QCOMPARE(calls->load(), 3);
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

    // Whether the cards fit at this width is settings_window_desktop's check.
    void minimumWidthFollowsTheWindowScale() {
        auto &scale = get_dialog_scale();
        const int before = scale.percent(), panel_before = get_ui_scale().percent();
        const auto restore = qScopeGuard([&] {
            scale.set_percent(before);
            get_ui_scale().set_percent(panel_before);
        });
        // 561 outside the card and for its title, 348 for the rest of the widest row; at 150% each part is
        // rounded half to even.
        for (const auto &[percent, expected] : {std::pair{100, 909}, std::pair{150, 1364}, std::pair{200, 1818}}) {
            scale.set_percent(percent);
            get_ui_scale().set_percent(100);
            SettingsDialog dialog(config_, config_->get_hotkey());
            dialog.fit_to_available({2560, 1392});
            QCOMPARE(dialog.minimumWidth(), expected);
            dialog.resize(400, dialog.height());
            QCOMPARE(dialog.width(), expected);
            get_ui_scale().set_percent(150);
            dialog.fit_to_available({2560, 1392});
            QCOMPARE(dialog.minimumWidth(), expected);
        }

        // A narrower screen gets its whole width; a wider one brings the minimum back.
        scale.set_percent(150);
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.fit_to_available({1280, 672});
        QCOMPARE(dialog.minimumWidth(), 1280);
        QCOMPARE(dialog.width(), 1280);
        dialog.fit_to_available({2560, 1392});
        QCOMPARE(dialog.minimumWidth(), 1364);
        QCOMPARE(dialog.width(), 1575);
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
        QTRY_VERIFY(combo->property("queried").toBool());
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
        QTRY_VERIFY(combo->findData("ppocr_rust:new") >= 0);
        QCOMPARE(combo->currentData().toString(), QString("ppocr_rust:ppocr-v5-korean"));

        combo->setCurrentIndex(combo->findData("ppocr_rust:local"));
        emit combo->activated(combo->currentIndex());
        QVERIFY(dialog.apply_settings());
        QCOMPARE(config_->get_ocr_engine(), QString("ppocr_rust"));
        QCOMPARE(config_->get_ppocr_model(), QString("local"));
        // "Download more models..." is current while the model manager is open; a save then keeps the chosen model.
        combo->setCurrentIndex(combo->findData("manage_models"));
        dialog.apply_settings();
        QCOMPARE(config_->get_ocr_engine(), QString("ppocr_rust"));
        QCOMPARE(config_->get_ppocr_model(), QString("local"));
        combo->setCurrentIndex(combo->findData("ppocr_rust:local"));

        QVERIFY(QDir(root.filePath("local")).removeRecursively());
        emit model_downloads()->models_changed();
        QTRY_COMPARE(combo->currentText(), settings_tr("%1 (not installed)").replace("%1", "local"));
        QCOMPARE(combo->currentData().toString(), QString("ppocr_rust:local"));
        QCOMPARE(combo->currentText(), settings_tr("%1 (not installed)").replace("%1", "local"));
        QVERIFY(!items->item(combo->currentIndex())->isEnabled());
        QCOMPARE(ocr_choice("ppocr_rust:local"), (std::pair<QString, std::optional<QString>>{"ppocr_rust", QString("local")}));
        QCOMPARE(ocr_choice("oneocr"), (std::pair<QString, std::optional<QString>>{"oneocr", std::nullopt}));

        reset_page(dialog);
        QCOMPARE(combo->currentData().toString(), QString("auto"));
    }

    // The window is kept hidden between uses: a Snipping Tool installed or removed meanwhile is
    // listed or dropped when it comes back, and the unavailable card follows.
    void ocrListFollowsTheSnippingToolWhenShownAgain() {
        QTemporaryDir models;
        QVERIFY(models.isValid());
        ppocr_models::set_models_root_for_test(models.path());
        set_oneocr_available_for_test(false);
        const auto restore = qScopeGuard([] {
            ppocr_models::set_models_root_for_test({});
            set_oneocr_available_for_test(std::nullopt);
        });
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.on_nav_changed(1, "capture");
        dialog.show();
        auto *combo = dialog.attr<QComboBox>("ocr_engine_combo");
        auto *card = dialog.attr<QWidget>("ocr_unavailable_card");
        QVERIFY(combo && card);
        QTRY_VERIFY(combo->property("queried").toBool());
        QCOMPARE(combo->findData(OCRManager::ENGINE_ONEOCR), -1);
        QVERIFY(!card->isHidden());
        dialog.on_nav_changed(11, "pin");
        auto *ocr_toggle = dialog.attr<SwitchSettingCard>("ocr_enable_toggle");
        QVERIFY(ocr_toggle);
        QVERIFY(!ocr_toggle->isEnabled());
        QCOMPARE(ocr_toggle->isChecked(), config_->get_ocr_enabled());

        dialog.hide();
        set_oneocr_available_for_test(true);
        dialog.show();
        QTRY_COMPARE(combo->findData(OCRManager::ENGINE_ONEOCR), 1);
        QCOMPARE(combo->currentData().toString(), QString("auto"));
        QVERIFY(card->isHidden());
        QTRY_VERIFY(ocr_toggle->isEnabled());
        QVERIFY(!dialog.has_unsaved_changes());

        dialog.hide();
        set_oneocr_available_for_test(false);
        dialog.show();
        QTRY_COMPARE(combo->findData(OCRManager::ENGINE_ONEOCR), -1);
        QVERIFY(!card->isHidden());
        QTRY_VERIFY(!ocr_toggle->isEnabled());
        QCOMPARE(ocr_toggle->isChecked(), config_->get_ocr_enabled());
        QVERIFY(!dialog.has_unsaved_changes());
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

    // The hotkey's kinds have a group of their own, third to last, one switch each, with the
    // Explorer entry's group and the pin keys after it; formatting, which the clipboard window's
    // pins follow too, stays with the behaviour.
    void pinClipboardKindsAndFormattingSaveAndReset() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        dialog.on_nav_changed(11, "pin");
        auto *image = dialog.attr<SwitchSettingCard>("pin_clipboard_image_toggle");
        auto *text = dialog.attr<SwitchSettingCard>("pin_clipboard_text_toggle");
        auto *files = dialog.attr<SwitchSettingCard>("pin_clipboard_file_toggle");
        auto *formatting = dialog.attr<SwitchSettingCard>("pin_text_formatting_toggle");
        QVERIFY(image && text && files && formatting);
        const auto group_of = [](QWidget *widget) -> SettingCardGroup * {
            for (QWidget *w = widget; w; w = w->parentWidget())
                if (auto *group = qobject_cast<SettingCardGroup *>(w)) return group;
            return nullptr;
        };
        auto *own = group_of(image);
        QVERIFY(own && group_of(text) == own && group_of(files) == own);
        auto groups = own->parentWidget()->findChildren<SettingCardGroup *>(Qt::FindDirectChildrenOnly);
        std::sort(groups.begin(), groups.end(), [](QWidget *a, QWidget *b) { return a->y() < b->y(); });
        QCOMPARE(groups.indexOf(own), groups.size() - 3);
        QVERIFY(!groups.last()->findChildren<QPushButton *>().isEmpty());
        QVERIFY(group_of(formatting) == group_of(dialog.attr_widget("pin_auto_toolbar_toggle")));
        // Stacked, one row per kind.
        QVERIFY(text->mapTo(own, QPoint()).y() > image->mapTo(own, QPoint()).y());
        QVERIFY(files->mapTo(own, QPoint()).y() > text->mapTo(own, QPoint()).y());
        QCOMPARE(text->switchButton->mapTo(own, QPoint()).x(), image->switchButton->mapTo(own, QPoint()).x());
        QCOMPARE(files->switchButton->mapTo(own, QPoint()).x(), image->switchButton->mapTo(own, QPoint()).x());
        // All on by default, each turned off on its own.
        QVERIFY(image->isChecked() && text->isChecked() && files->isChecked());
        image->setChecked(false);
        files->setChecked(false);
        QVERIFY(text->isChecked() && text->isEnabled());
        formatting->setChecked(false);
        QVERIFY(dialog.apply_settings());
        QVERIFY(!config_->get_pin_clipboard_image());
        QVERIFY(!config_->get_pin_clipboard_file());
        QVERIFY(config_->get_pin_clipboard_text());
        QVERIFY(!config_->get_pin_text_formatting());
        {
            SettingsDialog again(config_, config_->get_hotkey());
            again.on_nav_changed(11, "pin");
            auto *loaded = again.attr<SwitchSettingCard>("pin_clipboard_image_toggle");
            QVERIFY(loaded && !loaded->isChecked());
        }
        text->setChecked(false);
        reset_page(dialog);
        QVERIFY(image->isChecked() && text->isChecked() && files->isChecked());
        QVERIFY(formatting->isChecked());
        QVERIFY(dialog.apply_settings());
        QVERIFY(config_->get_pin_clipboard_image());
        QVERIFY(config_->get_pin_clipboard_text());
        QVERIFY(config_->get_pin_clipboard_file());
        QVERIFY(config_->get_pin_text_formatting());
        dialog.hide();
    }

    // Select After Drawing folds its switch rows, one per tool, into a single row whose summary names
    // what is off; each tool saves on its own and the screenshot page resets them.
    void autoSelectRowsFoldSummarizeSaveAndReset() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        dialog.on_nav_changed(1, "capture");
        auto *summary = dialog.attr<QLabel>("auto_select_summary");
        QList<SwitchSettingCard *> rows;
        for (const auto &tool : AUTO_SELECT_TOOLS) rows << dialog.attr<SwitchSettingCard>(auto_select_setting(tool.tool_id) + "_toggle");
        QVERIFY(summary && !rows.contains(nullptr));
        auto *number = dialog.attr<SwitchSettingCard>("auto_select_number_toggle");
        auto *rect = dialog.attr<SwitchSettingCard>("auto_select_rect_toggle");
        auto *expander = qobject_cast<SettingExpander *>(number->parentWidget()->parentWidget());
        QVERIFY(expander && !expander->isExpanded());
        QVERIFY(summary->isVisible() && !number->isVisible());
        expander->arrow()->click();
        QVERIFY(number->isVisible());

        for (auto *row : rows) QVERIFY(row->isChecked());
        QCOMPARE(summary->text(), settings_tr("All On"));
        number->setChecked(false);
        QCOMPARE(summary->text(), settings_tr("Off: %1").arg(settings_tr("Number")));
        rect->setChecked(false);
        QCOMPARE(summary->text(), QCoreApplication::translate("SettingsDialog", "%n Off", nullptr, 2));
        QVERIFY(dialog.apply_settings());
        QVERIFY(!config_->get_auto_select_number());
        QVERIFY(!config_->get_auto_select_rect());
        QVERIFY(config_->get_auto_select_arrow() && config_->get_auto_select_ellipse());
        {
            SettingsDialog again(config_, config_->get_hotkey());
            again.on_nav_changed(1, "capture");
            QVERIFY(!again.attr<SwitchSettingCard>("auto_select_number_toggle")->isChecked());
            QCOMPARE(again.attr<QLabel>("auto_select_summary")->text(), QCoreApplication::translate("SettingsDialog", "%n Off", nullptr, 2));
        }
        for (auto *row : rows) row->setChecked(false);
        QCOMPARE(summary->text(), settings_tr("All Off"));
        // Sized for its longest text from the start, so the control column never cuts it short.
        QVERIFY(summary->width() >= summary->sizeHint().width());
        reset_page(dialog);
        for (auto *row : rows) QVERIFY(row->isChecked());
        QCOMPARE(summary->text(), settings_tr("All On"));
        QVERIFY(dialog.apply_settings());
        QVERIFY(config_->get_auto_select_number());
        dialog.hide();
    }

    // The Explorer entry's group follows the clipboard hotkey's; its choice saves and resets with the page.
    void explorerMenuChoiceSavesAndResets() {
        config_->set_explorer_context_menu("off");
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        dialog.on_nav_changed(11, "pin");
        auto *combo = qobject_cast<QComboBox *>(dialog.behavior_control("explorer_context_menu"));
        QVERIFY(combo);
        QStringList modes;
        for (int i = 0; i < combo->count(); ++i) modes << combo->itemData(i).toString();
        QCOMPARE(modes, QStringList({"off", "images", "images_text"}));
        QCOMPARE(combo->itemText(0), settings_tr("Off"));
        QCOMPARE(combo->itemText(1), settings_tr("Images"));
        QCOMPARE(combo->itemText(2), settings_tr("Images and Text Files"));
        QCOMPARE(combo->currentData().toString(), QString("off"));
        const auto group_of = [](QWidget *widget) -> SettingCardGroup * {
            for (QWidget *w = widget; w; w = w->parentWidget())
                if (auto *group = qobject_cast<SettingCardGroup *>(w)) return group;
            return nullptr;
        };
        auto *own = group_of(combo);
        QVERIFY(own);
        QCOMPARE(own->titleLabel->text(), settings_tr("File Explorer"));
        auto groups = own->parentWidget()->findChildren<SettingCardGroup *>(Qt::FindDirectChildrenOnly);
        std::sort(groups.begin(), groups.end(), [](QWidget *a, QWidget *b) { return a->y() < b->y(); });
        QCOMPARE(groups[groups.indexOf(own) - 1]->titleLabel->text(), settings_tr("Pin from Clipboard"));
        // Nothing is written until the choice changes.
        QVERIFY(dialog.apply_settings());
        QVERIFY(dialog.saved_keys().isEmpty());
        combo->setCurrentIndex(combo->findData(QString("images_text")));
        QVERIFY(dialog.apply_settings());
        QCOMPARE(config_->get_explorer_context_menu(), QString("images_text"));
        QVERIFY(dialog.saved_keys().contains("explorer_context_menu"));
        {
            SettingsDialog again(config_, config_->get_hotkey());
            again.on_nav_changed(11, "pin");
            auto *loaded = qobject_cast<QComboBox *>(again.behavior_control("explorer_context_menu"));
            QVERIFY(loaded);
            QCOMPARE(loaded->currentData().toString(), QString("images_text"));
        }
        reset_page(dialog);
        QCOMPARE(combo->currentData().toString(), QString("off"));
        QVERIFY(dialog.apply_settings());
        QCOMPARE(config_->get_explorer_context_menu(), QString("off"));
        QVERIFY(dialog.saved_keys().contains("explorer_context_menu"));
        dialog.hide();
    }

    // Windows 11 hides a registry entry under "Show more options"; the packaged build's entry is on the first level.
    void explorerMenuCardTellsWhereTheEntryIsPerBuild() {
        const auto description = [this](bool packaged) {
            set_packaged_for_test(packaged);
            SettingsDialog dialog(config_, config_->get_hotkey());
            dialog.on_nav_changed(11, "pin");
            for (QWidget *w = dialog.behavior_control("explorer_context_menu"); w; w = w->parentWidget())
                if (auto *card = qobject_cast<SettingCard *>(w)) return card->contentLabel->text();
            return QString();
        };
        const auto restore = qScopeGuard([] { set_packaged_for_test(std::nullopt); });
        const QString portable = description(false), packaged = description(true);
        QCOMPARE(portable, settings_tr("Right-click files in File Explorer and choose \"Pin to Screen with jietuba\". Windows 11 lists it under Show more options."));
        QCOMPARE(packaged, settings_tr("Right-click files in File Explorer and choose \"Pin to Screen with jietuba\"."));
        QVERIFY(portable != packaged);
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

    // What people change most comes first: the capture itself and where it is saved. Each recognition
    // switch sits in the OCR group, and the capture-time switches in the first group.
    void theCapturePageGroupsFollowTheOrderOfUse() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        dialog.on_nav_changed(1, "capture");
        const auto group_of = [](QWidget *widget) -> SettingCardGroup * {
            for (QWidget *w = widget; w; w = w->parentWidget())
                if (auto *group = qobject_cast<SettingCardGroup *>(w)) return group;
            return nullptr;
        };
        const auto behavior_control = [&dialog](const QString &key) -> QWidget * {
            for (const auto &[name, widget] : dialog.behavior_controls)
                if (name == key) return widget;
            return nullptr;
        };
        const QList<QWidget *> markers = {dialog.attr_widget("capture_engine_combo"), dialog.attr_widget("save_toggle"), dialog.attr_widget("cross_tool_selection_toggle"),
                                          dialog.attr_widget("smart_mode_combo"), dialog.attr_widget("magnifier_enabled_toggle"), dialog.attr_widget("ocr_engine_combo"),
                                          behavior_control("recording_hide_toolbar_in_captures"), dialog.attr_widget("watermark_auto_toggle")};
        QList<int> positions;
        for (auto *marker : markers) {
            QVERIFY(marker);
            auto *group = group_of(marker);
            QVERIFY(group);
            positions << group->parentWidget()->layout()->indexOf(group);
        }
        for (int i = 1; i < positions.size(); ++i) QVERIFY2(positions[i - 1] < positions[i], qPrintable(QString("group %1 is not below group %2").arg(i).arg(i - 1)));
        QCOMPARE(group_of(behavior_control("capture_include_cursor")), group_of(markers[0]));
        QCOMPARE(group_of(behavior_control("capture_fullscreen_crosshair")), group_of(markers[0]));
        QCOMPARE(group_of(dialog.attr_widget("ocr_copy_directly_toggle")), group_of(markers[5]));
        QCOMPARE(group_of(dialog.attr_widget("barcode_copy_single_toggle")), group_of(markers[5]));
        dialog.hide();
    }

    // The picker style is an appearance choice; the theme and mask colours open the picker it names.
    void colorPickerStyleLoadsSavesAndPicksAppearanceColours() {
        const auto restore = qScopeGuard([this] { config_->set_app_setting("color_picker_style", "modern"); });
        config_->set_app_setting("color_picker_style", "system");
        SettingsDialog dialog(config_, config_->get_hotkey());
        QVERIFY(settings_transfer::IMPORTED_KEYS().contains("app/color_picker_style"));
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

    // New settings take Ctrl+1 to Ctrl+5; settings from earlier versions keep the hotkeys they had.
    void olderSettingsKeepTheirHotkeyDefaults() {
        const auto open = [&](const QString &name, const std::function<void(QSettings &)> &before) {
            QFile::remove(dir_.filePath(name));
            auto settings = std::make_unique<QSettings>(dir_.filePath(name), QSettings::IniFormat);
            before(*settings);
            return settings;
        };
        const auto hotkeys = [](ToolSettingsManager &config) {
            return QStringList{config.get_hotkey(), config.get_pin_clipboard_hotkey(), config.get_clipboard_hotkey(),
                               config.get_translation_hotkey(), config.get_screen_annotation_hotkey()};
        };
        {
            auto fresh = open("fresh.ini", [](QSettings &) {});
            ToolSettingsManager config(*fresh);
            config.keep_older_hotkey_defaults();
            QCOMPARE(hotkeys(config), QStringList({"ctrl+1", "ctrl+2", "ctrl+3", "ctrl+4", "ctrl+5"}));
            QVERIFY(!fresh->contains("clipboard/hotkey"));
        }
        // Run before, or any setting stored: an earlier version. What the user set stays.
        for (const auto &before : std::vector<std::function<void(QSettings &)>>{
                 [](QSettings &s) { s.setValue("app/has_run_before", true); }, [](QSettings &s) { s.setValue("app/translation_hotkey", "f3"); }}) {
            auto older = open("older.ini", before);
            ToolSettingsManager config(*older);
            const QString translation = config.get_translation_hotkey() == "f3" ? "f3" : "";
            config.keep_older_hotkey_defaults();
            QCOMPARE(hotkeys(config), QStringList({"ctrl+1", "", "ctrl+2", translation, ""}));
            // Once only: a hotkey removed later takes the new default.
            older->remove("clipboard/hotkey");
            config.keep_older_hotkey_defaults();
            QCOMPARE(config.get_clipboard_hotkey(), QString("ctrl+3"));
        }
    }

    // Until the third clipboard slot is saved it follows the old switch, without writing.
    void winVMigrationFollowsTheOldSwitch() {
        // A repeated run (check.ps1 -Mode Leak) would find the slots the first run saved.
        QFile::remove(dir_.filePath("migration.ini"));
        QSettings settings(dir_.filePath("migration.ini"), QSettings::IniFormat);
        ToolSettingsManager config(settings);
        QCOMPARE(config.get_global_hotkeys("clipboard"), QStringList({"ctrl+3", "", "win+v", ""}));
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

    // Off by default; saved, reloaded, imported and conflict-checked like the other actions.
    void globalMouseScanBindingPersistsAndRejectsConflicts() {
        const auto saved = config_->get_quick_capture_scan_code();
        const auto restore = qScopeGuard([&] { config_->set_quick_capture_scan_code(saved); });
        config_->set_quick_capture_scan_code("");
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.on_nav_changed(10, "mouse");
        auto *scan = qobject_cast<MouseBindingEditor *>(dialog.behavior_control("quick_capture_scan_code"));
        QVERIFY(scan);
        QVERIFY(scan->currentData().isEmpty());
        QVERIFY(settings_transfer::IMPORTED_KEYS().contains("app/quick_capture_scan_code"));
        QVERIFY(!ResourceManager::get_icon_by_name(QString::fromUtf8("扫码.svg")).isNull());
        scan->setBinding("ctrl+alt+dragright");
        QVERIFY(dialog.apply_settings());
        QCOMPARE(config_->get_quick_capture_scan_code(), QString("ctrl+alt+dragright"));
        settings_->sync();
        QSettings reloaded(settings_->fileName(), QSettings::IniFormat);
        ToolSettingsManager fresh(reloaded);
        QCOMPARE(get_quick_capture_binding(&fresh, "scan_code"), QString("ctrl+alt+dragright"));
        const auto bindings = get_quick_capture_bindings(&fresh);
        const auto found = std::find_if(bindings.begin(), bindings.end(), [](const auto &entry) { return entry.second == "scan_code"; });
        QVERIFY(found != bindings.end());
        QCOMPARE(found->first.first, (QStringList{"alt", "ctrl"}));
        QCOMPARE(found->first.second, QString("right"));

        auto *pin = qobject_cast<MouseBindingEditor *>(dialog.behavior_control("quick_capture_pin"));
        QVERIFY(pin && !pin->currentData().isEmpty());
        answer_next(true);
        scan->setBinding(pin->currentData());
        QVERIFY(dialog.apply_settings());
        QCOMPARE(scan->currentData(), QString("ctrl+alt+dragright"));
        QCOMPARE(config_->get_quick_capture_scan_code(), QString("ctrl+alt+dragright"));
        scan->setBinding("");
        QVERIFY(dialog.apply_settings());
        for (const auto &binding : get_quick_capture_bindings(config_)) QVERIFY(binding.second != "scan_code");
    }

    void globalLongScreenshotDefaultsPreserveOlderMouseBindings() {
        QSettings settings(dir_.filePath("long-shortcut-defaults.ini"), QSettings::IniFormat);
        settings.clear();
        ToolSettingsManager config(settings);
        QVERIFY(config.get_quick_capture_long_screenshot().isEmpty());
        config.set_quick_capture_copy("WIN+dragmiddle");
        QVERIFY(config.get_quick_capture_long_screenshot().isEmpty());
        QCOMPARE(config.get_quick_capture_copy(), QString("WIN+dragmiddle"));
        const auto bindings = get_quick_capture_bindings(&config);
        QVERIFY(std::any_of(bindings.begin(), bindings.end(), [](const auto &b) { return b.second == "copy"; }));
        QVERIFY(std::none_of(bindings.begin(), bindings.end(), [](const auto &b) { return b.second == "long_screenshot"; }));
        SettingsDialog dialog(&config, config.get_hotkey());
        dialog.on_nav_changed(10, "mouse");
        QVERIFY(mouse_binding_conflicts(&dialog).empty());
        QVERIFY(qobject_cast<MouseBindingEditor *>(dialog.behavior_control("quick_capture_long_screenshot"))->currentData().isEmpty());
        dialog.refresh_settings();
        QVERIFY(qobject_cast<MouseBindingEditor *>(dialog.behavior_control("quick_capture_long_screenshot"))->currentData().isEmpty());
        QCOMPARE(qobject_cast<MouseBindingEditor *>(dialog.behavior_control("quick_capture_copy"))->currentData(), QString("win+dragmiddle"));
    }

    void longScreenshotMouseBindingsAreIndependentAndPersist() {
        QSettings settings(dir_.filePath("long-mouse.ini"), QSettings::IniFormat);
        settings.clear();
        ToolSettingsManager config(settings);
        SettingsDialog dialog(&config, config.get_hotkey());
        dialog.on_nav_changed(10, "mouse");
        QCOMPARE(LONG_SCREENSHOT_MOUSE_ACTIONS().size(), size_t(5));
        for (const auto &action : LONG_SCREENSHOT_MOUSE_ACTIONS()) {
            QVERIFY(get_long_screenshot_mouse_binding(&config, action.key).isEmpty());
            auto *edit = qobject_cast<MouseBindingEditor *>(dialog.behavior_control(QString("mouse_long_") + action.key));
            QVERIFY(edit && edit->currentData().isEmpty());
            QVERIFY(settings_transfer::IMPORTED_KEYS().contains(QString("app/mouse_long_") + action.key));
        }
        auto *pin = qobject_cast<MouseBindingEditor *>(dialog.behavior_control("mouse_long_pin"));
        pin->setBinding("ctrl+middle");
        QVERIFY(dialog.apply_settings());
        QCOMPARE(config.get_mouse_long_pin(), QString("ctrl+middle"));
        QCOMPARE(config.get_mouse_capture_copy(), QString("doubleleft"));
        QCOMPARE(config.get_mouse_capture_close(), QString("right"));
        settings.sync();
        QSettings reopened(settings.fileName(), QSettings::IniFormat);
        ToolSettingsManager fresh(reopened);
        QCOMPARE(get_long_screenshot_mouse_binding(&fresh, "pin"), QString("ctrl+middle"));
        const auto imported = settings_transfer::read_settings_file(settings.fileName());
        QCOMPARE(imported.value("app/mouse_long_pin").toString(), QString("ctrl+middle"));
        auto *save = qobject_cast<MouseBindingEditor *>(dialog.behavior_control("mouse_long_save"));
        save->setBinding("ctrl+middle");
        QVERIFY(!mouse_binding_conflicts(&dialog).empty());
        answer_next(true);
        QVERIFY(dialog.apply_settings());
        QVERIFY(save->currentData().isEmpty());
        QVERIFY(config.get_mouse_long_save().isEmpty());
        QCOMPARE(config.get_mouse_long_pin(), QString("ctrl+middle"));
        pin->setBinding("");
        QVERIFY(dialog.apply_settings());
        settings.sync();
        QSettings cleared(settings.fileName(), QSettings::IniFormat);
        ToolSettingsManager empty(cleared);
        QVERIFY(empty.get_mouse_long_pin().isEmpty());
    }

    void scanActionIsTranslatedInSettings() {
        const auto language = I18nManager::get_current_language();
        const auto restore = qScopeGuard([&] { I18nManager::load_language(language); });
        for (const auto *code : {"zh", "zh_TW", "ja", "ko"}) {
            QVERIFY(I18nManager::load_language(code));
            const auto label = settings_tr("Scan QR code / barcode");
            QVERIFY(label != QString("Scan QR code / barcode"));
            QVERIFY(!label.isEmpty());
        }
    }

    // Each language names the window in its own words: Japanese and Korean call the full-screen capture a screen.
    void shortcutGroupTranslationsHaveExpectedText() {
        const auto language = I18nManager::get_current_language();
        const auto restore = qScopeGuard([&] { I18nManager::load_language(language); });
        const QMap<QString, QStringList> expected{
            {"zh", {QString::fromUtf8("截图窗口"), QString::fromUtf8("长截图窗口"), QString::fromUtf8("钉图窗口"), QString::fromUtf8("剪贴板窗口"), QString::fromUtf8("标注工具")}},
            {"zh_TW", {QString::fromUtf8("截圖視窗"), QString::fromUtf8("長截圖視窗"), QString::fromUtf8("釘圖視窗"), QString::fromUtf8("剪貼簿視窗"), QString::fromUtf8("標註工具")}},
            {"en", {"Screenshot Window", "Long Capture Window", "Pinned Window", "Clipboard Window", "Annotation Tools"}},
            {"ja", {QString::fromUtf8("キャプチャウィンドウ"), QString::fromUtf8("スクロールキャプチャ"), QString::fromUtf8("ピン留めウィンドウ"), QString::fromUtf8("クリップボード画面"), QString::fromUtf8("注釈ツール")}},
            {"ko", {QString::fromUtf8("캡처 화면"), QString::fromUtf8("스크롤 캡처 화면"), QString::fromUtf8("고정 창"), QString::fromUtf8("클립보드 창"), QString::fromUtf8("주석 도구")}},
        };
        const QStringList sources{"Screenshot Window", "Long Screenshot Window", "Pinned Window", "Clipboard Window", "Annotation Tools"};
        for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
            QVERIFY(I18nManager::load_language(it.key()));
            for (int i = 0; i < sources.size(); ++i)
                QCOMPARE(i18n_tr(sources[i], "ShortcutTabs"), it.value()[i]);
        }
        // The tray keeps the full Japanese name; the group, also a navigation item, has a shorter one.
        QVERIFY(I18nManager::load_language("ja"));
        QCOMPARE(i18n_tr("Global Mouse Shortcuts", "ShortcutTabs"), QString::fromUtf8("グローバルマウス操作"));
        QCOMPARE(settings_tr("Global Mouse Shortcuts"), QString::fromUtf8("グローバルマウスショートカット"));
    }

    void longScreenshotMousePageFitsEveryLanguage() {
        if (QGuiApplication::platformName() != "windows") QSKIP("needs desktop fonts");
        const auto language = I18nManager::get_current_language();
        const int scale = get_dialog_scale().percent();
        const auto restore = qScopeGuard([&] { get_dialog_scale().set_percent(scale); I18nManager::load_language(language); });
        for (int percent : {100, 150, 200}) {
            get_dialog_scale().set_percent(percent);
            for (const auto *code : {"zh", "zh_TW", "en", "ja", "ko"}) {
                QVERIFY(I18nManager::load_language(code));
                SettingsDialog dialog(config_, config_->get_hotkey());
                dialog.fit_to_available({4000, dialog.height()});
                dialog.on_nav_changed(10, "mouse");
                dialog.resize(0, dialog.height());
                dialog.show();
                QApplication::processEvents();
                const auto shots = qEnvironmentVariable("JIETUBA_SETTINGS_SHOTS");
                if (!shots.isEmpty() && percent == 100)
                    QVERIFY(dialog.grab().save(QDir(shots).filePath(QString("long_mouse_%1.png").arg(code))));
                for (const int page : {10, 0}) {
                    dialog.on_nav_changed(page, page == 10 ? "mouse" : "shortcuts");
                    QApplication::processEvents();
                    if (page == 0 && !shots.isEmpty() && percent == 100)
                        QVERIFY(dialog.grab().save(QDir(shots).filePath(QString("keyboard_%1.png").arg(code))));
                    auto *scroll = qobject_cast<QScrollArea *>(dialog.content_stack->currentWidget());
                    QVERIFY(scroll);
                    const auto where = QString("%1 at %2%%, page %3: width %4, overflow %5").arg(code).arg(percent).arg(page).arg(scroll->widget()->width()).arg(scroll->horizontalScrollBar()->maximum());
                    QVERIFY2(scroll->horizontalScrollBar()->maximum() == 0, qPrintable(where));
                    for (auto *group : page_groups(scroll)) {
                        for (auto *label : group->findChildren<QLabel *>()) {
                            if (!label->isVisible() || label->wordWrap()) continue;
                            QVERIFY2(label->width() >= label->sizeHint().width(), qPrintable(where + ": " + label->text()));
                        }
                        // A wrapped caption or row title gets the height of its lines.
                        for (auto *label : group->findChildren<QLabel *>()) {
                            if (!label->isVisible() || !label->wordWrap()) continue;
                            QVERIFY2(label->height() >= label->heightForWidth(label->width()), qPrintable(where + ": " + label->text()));
                        }
                    }
                }
                QVERIFY(group_titled(dialog, i18n_tr("Long Screenshot Window", "ShortcutTabs")));
                dialog.on_nav_changed(10, "mouse");
                QApplication::processEvents();
                for (const auto &action : LONG_SCREENSHOT_MOUSE_ACTIONS()) {
                    auto *edit = dialog.behavior_control(QString("mouse_long_") + action.key);
                    QVERIFY(edit && edit->isVisible());
                    QVERIFY2(edit->width() >= edit->minimumSizeHint().width(), qPrintable(QString("%1 at %2%%").arg(code).arg(percent)));
                }
            }
        }
    }

    // The image quality, PNG compression and PDF page size cards in every language at the window's narrowest and largest scale:
    // titles and captions keep their whole text, a combo shows its longest choice and the slider keeps its value.
    void saveOptionCardsFitEveryLanguage() {
        if (QGuiApplication::platformName() != "windows") QSKIP("needs desktop fonts");
        const auto language = I18nManager::get_current_language();
        const int scale = get_dialog_scale().percent();
        const auto restore = qScopeGuard([&] { get_dialog_scale().set_percent(scale); I18nManager::load_language(language); });
        for (const int percent : {100, 150, 200}) {
            get_dialog_scale().set_percent(percent);
            for (const auto *code : {"zh", "zh_TW", "en", "ja", "ko"}) {
                QVERIFY(I18nManager::load_language(code));
                SettingsDialog dialog(config_, config_->get_hotkey());
                dialog.fit_to_available({4000, dialog.height()});
                dialog.build_all_pages();
                dialog.resize(0, dialog.height());
                dialog.show();
                dialog.on_nav_changed(1, "capture");
                QApplication::processEvents();
                for (const char *title : {"Image Quality", "PNG Compression", "PDF Page Size"}) {
                    const QString where = QString("%1 at %2%%, %3").arg(code).arg(percent).arg(settings_tr(title));
                    SettingCard *card = nullptr;
                    for (auto *candidate : dialog.findChildren<SettingCard *>())
                        if (candidate->titleLabel->text() == settings_tr(title)) card = candidate;
                    QVERIFY2(card && card->isVisible(), qPrintable(where));
                    QVERIFY2(card->width() >= card->layout()->totalMinimumSize().width(), qPrintable(where + ": the card is squeezed"));
                    QVERIFY2(card->titleLabel->width() >= card->titleLabel->sizeHint().width(), qPrintable(where + ": the title is cut"));
                    QVERIFY2(card->contentLabel->height() >= card->contentLabel->heightForWidth(card->contentLabel->width()), qPrintable(where + ": the caption is cut"));
                    for (auto *combo : card->findChildren<ComboBox *>()) {
                        const QFontMetrics metrics(combo->font());
                        for (int index = 0; index < combo->count(); ++index) {
                            const int needs = metrics.horizontalAdvance(combo->itemText(index)) + widget_scaled(combo, 2 * INPUT_PADDING_H + 18);
                            QVERIFY2(combo->width() >= needs, qPrintable(QString("%1: %2 needs %3, has %4").arg(where, combo->itemText(index)).arg(needs).arg(combo->width())));
                        }
                    }
                    for (auto *slider : card->findChildren<ValueSlider *>()) {
                        QVERIFY2(slider->valueLabel()->width() >= slider->valueLabel()->sizeHint().width(), qPrintable(where + ": the value is cut"));
                        QVERIFY2(slider->slider()->width() >= slider->slider()->minimumWidth(), qPrintable(where + ": the slider is squeezed"));
                    }
                }
            }
        }
    }

    void globalLongScreenshotCanRebindClearAndTransfer() {
        QSettings settings(dir_.filePath("long-shortcut-save.ini"), QSettings::IniFormat);
        settings.clear();
        ToolSettingsManager config(settings);
        SettingsDialog dialog(&config, config.get_hotkey());
        dialog.on_nav_changed(10, "mouse");
        auto *edit = qobject_cast<MouseBindingEditor *>(dialog.behavior_control("quick_capture_long_screenshot"));
        QVERIFY(edit);
        QVERIFY(edit->currentData().isEmpty());
        QVERIFY(!ResourceManager::get_icon_by_name(QString::fromUtf8("长截图.svg")).isNull());
        edit->setBinding("ctrl+alt+dragmiddle");
        QVERIFY(dialog.apply_settings());
        settings.sync();
        QSettings reloaded(settings.fileName(), QSettings::IniFormat);
        ToolSettingsManager fresh(reloaded);
        QCOMPARE(fresh.get_quick_capture_long_screenshot(), QString("ctrl+alt+dragmiddle"));
        const auto imported = settings_transfer::read_settings_file(settings.fileName());
        QCOMPARE(imported.value("app/quick_capture_long_screenshot").toString(), QString("ctrl+alt+dragmiddle"));
        QVERIFY(settings_transfer::IMPORTED_KEYS().contains("app/quick_capture_long_screenshot"));
        auto *pin = qobject_cast<MouseBindingEditor *>(dialog.behavior_control("quick_capture_pin"));
        QVERIFY(pin);
        answer_next(true);
        edit->setBinding(pin->currentData());
        QVERIFY(dialog.apply_settings());
        QCOMPARE(config.get_quick_capture_long_screenshot(), QString("ctrl+alt+dragmiddle"));
        edit->setBinding("");
        QVERIFY(dialog.apply_settings());
        settings.sync();
        QSettings cleared(settings.fileName(), QSettings::IniFormat);
        ToolSettingsManager empty(cleared);
        QVERIFY(empty.get_quick_capture_long_screenshot().isEmpty());
        const auto bindings = get_quick_capture_bindings(&empty);
        QVERIFY(std::none_of(bindings.begin(), bindings.end(), [](const auto &b) { return b.second == "long_screenshot"; }));
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

    void quickEditLeavesTheMiddleClickToPinning() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.on_nav_changed(10, "mouse");
        // Middle click pins text too, so quick edit has no gesture of its own and the defaults do not collide.
        QVERIFY(mouse_binding_conflicts(&dialog).empty());
        auto *quick_edit = qobject_cast<MouseBindingEditor *>(dialog.behavior_control("mouse_clipboard_quick_edit"));
        QVERIFY(quick_edit);
        QVERIFY(quick_edit->currentData().isEmpty());
        quick_edit->setBinding("middle");
        QCOMPARE(mouse_binding_conflicts(&dialog).size(), size_t(1));
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

    // Import takes the settings the pages show from another settings.ini, fills the controls and saves them.
    void importFromAnotherSettingsIni() {
        config_->set_log_level("ERROR");
        config_->set_screenshot_save_path(dir_.path());
        const QString save_path = config_->get_screenshot_save_path();
        SettingsDialog dialog(config_, config_->get_hotkey());
        const auto &keys = settings_transfer::IMPORTED_KEYS();
        QVERIFY(keys.contains("app/hotkey"));
        QVERIFY(keys.contains("app/screenshot_toolbar_layout"));
        QVERIFY(keys.contains("pin/toolbar_layout"));
        // Settings no page shows stay out: the window could never show what was imported.
        for (const char *key : {"app/clipboard_window_width", "clipboard/starter_groups_seeded", "gif/fps", "app/save_dialog_quality"})
            QVERIFY2(!keys.contains(key), key);
        for (const char *key : {"app/screenshot_quality", "app/png_compression", "app/pdf_page_size"}) QVERIFY2(keys.contains(key), key);
        for (const auto &key : LOCAL_SETTING_PATHS()) QVERIFY2(!keys.contains(key), qPrintable(key));
        // A control a page edits by name has its setting imported.
        dialog.build_all_pages();
        QVERIFY(!dialog.behavior_controls.empty());
        for (const auto &entry : dialog.behavior_controls)
            QVERIFY2(keys.contains(ToolSettingsManager::app_setting_path(entry.first)), qPrintable(entry.first));

        const QString other_dir = dir_.filePath("other");
        QVERIFY(QDir().mkpath(other_dir));
        const QString path = QDir(other_dir).filePath("settings.ini");
        {
            QSettings other(path, QSettings::IniFormat);
            other.setValue("app/log_level", "DEBUG");
            other.setValue("app/screenshot_save_path", other_dir);
            other.setValue("app/program_path", QDir(other_dir).filePath("jietuba.exe"));
            other.setValue("app/clipboard_window_width", 999);
        }
        const auto values = settings_transfer::read_settings_file(path);
        QCOMPARE(values.value("app/log_level").toString(), QString("DEBUG"));
        for (const char *key : {"app/screenshot_save_path", "app/program_path", "app/clipboard_window_width"}) QVERIFY2(!values.contains(key), key);
        dialog.load_imported_settings(values);
        QCOMPARE(dialog.attr<QComboBox>("log_level_combo")->currentText(), QString("DEBUG"));
        QVERIFY(!dialog.has_unsaved_changes());
        QCOMPARE(config_->get_log_level(), QString("DEBUG"));
        QCOMPARE(config_->get_screenshot_save_path(), save_path);
        config_->set_log_level("ERROR");
    }

    // Files exported by earlier versions still import; machine-local keys in them are skipped.
    void importReadsEarlierJsonExports() {
        const QString path = dir_.filePath("export.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"format": "jietuba-settings", "version": 1, "app_version": "2.1.7",
                      "settings": {"app/log_level": "WARNING", "app/screenshot_save_path": "D:\\captures"}})");
        file.close();
        const auto values = settings_transfer::read_settings_file(path);
        QCOMPARE(values.value("app/log_level").toString(), QString("WARNING"));
        QVERIFY(!values.contains("app/screenshot_save_path"));

        QFile broken(dir_.filePath("broken.json"));
        QVERIFY(broken.open(QIODevice::WriteOnly));
        broken.write("{\"format\": \"other\"}");
        broken.close();
        QVERIFY_THROWS_EXCEPTION(settings_transfer::SettingsFileError, settings_transfer::read_settings_file(broken.fileName()));
        // Text without any setting the window shows is not a settings file either.
        QFile unrelated(dir_.filePath("notes.ini"));
        QVERIFY(unrelated.open(QIODevice::WriteOnly));
        unrelated.write("[general]\nname=value\n");
        unrelated.close();
        QVERIFY_THROWS_EXCEPTION(settings_transfer::SettingsFileError, settings_transfer::read_settings_file(unrelated.fileName()));
    }

    // The page names the settings file in use instead of offering an export.
    void miscPageShowsTheSettingsFile() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.build_all_pages();
        QVERIFY(dialog.attr<QPushButton>("import_settings_button"));
        const QString shown = QDir::toNativeSeparators(data_location().settings_file());
        bool found = false;
        for (auto *label : dialog.findChildren<QLabel *>()) found = found || label->text().contains(shown);
        QVERIFY2(found, qPrintable(shown));
        UrlCatcher catcher;
        QDesktopServices::setUrlHandler("file", &catcher, "open");
        const auto unset = qScopeGuard([] { QDesktopServices::unsetUrlHandler("file"); });
        dialog.attr<QPushButton>("open_settings_folder_button")->click();
        QTRY_COMPARE(catcher.urls.size(), 1);
        QCOMPARE(QDir::cleanPath(catcher.urls[0].toLocalFile()), QDir::cleanPath(data_location().dir));
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

    // What the clipboard window's menu sets is on the clipboard page too, saved and reset with it.
    void theClipboardPageCarriesTheWindowMenuSettings() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.on_nav_changed(2, "clipboard");
        for (const auto &key : CLIPBOARD_BEHAVIOR_KEYS) QVERIFY2(dialog.behavior_control(key), qPrintable(key));
        auto *lines = qobject_cast<QComboBox *>(dialog.behavior_control("clipboard_display_lines"));
        auto *bar = qobject_cast<QComboBox *>(dialog.behavior_control("clipboard_group_bar_position"));
        auto *hide = qobject_cast<SwitchSettingCard *>(dialog.behavior_control("clipboard_hide_grouped_in_history"));
        auto *auto_paste = qobject_cast<SwitchSettingCard *>(dialog.behavior_control("clipboard_auto_paste"));
        auto *record_private = qobject_cast<SwitchSettingCard *>(dialog.behavior_control("clipboard_record_private"));
        auto *separator = dialog.attr<QComboBox>("clipboard_separator_combo");
        auto *custom = dialog.attr<QLineEdit>("clipboard_separator_edit");
        QVERIFY(lines && bar && hide && auto_paste && record_private && separator && custom);
        QCOMPARE(lines->count(), 5);
        // In the first group beside the monitoring switch, where it is seen without scrolling.
        QCOMPARE(record_private->parentWidget(), dialog.attr_widget("clipboard_enabled_toggle")->parentWidget());
        // The history limit sits with the other data settings.
        auto *limit_spin = dialog.attr<QSpinBox>("clipboard_history_limit_spin");
        QVERIFY(limit_spin && dialog.attr_widget("_clipboard_single_file_toggle")->parentWidget()->isAncestorOf(limit_spin));
        QVERIFY(!record_private->isChecked());
        lines->setCurrentIndex(lines->findData(3));
        bar->setCurrentIndex(bar->findData(QString("left")));
        hide->setChecked(true);
        auto_paste->setChecked(false);
        record_private->setChecked(true);
        QVERIFY(dialog.apply_settings());
        QCOMPARE(config_->get_clipboard_display_lines(), 3);
        QCOMPARE(config_->get_clipboard_group_bar_position(), QString("left"));
        QVERIFY(config_->get_clipboard_hide_grouped_in_history());
        QVERIFY(!config_->get_clipboard_auto_paste());
        QVERIFY(config_->get_clipboard_record_private());
        QVERIFY(dialog.saved_keys().contains("clipboard_display_lines"));

        // A separator outside the presets is typed beside Custom, \n for a line break.
        QVERIFY(custom->isHidden());
        separator->setCurrentIndex(separator->count() - 1);
        QVERIFY(!custom->isHidden());
        custom->setText("\\n--\\n");
        QVERIFY(dialog.apply_settings());
        QCOMPARE(config_->get_clipboard_multi_paste_separator(), QString("\n--\n"));
        QVERIFY(dialog.saved_keys().contains("clipboard_separator"));
        // Changed in the window's menu meanwhile, the page shows it when it opens again.
        config_->set_clipboard_multi_paste_separator(", ");
        config_->set_clipboard_image_size("large");
        dialog.hide();
        dialog.show();
        QCOMPARE(separator->currentData().toString(), QString(", "));
        QVERIFY(custom->isHidden());
        QCOMPARE(qobject_cast<QComboBox *>(dialog.behavior_control("clipboard_image_size"))->currentData().toString(), QString("large"));
        config_->set_clipboard_multi_paste_separator("; ");
        dialog.hide();
        dialog.show();
        QVERIFY(!custom->isHidden());
        QCOMPARE(custom->text(), QString("; "));

        reset_page(dialog);
        QCOMPARE(lines->currentData().toInt(), 1);
        QCOMPARE(bar->currentData().toString(), QString("top"));
        QVERIFY(!hide->isChecked());
        QVERIFY(auto_paste->isChecked());
        QVERIFY(!record_private->isChecked());
        QCOMPARE(separator->currentData().toString(), QString("\n"));
        QVERIFY(dialog.apply_settings());
        QCOMPARE(config_->get_clipboard_display_lines(), 1);
        QCOMPARE(config_->get_clipboard_image_size(), QString("small"));
        QCOMPARE(config_->get_clipboard_multi_paste_separator(), QString("\n"));
        QVERIFY(!config_->get_clipboard_hide_grouped_in_history());
        QVERIFY(!config_->get_clipboard_record_private());
        dialog.hide();
    }

    void thePopupAutoCloseSwitchSavesAndResets() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.on_nav_changed(4, "translation");
        auto *toggle = dialog.attr<SwitchSettingCard>("translation_auto_close_toggle");
        QVERIFY(toggle);
        QVERIFY(!toggle->isChecked());
        toggle->setChecked(true);
        QVERIFY(dialog.apply_settings());
        QVERIFY(config_->get_translation_popup_auto_close());
        reset_page(dialog);
        QVERIFY(!toggle->isChecked());
        QVERIFY(dialog.apply_settings());
        QVERIFY(!config_->get_translation_popup_auto_close());
    }

    void theScreenshotTranslateModeSavesAndResets() {
        config_->settings().remove("translation/screenshot_mode");
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.on_nav_changed(4, "translation");
        auto *combo = dialog.attr<QComboBox>("screenshot_translate_mode_combo");
        QVERIFY(combo);
        QCOMPARE(combo->currentData().toString(), QString("compare"));
        combo->setCurrentIndex(combo->findData("overlay"));
        QVERIFY(dialog.apply_settings());
        QCOMPARE(config_->get_screenshot_translate_mode(), QString("overlay"));
        reset_page(dialog);
        QCOMPARE(combo->currentData().toString(), QString("compare"));
        QVERIFY(dialog.apply_settings());
        QCOMPARE(config_->get_screenshot_translate_mode(), QString("compare"));
        // A value no build writes reads as the default.
        config_->settings().setValue("translation/screenshot_mode", "sideways");
        QCOMPARE(config_->get_screenshot_translate_mode(), QString("compare"));
        config_->set_screenshot_translate_mode("sideways");
        QCOMPARE(config_->settings().value("translation/screenshot_mode").toString(), QString("compare"));
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
        QVERIFY(config_->set_accessor_value("custom_llm_2_model", "m2"));
        QCOMPARE(config_->accessor_value("custom_llm_2_model")->toString(), QString("m2"));
        QVERIFY(!config_->accessor_value("not_a_field"));
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
        QVERIFY(config_->set_accessor_value("volcengine_translate_quota_enabled", false));
        QCOMPARE(config_->accessor_value("volcengine_translate_quota_enabled")->toBool(), false);
        QVERIFY(config_->set_accessor_value("tencent_translate_quota_limit", " 77 "));
        QCOMPARE(config_->accessor_value("tencent_translate_quota_limit")->toString(), QString("77"));
        QVERIFY(config_->set_accessor_value("tencent_translate_secret_id", " id "));
        QCOMPARE(config_->accessor_value("tencent_translate_secret_id")->toString(), QString("id"));
        config_->settings().remove("translation/providers/volcengine");
        config_->settings().remove("translation/providers/tencent");
    }

    // Every service has its two pacing rows on its page; what they hold, saved or still being typed,
    // is what its provider is built with, and blank means the provider's own default.
    void everyServicesPacingRowsReachItsProvider() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.on_nav_changed(4, "translation");
        QVERIFY(dialog.provider_field_widgets);
        const auto registry = dialog.translation_registry;
        QStringList keys;
        const auto reset = qScopeGuard([this, &keys] {
            for (const auto &key : keys) config_->set_accessor_value(key, "");
        });
        const auto providers = registry->available_providers();
        QCOMPARE(providers.size(), size_t(12));
        for (const auto &meta : providers) {
            QVERIFY2(meta.pacing.size() == 2, qPrintable(meta.provider_id));
            const auto &parallel = meta.pacing[0];
            const auto &interval = meta.pacing[1];
            keys << parallel.config_key << interval.config_key;
            auto *edit = qobject_cast<QLineEdit *>(dialog.provider_field_widgets->value(parallel.config_key).data());
            QVERIFY2(edit && dialog.provider_field_widgets->value(interval.config_key), qPrintable(meta.provider_id));
            QCOMPARE(edit->placeholderText(), parallel.placeholder);
            const auto build = [&](const QVariantMap &config) { return registry->create(meta.provider_id, config); };

            auto provider = build(config_->get_translation_provider_config(meta.provider_id));
            QCOMPARE(QString::number(provider->parallel_requests), parallel.placeholder);
            QCOMPARE(QString::number(provider->request_interval_ms), interval.placeholder);

            QVERIFY(config_->set_accessor_value(parallel.config_key, " 3 "));
            QVERIFY(config_->set_accessor_value(interval.config_key, "70"));
            provider = build(config_->get_translation_provider_config(meta.provider_id));
            QVERIFY2(provider->parallel_requests == 3 && provider->request_interval_ms == 70, qPrintable(meta.provider_id));

            edit->setText("5");
            const auto fields = provider_fields::provider_fields(*registry, meta.provider_id);
            const auto pending = provider_fields::pending_config(config_, fields, *dialog.provider_field_widgets);
            provider = build(pending->get_translation_provider_config(meta.provider_id));
            QVERIFY2(provider->parallel_requests == 5, qPrintable(meta.provider_id));
        }
    }

    void youdaoCredentialsReachTheProvider() {
        QVERIFY(config_->set_accessor_value("youdao_translate_app_key", " app "));
        QVERIFY(config_->set_accessor_value("youdao_translate_app_secret", " secret "));
        QCOMPARE(config_->accessor_value("youdao_translate_app_key")->toString(), QString("app"));
        QCOMPARE(config_->get_translation_provider_config("youdao"), (QVariantMap{{"app_key", "app"}, {"app_secret", "secret"}, {"parallel_requests", ""}, {"request_interval_ms", ""}}));
        config_->settings().remove("translation/providers/youdao");
        QCOMPARE(config_->get_translation_provider_config("youdao"), (QVariantMap{{"app_key", ""}, {"app_secret", ""}, {"parallel_requests", ""}, {"request_interval_ms", ""}}));
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

        // Under the hint of each quota provider's card, with a bar; other cards have neither.
        for (const QString provider : {"tencent", "volcengine"}) {
            auto *label = dialog.provider_sections.value(provider)->findChild<QLabel *>("ProviderQuota");
            QVERIFY2(label, qPrintable(provider));
            dialog.refresh_external_state();
            QTRY_COMPARE(label->text(), quota_usage_text(&dialog, provider));
        }
        QVERIFY(!dialog.provider_sections.value("deepl")->findChild<QLabel *>("ProviderQuota"));
        config_->settings().remove("translation/usage");
        config_->settings().remove("translation/providers/volcengine");
    }

    // The window is kept hidden between uses; what was translated meanwhile shows when it is back.
    void usageShowsTranslationsMadeWhileHidden() {
        const QString provider = config_->get_translation_provider();
        const auto restore = qScopeGuard([this, provider] {
            config_->settings().remove("translation/usage");
            config_->set_translation_provider(provider);
        });
        config_->settings().remove("translation/usage");
        config_->set_translation_provider("tencent");
        config_->add_translation_usage_chars("tencent", 500);
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        dialog.on_nav_changed(4, "translation");
        auto *label = dialog.provider_sections.value("tencent")->findChild<QLabel *>("ProviderQuota");
        QVERIFY(label);
        // Shown as soon as the page is built, before the background read returns.
        QVERIFY2(label->text().contains("500 / "), qPrintable(label->text()));
        dialog.hide();
        config_->add_translation_usage_chars("tencent", 1000);
        dialog.show();
        QTRY_VERIFY(label->text().contains("1,500 / "));
    }

    // One page: the options, read aloud and the API services under each other, listed in the outline.
    // The services have a chip per provider, with the chosen one's cards below and the provider's own
    // switches and the general options only it uses.
    void translationPageListsItsGroupsAndHasNoTabs() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.resize(1050, 750);
        dialog.show();
        dialog.on_nav_changed(4, "translation");
        auto *nav = dialog.findChild<NavigationInterface *>("SettingsNavigation");
        auto *scroll = qobject_cast<QScrollArea *>(dialog.content_stack->currentWidget());
        QVERIFY(nav && scroll);
        QVERIFY(!dialog.attr_widget("translation_tabs"));
        QTRY_COMPARE(nav->currentSubRoute(), QString("translation"));
        const QStringList titles{settings_tr("General"), settings_tr("Read Aloud"), settings_tr("API Services")};
        QTRY_COMPARE(nav->subItemTitles("translation"), titles);
        const auto groups = page_groups(scroll);
        QCOMPARE(groups.size(), size_t(3));
        QCOMPARE(groups[0], dialog.attr_widget("translation_options_group"));
        QCOMPARE(groups[1], dialog.attr_widget("speech_group"));
        QCOMPARE(groups[2], dialog.attr_widget("translation_api_group"));

        auto *combo = dialog.attr<QComboBox>("translation_provider_combo");
        auto *picker = dialog.attr_widget("translation_provider_picker");
        QVERIFY(combo->isVisible() && picker->isVisible());
        QVERIFY(groups[2]->isAncestorOf(picker));
        QVERIFY(groups[0]->isAncestorOf(combo));
        QVERIFY(dialog.attr_widget("translation_auto_close_toggle")->isVisible());
        // The last group is reached by scrolling, and the outline follows.
        auto *bar = scroll->verticalScrollBar();
        QVERIFY(bar->maximum() > 0);
        bar->setValue(bar->maximum());
        QTRY_COMPARE(nav->currentSubItem(), 2);
        bar->setValue(0);
        QTRY_COMPARE(nav->currentSubItem(), 0);

        // A chip per provider; the language models in a group of their own.
        const auto providers = dialog.translation_registry->available_providers();
        for (const auto &meta : providers) QVERIFY2(provider_chip(dialog, meta.provider_id), qPrintable(meta.provider_id));
        QVERIFY(provider_chip(dialog, "deepseek")->parentWidget() != provider_chip(dialog, "deepl")->parentWidget());
        QCOMPARE(provider_chip(dialog, "custom_llm")->parentWidget(), provider_chip(dialog, "deepseek")->parentWidget());
        QCOMPARE(provider_chip(dialog, "baidu")->parentWidget(), provider_chip(dialog, "deepl")->parentWidget());
        // The engine's card comes first; a chip shows another.
        const QString engine = combo->currentData().toString();
        QVERIFY(provider_chip(dialog, engine)->isChecked());
        QVERIFY(dialog.provider_sections.value(engine)->isVisible());
        const QString other = engine == "amazon" ? "azure" : "amazon";
        provider_chip(dialog, other)->click();
        QVERIFY(dialog.provider_sections.value(other)->isVisible());
        QVERIFY(!dialog.provider_sections.value(engine)->isVisible());
        QCOMPARE(combo->currentData().toString(), engine);

        // Ignore Line Breaks and Preserve Formatting are only in DeepL's card.
        for (const char *attr : {"split_sentences_toggle", "preserve_formatting_toggle"}) {
            QWidget *toggle = dialog.attr_widget(attr);
            QVERIFY2(toggle && dialog.provider_sections.value("deepl")->isAncestorOf(toggle), attr);
        }
        int rows = 0;
        for (auto *label : groups[2]->findChildren<QLabel *>()) rows += label->text() == settings_tr("Ignore Line Breaks");
        QCOMPARE(rows, 1);
    }

    // Rarely changed settings are folded under Advanced; the settings needed to work are not.
    void advancedFoldsTheRarelyChangedSettings() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        dialog.on_nav_changed(4, "translation");
        show_translation_provider(&dialog, "custom_llm");
        auto *card = dialog.provider_sections.value("custom_llm").data();
        QVERIFY(card->isVisible());
        QWidget *base_url = dialog.provider_field_widgets->value("custom_llm_base_url");
        QWidget *temperature = dialog.provider_field_widgets->value("custom_llm_temperature");
        QWidget *parallel = dialog.provider_field_widgets->value("custom_llm_parallel_requests");
        QVERIFY(base_url->isVisible());
        QVERIFY(!temperature->isVisible());
        QVERIFY(!parallel->isVisible());
        QAbstractButton *advanced = nullptr;
        for (auto *button : card->findChildren<QAbstractButton *>())
            if (button->text() == settings_tr("Advanced")) advanced = button;
        QVERIFY(advanced);
        advanced->click();
        QVERIFY(temperature->isVisible());
        QVERIFY(parallel->isVisible());
        advanced->click();
        QVERIFY(!temperature->isVisible());
        // DeepL's Pro switch is needed to work, so it is not folded.
        show_translation_provider(&dialog, "deepl");
        QVERIFY(dialog.provider_field_widgets->value("deepl_use_pro")->isVisible());
    }

    // Folding near the bottom of a scrolled page changes the heights of several nested layouts; each paint
    // shows either the old layout or the final one, never a level in between.
    void foldingNearTheBottomOfAPagePaintsNoInBetweenLayout() {
        struct PaintSpy : QObject {
            std::function<void()> on_paint;
            bool eventFilter(QObject *, QEvent *event) override {
                if (event->type() == QEvent::Paint && on_paint) on_paint();
                return false;
            }
        };
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.resize(1050, 600);
        dialog.show();
        dialog.on_nav_changed(4, "translation");
        show_translation_provider(&dialog, "google");
        QWidget *card = dialog.provider_sections.value("google");
        QAbstractButton *advanced = nullptr;
        for (auto *button : card->findChildren<QAbstractButton *>())
            if (button->text() == settings_tr("Advanced")) advanced = button;
        QVERIFY(advanced);
        QWidget *footer = card_button(dialog, "google", settings_tr("Test Connection"))->parentWidget();
        QScrollArea *area = nullptr;
        for (QWidget *parent = card; parent && !area; parent = parent->parentWidget()) area = qobject_cast<QScrollArea *>(parent);
        QVERIFY(area);
        // Opening the provider scrolls to its group; that animation must end first.
        if (auto *scroll = area->findChild<QVariantAnimation *>("GroupScrollAnimation")) QTRY_COMPARE(scroll->state(), QAbstractAnimation::Stopped);
        area->verticalScrollBar()->setValue(area->verticalScrollBar()->maximum());
        QTest::qWait(100);

        QList<QPair<int, int>> frames;
        PaintSpy spy;
        spy.on_paint = [&] {
            const QPair<int, int> frame(advanced->mapTo(area->viewport(), QPoint()).y(), footer->mapTo(area->viewport(), QPoint()).y());
            if (frames.isEmpty() || frames.last() != frame) frames << frame;
        };
        area->viewport()->installEventFilter(&spy);
        for (const char *step : {"unfold", "fold"}) {
            frames.clear();
            advanced->click();
            QTest::qWait(150);
            QStringList trail;
            for (const auto &frame : frames) trail << QString("%1/%2").arg(frame.first).arg(frame.second);
            QVERIFY2(!frames.isEmpty() && frames.size() <= 1, qPrintable(QString(step) + ": " + trail.join(' ')));
        }
        spy.on_paint = nullptr;
    }

    // Restoring the minimized window shows the page as it was left: the controls are not loaded again,
    // which would relayout the whole page on the restore paint.
    void restoringFromMinimizedDoesNotReloadThePage() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        dialog.on_nav_changed(1, "capture");
        QWidget *control = dialog.attr_widget("smart_animation_toggle");
        QVERIFY(control);
        auto *card = qobject_cast<SwitchSettingCard *>(control);
        auto *toggle = card ? card->switchButton : qobject_cast<SwitchButton *>(control);
        QVERIFY2(toggle, control->metaObject()->className());
        const bool saved = toggle->isChecked();
        {
            // Not saved, so a reload would put the saved value back.
            const QSignalBlocker blocker(toggle);
            toggle->setChecked(!saved);
        }
        // What the window gets when restored; the offscreen platform does not minimize.
        QWindowStateChangeEvent restored(Qt::WindowMinimized);
        QCoreApplication::sendEvent(&dialog, &restored);
        QCOMPARE(toggle->isChecked(), !saved);
        // Opening the window again does load it.
        const auto restore = qScopeGuard([this, saved] { config_->set_smart_selection_animation(saved); });
        dialog.hide();
        QCOMPARE(config_->get_smart_selection_animation(), !saved);
        {
            const QSignalBlocker blocker(toggle);
            toggle->setChecked(saved);
        }
        dialog.show();
        QCOMPARE(toggle->isChecked(), !saved);
    }

    // Restored from the taskbar, the window is painted at once, before queued layouts run; the show events
    // its rows get on the way leave every row where it was.
    void restoringFromMinimizedKeepsEveryRowInPlace() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.resize(1050, 700);
        dialog.show();
        const auto layout = [&dialog] {
            QHash<QWidget *, QRect> rows;
            for (auto *widget : dialog.findChildren<QWidget *>())
                if (widget->isVisible()) rows.insert(widget, QRect(widget->mapTo(&dialog, QPoint()), widget->size()));
            return rows;
        };
        for (const auto &[index, route] : QList<std::pair<int, QString>>{{1, "capture"}, {2, "clipboard"}, {5, "log"}}) {
            dialog.on_nav_changed(index, route);
            // The page settles first: queries of the system fill some rows in later.
            auto before = layout();
            QTRY_VERIFY_WITH_TIMEOUT([&] {
                QTest::qWait(100);
                const auto now = layout();
                return std::exchange(before, now) == now;
            }(), 5000);
            dialog.setWindowState(Qt::WindowMinimized);
            QTest::qWait(50);
            dialog.setWindowState(Qt::WindowNoState);
            const auto restored = layout();
            QCOMPARE(restored.size(), before.size());
            for (auto it = before.begin(); it != before.end(); ++it) {
                const QRect now = restored.value(it.key());
                const QByteArray where = QString("%1: %2 in %3, %4,%5 %6x%7 -> %8,%9 %10x%11")
                                             .arg(route, it.key()->metaObject()->className(), it.key()->parentWidget()->metaObject()->className())
                                             .arg(it->x()).arg(it->y()).arg(it->width()).arg(it->height())
                                             .arg(now.x()).arg(now.y()).arg(now.width()).arg(now.height())
                                             .toUtf8();
                QVERIFY2(now == it.value(), where.constData());
            }
        }
    }

    // The engine row says whether the engine can translate, from what the form holds now, and its
    // button opens the engine's card; Use This on another configured provider makes it the engine.
    void theEngineStatusFollowsTheFormAndLeadsToTheCard() {
        const QString provider = config_->get_translation_provider();
        const auto restore = qScopeGuard([this, provider] {
            config_->set_translation_provider(provider);
            for (const char *key : {"deepl_api_key", "baidu_translate_appid", "baidu_translate_secret_key", "deepseek_api_key", "deepseek_model"})
                config_->set_accessor_value(key, "");
        });
        for (const char *key : {"deepl_api_key", "baidu_translate_appid", "baidu_translate_secret_key", "deepseek_api_key", "deepseek_model"})
            config_->set_accessor_value(key, "");
        config_->set_translation_provider("deepl");
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        dialog.on_nav_changed(4, "translation");
        auto *status = dialog.attr<SettingCard>("translation_engine_card")->contentLabel;
        QVERIFY2(status->text().contains(settings_tr("Not configured yet")), qPrintable(status->text()));
        auto *chip = provider_chip(dialog, "deepl");
        QVERIFY(chip->property("current").toBool() && !chip->property("configured").toBool());
        const auto edit = [&dialog](const char *key) { return qobject_cast<QLineEdit *>(dialog.provider_field_widgets->value(key).data()); };
        edit("deepl_api_key")->setText("key:fx");
        QVERIFY2(status->text().contains(settings_tr("Configured")), qPrintable(status->text()));
        QVERIFY(chip->property("configured").toBool());
        // A language model also names its model, the default one while the field is empty.
        edit("deepseek_api_key")->setText("sk-test");
        auto *combo = dialog.attr<QComboBox>("translation_provider_combo");
        combo->setCurrentIndex(combo->findData("deepseek"));
        QVERIFY2(status->text().contains(edit("deepseek_model")->placeholderText()), qPrintable(status->text()));
        edit("deepseek_model")->setText("deepseek-chat");
        QVERIFY2(status->text().contains("deepseek-chat"), qPrintable(status->text()));
        QVERIFY(provider_chip(dialog, "deepseek")->property("current").toBool());
        QVERIFY(!chip->property("current").toBool());

        // The button opens the engine's card in the API services group.
        dialog.attr<QPushButton>("translation_configure_button")->click();
        QVERIFY(provider_chip(dialog, "deepseek")->isChecked());
        QVERIFY(dialog.provider_sections.value("deepseek")->isVisible());
        QPushButton *use = card_button(dialog, "deepseek", settings_tr("Use This"));
        QVERIFY(use && !use->isVisible());

        // Use This shows on a configured provider that is not the engine, and makes it the engine.
        provider_chip(dialog, "baidu")->click();
        use = card_button(dialog, "baidu", settings_tr("Use This"));
        QVERIFY(use && !use->isVisible());
        edit("baidu_translate_appid")->setText("app");
        edit("baidu_translate_secret_key")->setText("secret");
        QVERIFY(use->isVisible());
        use->click();
        QCOMPARE(combo->currentData().toString(), QString("baidu"));
        QVERIFY(!use->isVisible());
        QVERIFY(dialog.apply_settings());
        QCOMPARE(config_->get_translation_provider(), QString("baidu"));
    }

    void wizardNavigatesAcrossEveryPage() {
        WelcomeWizard wizard(config_);
        QCOMPARE(wizard.page_count(), 5);
        QCOMPARE(wizard.current_page(), 0);
        wizard.go_to_page(3);
        QCOMPARE(wizard.current_page(), 3);
        wizard.go_to_page(9);
        QCOMPARE(wizard.current_page(), 3);
    }

    void wizardUsesTheStandardCaption() {
        use_test_app_icon();
        WelcomeWizard wizard(config_);
        VERIFY_STANDARD_CAPTION(wizard);
    }

    // One main hotkey per function, in the settings window's order. A conflicting hotkey keeps
    // its stored value; an empty screenshot hotkey keeps the old one, the others clear.
    void wizardHotkeyPageSkipsConflicts() {
        config_->set_hotkey("ctrl+1");
        config_->set_hotkey_2("f1");
        config_->set_translation_hotkey("");
        config_->set_pin_clipboard_hotkey("ctrl+2");
        HotkeyPage page(config_);
        auto edits = page.findChildren<HotkeyEdit *>();
        QCOMPARE(edits.size(), 5);
        QCOMPARE(edits[0]->text(), QString("ctrl+1"));
        QCOMPARE(edits[1]->text(), QString("ctrl+2"));
        // Translation taking the screenshot's key is not written.
        edits[3]->setText("ctrl+1");
        page.save();
        QCOMPARE(config_->get_hotkey(), QString("ctrl+1"));
        QCOMPARE(config_->get_translation_hotkey(), QString(""));
        edits[3]->setText("ctrl+7");
        edits[0]->setText("");
        edits[1]->setText("");
        page.save();
        QCOMPARE(config_->get_hotkey(), QString("ctrl+1"));
        QCOMPARE(config_->get_translation_hotkey(), QString("ctrl+7"));
        QCOMPARE(config_->get_pin_clipboard_hotkey(), QString(""));
        // The settings window's further slots stay as they are.
        QCOMPARE(config_->get_hotkey_2(), QString("f1"));
        config_->set_hotkey_2("");
    }

    // Saving off greys out the folder; the folder is written with the page.
    void wizardStoragePageSavesTheScreenshotFolder() {
        const QString folder = dir_.filePath("shots");
        StoragePage page(config_);
        auto *autosave = page.findChildren<WelcomeToggleSwitch *>().first();
        QLineEdit *path = nullptr;
        for (auto *edit : page.findChildren<QLineEdit *>())
            if (!edit->isReadOnly()) path = edit;
        QVERIFY(path);
        autosave->setChecked(false);
        QVERIFY(!path->isEnabled());
        autosave->setChecked(true);
        QVERIFY(path->isEnabled());
        path->setText(QDir::toNativeSeparators(folder));
        page.save();
        QCOMPARE(QDir::fromNativeSeparators(config_->get_screenshot_save_path()), folder);
        QVERIFY(QFileInfo(folder).isDir());
        QVERIFY(config_->get_screenshot_save_enabled());
        // The settings file it shows is where the settings are.
        bool shown = false;
        for (auto *edit : page.findChildren<QLineEdit *>())
            shown = shown || edit->text() == QDir::toNativeSeparators(data_location().settings_file());
        QVERIFY(shown);
        page.retranslate();
    }

    // Green mode moves the settings file between the program's folder and %APPDATA%, the
    // running program going on with the moved file.
    void greenModeMovesTheSettingsFile() {
        const DataLocation original = data_location();
        const auto restore = qScopeGuard([original] { set_data_location(original); });
        QTemporaryDir root;
        const QString program = root.filePath("program"), roaming = root.filePath("roaming");
        QDir().mkpath(program);
        QDir().mkpath(roaming + "/Jietuba");
        const QString appdata_file = roaming + "/Jietuba/settings.ini", beside_file = program + "/settings.ini";
        QSettings start(appdata_file, QSettings::IniFormat);
        ToolSettingsManager config(start);
        config.set_hotkey("ctrl+9");
        QVERIFY(data_folder::move_settings_file(config, true, program, roaming).isEmpty());
        QVERIFY(QFileInfo(beside_file).isFile());
        QVERIFY(!QFileInfo::exists(appdata_file));
        QVERIFY(data_location().beside_program);
        QCOMPARE(QDir::cleanPath(data_location().settings_file()), QDir::cleanPath(beside_file));
        QCOMPARE(QSettings(beside_file, QSettings::IniFormat).value("app/hotkey").toString(), QString("ctrl+9"));
        // Later writes go to the moved file, which the next start finds first.
        config.set_hotkey("ctrl+8");
        config.qsettings.sync();
        QCOMPARE(QSettings(beside_file, QSettings::IniFormat).value("app/hotkey").toString(), QString("ctrl+8"));
        QVERIFY(resolve_data_location(program, roaming).beside_program);
        // Again is nothing to do.
        QVERIFY(data_folder::move_settings_file(config, true, program, roaming).isEmpty());
        // Off, nothing is left beside the program.
        QVERIFY(data_folder::move_settings_file(config, false, program, roaming).isEmpty());
        QVERIFY(!QFileInfo::exists(beside_file));
        QCOMPARE(QSettings(appdata_file, QSettings::IniFormat).value("app/hotkey").toString(), QString("ctrl+8"));
        QVERIFY(!data_location().beside_program);
        QVERIFY(!resolve_data_location(program, roaming).beside_program);
        // A folder that cannot be made changes nothing.
        QFile blocker(root.filePath("blocked"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        QVERIFY(!data_folder::folder_writable(root.filePath("blocked")));
        QVERIFY(!data_folder::move_settings_file(config, true, root.filePath("blocked"), roaming).isEmpty());
        QVERIFY(QFileInfo(appdata_file).isFile());
        QVERIFY(!data_location().beside_program);
    }

    // All global mouse shortcuts save; conflicting actions keep their stored bindings.
    void wizardMousePageSavesBindingsAndSkipsConflicts() {
        const auto restore = qScopeGuard([this] {
            for (const auto &action : QUICK_CAPTURE_ACTIONS()) config_->set_app_setting(QString("quick_capture_") + action.key, action.binding);
        });
        config_->set_app_setting("quick_capture_scan_code", "ctrl+alt+dragright");
        MouseShortcutPage page(config_);
        const auto editors = page.findChildren<MouseBindingEditor *>();
        QCOMPARE(editors.size(), 8);
        const auto copy = editors[0], pin = editors[1], ocr = editors[3];
        QCOMPARE(pin->currentData(), QString("win+dragleft"));
        QLabel *conflict = nullptr;
        for (auto *label : page.findChildren<QLabel *>())
            if (label->styleSheet().contains("#D9534F")) conflict = label;
        QVERIFY(conflict && conflict->isHidden());
        // Taking the scan action's gesture is a conflict too.
        ocr->setBinding("ctrl+alt+dragright");
        QVERIFY(!conflict->isHidden());
        QVERIFY(conflict->text().contains(settings_tr("Scan QR code / barcode")));
        page.save();
        QCOMPARE(get_quick_capture_binding(config_, "ocr"), QString(""));
        QCOMPARE(get_quick_capture_binding(config_, "scan_code"), QString("ctrl+alt+dragright"));
        ocr->setBinding("ctrl+win+dragleft");
        copy->setBinding("win+dragleft");
        QVERIFY(conflict->text().contains(settings_tr("Pin to Screen")));
        page.save();
        QCOMPARE(get_quick_capture_binding(config_, "ocr"), QString("ctrl+win+dragleft"));
        QCOMPARE(get_quick_capture_binding(config_, "copy"), QString(""));
        QCOMPARE(get_quick_capture_binding(config_, "pin"), QString("win+dragleft"));
        pin->setBinding("");
        QVERIFY(conflict->isHidden());
        page.save();
        QCOMPARE(get_quick_capture_binding(config_, "copy"), QString("win+dragleft"));
        QCOMPARE(get_quick_capture_binding(config_, "pin"), QString(""));
        editors[2]->setBinding("shift+win+dragmiddle");
        editors[5]->setBinding("alt+win+dragx1");
        editors[6]->setBinding("ctrl+win+dragx2");
        editors[7]->setBinding("ctrl+alt+dragmiddle");
        page.save();
        QCOMPARE(get_quick_capture_binding(config_, "copy_pin"), QString("shift+win+dragmiddle"));
        QCOMPARE(get_quick_capture_binding(config_, "scan_code"), QString("alt+win+dragx1"));
        QCOMPARE(get_quick_capture_binding(config_, "edit"), QString("ctrl+win+dragx2"));
        QCOMPARE(get_quick_capture_binding(config_, "long_screenshot"), QString("ctrl+alt+dragmiddle"));
    }

    // The wizard's mouse grid is too narrow for a chevron beside the longest gestures, so its combos leave it out.
    void wizardMouseCombosLeaveOutTheChevron() {
        WelcomeWizard wizard(config_);
        wizard.setAttribute(Qt::WA_DontShowOnScreen);
        wizard.show();
        wizard.go_to_page(2);
        const auto editors = wizard.findChildren<MouseBindingEditor *>();
        QCOMPARE(editors.size(), 8);
        for (auto *editor : editors) {
            QVERIFY(!editor->modifiers->chevronVisible());
            QVERIFY(!editor->gesture->chevronVisible());
        }
    }

    void wizardMouseGridFitsEveryLanguage() {
        if (QGuiApplication::platformName() != "windows") QSKIP("Needs desktop fonts.");
        const QString language = I18nManager::get_current_language();
        const int scale = get_dialog_scale().percent();
        const QString saved_language = config_->get_language();
        const auto restore = qScopeGuard([&] {
            get_dialog_scale().set_percent(scale);
            config_->set_language(saved_language);
            I18nManager::load_language(language);
        });
        for (int percent : {100, 150, 200}) {
            get_dialog_scale().set_percent(percent);
            for (const QString &lang : {QString("zh"), QString("zh_TW"), QString("en"), QString("ja"), QString("ko")}) {
                config_->set_language(lang);
                WelcomeWizard wizard(config_);
                wizard.setAttribute(Qt::WA_DontShowOnScreen);
                wizard.show();
                wizard.go_to_page(2);
                QTest::qWait(80);
                auto *page = wizard.findChild<MouseShortcutPage *>();
                page->set_animation_time(1400);
                const QString out = qEnvironmentVariable("JIETUBA_SETTINGS_SHOTS");
                if (!out.isEmpty() && percent == 100)
                    QVERIFY(wizard.grab().save(QDir(out).filePath(QString("mouse_grid_%1.png").arg(lang))));
                const auto editors = page->findChildren<MouseBindingEditor *>();
                QCOMPARE(editors.size(), 8);
                const QPoint left = editors[0]->mapTo(page, QPoint()), right = editors[1]->mapTo(page, QPoint());
                QCOMPARE(left.y(), right.y());
                QVERIFY(right.x() > left.x() + editors[0]->width());
                QCOMPARE(editors[7]->width(), editors[0]->width());
                for (auto *editor : editors) {
                    QVERIFY(page->rect().contains(QRect(editor->mapTo(page, QPoint()), editor->size())));
                    for (auto *combo : {editor->modifiers, editor->gesture}) {
                        for (int item = 0; item < combo->count(); ++item) {
                            const QString text = combo->itemText(item);
                            QVERIFY2(combo->width() >= combo->fontMetrics().horizontalAdvance(text) + dialog_scaled(8),
                                     qPrintable(QString("%1 %2%%: %3 needs %4, has %5").arg(lang).arg(percent).arg(text)
                                                    .arg(combo->fontMetrics().horizontalAdvance(text) + dialog_scaled(8)).arg(combo->width())));
                        }
                    }
                }
                for (auto *label : page->findChildren<QLabel *>()) {
                    if (!label->isVisible()) continue;
                    QVERIFY(page->rect().contains(QRect(label->mapTo(page, QPoint()), label->size())));
                    QVERIFY2(label->height() >= label->heightForWidth(label->width()),
                             qPrintable(QString("%1 %2%%: %3 needs height %4, has %5 at width %6").arg(lang).arg(percent).arg(label->text())
                                            .arg(label->heightForWidth(label->width())).arg(label->height()).arg(label->width())));
                }
                wizard.hide();
            }
        }
    }

    // The two new demos paint through every phase of their loop.
    void wizardDemosPaintEveryPhase() {
        MouseShortcutPage mouse(config_);
        mouse.resize(900, 560);
        mouse.show();
        QImage previous;
        for (int ms : {0, 600, 1400, 2200, 2800, 4000, 5500}) {
            mouse.set_animation_time(ms);
            const QImage frame = mouse.illus_area->grab().toImage();
            QVERIFY(!frame.isNull());
            if (ms == 1400) QVERIFY(frame != previous);
            previous = frame;
        }
        mouse.hide();
    }

    // The stage demos of the welcome, hotkey, storage and finish pages paint through their loops,
    // and the row or card beside the stage is lit for what plays.
    void wizardStageDemosPaintAndLightTheirRow() {
        config_->set_hotkey("ctrl+alt+shift+f10");
        config_->set_pin_clipboard_hotkey("ctrl+alt+shift+f9");
        const auto restore = qScopeGuard([this] { config_->set_pin_clipboard_hotkey(""); });
        WelcomeWizard wizard(config_);
        wizard.setAttribute(Qt::WA_DontShowOnScreen);
        wizard.show();
        const auto stage_demo = [&wizard](int page) -> welcome_demo::LoopingDemo * {
            wizard.go_to_page(page);
            for (auto *widget : wizard.findChildren<QWidget *>())
                if (auto *demo = dynamic_cast<welcome_demo::LoopingDemo *>(widget); demo && demo->isVisible()) return demo;
            return nullptr;
        };
        for (int page : {0, 1, 3, 4}) {
            auto *demo = stage_demo(page);
            QVERIFY2(demo, qPrintable(QString::number(page)));
            QImage previous;
            int distinct = 0;
            for (int ms = 0; ms < 9000; ms += 300) {
                demo->set_time(ms);
                const QImage frame = demo->grab().toImage();
                QVERIFY(!frame.isNull());
                distinct += frame != previous;
                previous = frame;
            }
            QVERIFY2(distinct >= 15, qPrintable(QString("page %1: %2").arg(page).arg(distinct)));
        }
        // Hotkeys: the screenshot plays first, then the pin; the rows follow.
        auto *hotkeys = stage_demo(1);
        const auto lit_rows = [&wizard] {
            QList<int> lit;
            const auto rows = wizard.findChild<HotkeyPage *>()->findChildren<QWidget *>("ListRow");
            for (int i = 0; i < rows.size(); ++i)
                if (rows[i]->property("listRowCurrent").toBool()) lit << i;
            return lit;
        };
        hotkeys->set_time(100);
        QCOMPARE(lit_rows(), QList<int>{0});
        hotkeys->set_time(2600 + 100);
        QCOMPARE(lit_rows(), QList<int>{1});
        // Storage: screenshots, the clipboard, then the settings file.
        auto *storage = stage_demo(3);
        const auto lit_cards = [&wizard] {
            QList<int> lit;
            int index = 0;
            for (auto *card : wizard.findChild<StoragePage *>()->findChildren<QWidget *>("SettingRow")) {
                if (card->property("welcomeRowCurrent").toBool()) lit << index;
                ++index;
            }
            return lit;
        };
        for (int slot : {0, 1, 2}) {
            storage->set_time(slot * 3000 + 100);
            QCOMPARE(lit_cards(), QList<int>{slot});
        }
        wizard.retranslate_ui();
        wizard.hide();
    }

    // The shared switch follows the window scale only, and its knob stays centred at any size.
    void switchFollowsTheWindowScale() {
        const auto restore = qScopeGuard([] {
            get_dialog_scale().set_percent(100);
            get_ui_scale().set_percent(100);
        });
        get_dialog_scale().set_percent(100);
        SwitchButton normal;
        configure_dialog_control(normal);
        QCOMPARE(normal.size(), QSize(40, 20));
        get_dialog_scale().set_percent(150);
        WelcomeToggleSwitch large;
        configure_dialog_control(large);
        QCOMPARE(large.size(), QSize(60, 30));
        get_ui_scale().set_percent(200);
        configure_dialog_control(large);
        QCOMPARE(large.size(), QSize(60, 30));
        // Down the knob's middle column, the white knob has as much track above it as below.
        large.setChecked(true);
        QTest::qWait(300);
        const QImage image = large.grab().toImage();
        const int column = large.width() - large.height() / 2;
        int first = -1, last = -1;
        for (int y = 0; y < image.height(); ++y) {
            const QColor pixel = image.pixelColor(column, y);
            if (pixel.red() > 245 && pixel.green() > 245 && pixel.blue() > 245) {
                if (first < 0) first = y;
                last = y;
            }
        }
        QVERIFY(first > 0);
        QVERIFY(qAbs(first - (image.height() - 1 - last)) <= 1);
    }

    // The finish page offers pausing hotkeys in games, off unless chosen before.
    void wizardFinishPageStoresTheGamePause() {
        const auto restore = qScopeGuard([this] { config_->set_app_setting("pause_hotkeys_in_games", false); });
        config_->set_app_setting("pause_hotkeys_in_games", false);
        FinishPage page(config_);
        auto switches = page.findChildren<WelcomeToggleSwitch *>();
        QCOMPARE(switches.size(), 4);
        auto *game = switches.last();
        QVERIFY(!game->isChecked());
        game->setChecked(true);
        page.save_preferences();
        QVERIFY(config_->get_app_setting("pause_hotkeys_in_games").toBool());
        FinishPage again(config_);
        QVERIFY(again.findChildren<WelcomeToggleSwitch *>().last()->isChecked());
    }

    // Automatic updates share the autostart card on the finish page, on unless turned off, and only
    // in builds that can replace their own program.
    void wizardFinishPageStoresTheAutoUpdate() {
        const auto restore = qScopeGuard([this] {
            set_helper_source_for_test({});
            config_->set_app_setting("auto_update", true);
        });
        {
            FinishPage page(config_);
            QCOMPARE(page.findChild<WelcomeToggleSwitch *>("autoUpdateSwitch") != nullptr, updater_bundled());
        }
        set_helper_source_for_test(QCoreApplication::applicationFilePath());
        FinishPage page(config_);
        auto *toggle = page.findChild<WelcomeToggleSwitch *>("autoUpdateSwitch");
        QVERIFY(toggle);
        QVERIFY(toggle->isChecked());
        // Still four cards: the autostart card holds both switches.
        QCOMPARE(page.findChildren<WelcomeToggleSwitch *>().size(), 5);
        QCOMPARE(toggle->parentWidget()->objectName(), QString("SettingRow"));
        toggle->setChecked(false);
        page.save_preferences();
        QVERIFY(!config_->get_auto_update());
        FinishPage again(config_);
        QVERIFY(!again.findChild<WelcomeToggleSwitch *>("autoUpdateSwitch")->isChecked());
    }

    // The settings window has the same switch on the misc page.
    void settingsAutoUpdateSwitchNeedsTheUpdaterHelper() {
        const auto restore = qScopeGuard([this] {
            set_helper_source_for_test({});
            config_->set_app_setting("auto_update", true);
        });
        {
            SettingsDialog dialog(config_, config_->get_hotkey());
            dialog.build_all_pages();
            QCOMPARE(dialog.has_attr("auto_update_toggle"), updater_bundled());
        }
        set_helper_source_for_test(QCoreApplication::applicationFilePath());
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.build_all_pages();
        auto *card = dialog.attr<SwitchSettingCard>("auto_update_toggle");
        QVERIFY(card);
        QVERIFY(card->isChecked());
        card->setChecked(false);
        QVERIFY(dialog.apply_settings());
        QVERIFY(!config_->get_auto_update());
    }

    // The finish page with the update switch: every card fits its panel and every text keeps its
    // full size in every language.
    void wizardFinishPageFitsEveryLanguage() {
        if (QGuiApplication::platformName() != "windows") QSKIP("Needs desktop fonts.");
        const QString language = I18nManager::get_current_language();
        const int scale = get_dialog_scale().percent();
        const QString saved_language = config_->get_language();
        set_helper_source_for_test(QCoreApplication::applicationFilePath());
        const auto restore = qScopeGuard([&] {
            set_helper_source_for_test({});
            get_dialog_scale().set_percent(scale);
            config_->set_language(saved_language);
            I18nManager::load_language(language);
        });
        QStringList problems;
        for (int percent : {100, 150, 200}) {
            get_dialog_scale().set_percent(percent);
            for (const QString &lang : {QString("zh"), QString("zh_TW"), QString("en"), QString("ja"), QString("ko")}) {
                config_->set_language(lang);
                WelcomeWizard wizard(config_);
                wizard.setAttribute(Qt::WA_DontShowOnScreen);
                wizard.show();
                wizard.go_to_page(WelcomeWizard::PAGE_COUNT - 1);
                QTest::qWait(80);
                auto *page = wizard.findChild<FinishPage *>();
                QVERIFY(page);
                QVERIFY(page->findChild<WelcomeToggleSwitch *>("autoUpdateSwitch")->isVisible());
                const QString where = QString("%1 %2%").arg(lang).arg(percent);
                const QString out = qEnvironmentVariable("JIETUBA_SETTINGS_SHOTS");
                if (!out.isEmpty() && percent == 100) QVERIFY(wizard.grab().save(QDir(out).filePath(QString("finish_%1.png").arg(lang))));
                // A layout given less than it needs squeezes its cards and cuts their texts.
                auto *panel = page->findChild<QFrame *>("ContentPanel");
                QVERIFY(panel);
                const int needed = panel->layout()->totalHeightForWidth(panel->width());
                if (needed > panel->height()) problems << QString("%1: cards need %2, panel has %3").arg(where).arg(needed).arg(panel->height());
                for (auto *label : page->findChildren<QLabel *>()) {
                    if (!label->isVisible()) continue;
                    if (!page->rect().contains(QRect(label->mapTo(page, QPoint()), label->size()))) problems << QString("%1: %2 leaves the page").arg(where, label->text());
                    const int height = label->wordWrap() ? label->heightForWidth(label->width()) : label->sizeHint().height();
                    if (label->height() < height)
                        problems << QString("%1: %2 needs height %3, has %4 at width %5").arg(where, label->text()).arg(height).arg(label->height()).arg(label->width());
                    if (!label->wordWrap() && label->width() < label->sizeHint().width())
                        problems << QString("%1: %2 needs width %3, has %4").arg(where, label->text()).arg(label->sizeHint().width()).arg(label->width());
                }
                wizard.hide();
            }
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    // The wizard's steps, buttons and pages respond and paint; nothing here closes or finishes
    // it, since the finish page writes the Run key and the desktop shortcut.
    void wizardStepsButtonsAndPagesWork() {
        WelcomeWizard wizard(config_);
        wizard.show();
        QVERIFY(QTest::qWaitForWindowExposed(&wizard));
        auto steps = wizard.findChildren<StepItem *>();
        QCOMPARE(steps.size(), 5);
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

    // Focusing a hotkey slot plays its function alone on the stage and lights its row.
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
        QCOMPARE(edits.size(), 5);
        welcome_demo::LoopingDemo *demo = nullptr;
        for (auto *widget : page->illus_area->findChildren<QWidget *>())
            if (auto *found = dynamic_cast<welcome_demo::LoopingDemo *>(widget)) demo = found;
        QVERIFY(demo);
        const auto lit_row = [page] {
            const auto rows = page->findChildren<QWidget *>("ListRow");
            for (int i = 0; i < rows.size(); ++i)
                if (rows[i]->property("listRowCurrent").toBool()) return i;
            return -1;
        };
        // The screenshot plays first; the clipboard slot, even unset, takes over once focused.
        demo->set_time(1500);
        QCOMPARE(lit_row(), 0);
        const QImage idle = page->illus_area->grab().toImage();
        edits[2]->setText("");
        edits[2]->edit->setFocus();
        QApplication::processEvents();
        QVERIFY(edits[2]->edit->hasFocus());
        demo->set_time(1500);
        QCOMPARE(lit_row(), 2);
        QVERIFY(page->illus_area->grab().toImage() != idle);
        edits[1]->edit->setFocus();
        QApplication::processEvents();
        QCOMPARE(lit_row(), 1);
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
        // The quick actions group sits on the screenshot page and resets with it.
        auto *text_on_top = dialog.attr<SwitchSettingCard>("text_always_on_top_toggle");
        QVERIFY(text_on_top);
        text_on_top->setChecked(!defaults.value("text_always_on_top").toBool());
        answer_next(true);
        reset->click();
        QCOMPARE(format->currentText().toUpper(), defaults.value("screenshot_format").toString().toUpper());
        QCOMPARE(text_on_top->isChecked(), defaults.value("text_always_on_top").toBool());
        // Every other page resets too.
        const std::pair<int, const char *> pages[] = {{3, "appearance"}, {4, "translation"}, {6, "other"}, {10, "mouse"}, {11, "pin"},
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
    // others; keeping the drawings moved here from the screenshot page; the hotkey is shown and edited on the
    // shortcut page.
    void screenAnnotationPage() {
        config_->set_global_hotkeys("screen_annotation", {"ctrl+alt+f9", "", "", ""});
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        dialog.on_nav_changed(1, "capture");
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

    // The read-aloud rows list the registered voices without starting the speech engine, save a
    // choice per language next to choices for languages not installed now, preview the row's
    // current voice and style, and reset to the automatic choice.
    void readAloudVoicesSaveResetAndPreview() {
        const QString root = QString("HKEY_CURRENT_USER\\Software\\jietuba_tests\\settings_speech_%1").arg(QCoreApplication::applicationPid());
        const QStringList categories{root + "\\Classic", root + "\\Modern"};
        const std::tuple<QString, QString, QString, QString> tokens[] = {{categories[0], "ZhA", "Microsoft Alpha - Chinese", "804"},
                                                                         {categories[1], "ZhB", "Microsoft Beta - Chinese", "804"},
                                                                         {categories[1], "En", "Microsoft Gamma - English", "409"}};
        auto backend = std::make_shared<RecordingSpeech>();
        for (const auto &[category, key, name, language] : tokens) {
            QSettings registry(category, QSettings::NativeFormat);
            registry.setValue("Tokens/" + key + "/Default", name);
            registry.setValue("Tokens/" + key + "/Attributes/Language", language);
            backend->voice_list.push_back({name, *locale_from_lcid(language), category + "\\Tokens\\" + key});
        }
        int engines = 0;
        SpeechService::set_voice_categories(categories);
        SpeechService::set_backend_factory([backend, &engines](QString &) {
            ++engines;
            return backend;
        });
        config_->set_speech_voices({"ko=elsewhere"});
        const auto restore = qScopeGuard([this, root] {
            SpeechService::reset_instance();
            SpeechService::set_backend_factory(nullptr);
            SpeechService::set_voice_categories({});
            QSettings(root, QSettings::NativeFormat).remove("");
            QSettings("HKEY_CURRENT_USER\\Software\\jietuba_tests", QSettings::NativeFormat).remove(root.section('\\', -1));
            config_->set_speech_voices({});
            config_->set_speech_rate(0);
            config_->set_speech_volume(100);
        });

        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        dialog.on_nav_changed(4, "translation");
        QTRY_COMPARE(dialog.speech_voice_combos.size(), size_t(2));
        QCOMPARE(engines, 0);
        QStringList keys;
        for (const auto &[key, combo] : dialog.speech_voice_combos) keys << key;
        QCOMPARE(keys, QStringList({"zh", "en"}));
        QPointer<QComboBox> chinese = dialog.speech_voice_combos.front().second;
        QStringList items;
        for (int index = 0; index < chinese->count(); ++index) items << chinese->itemText(index);
        QCOMPARE(items, QStringList({settings_tr("Auto"), "Alpha", "Beta"}));
        QCOMPARE(chinese->currentIndex(), 0);

        chinese->setCurrentIndex(2);
        auto *rate = qobject_cast<QComboBox *>(dialog.behavior_control("speech_rate"));
        auto *volume = qobject_cast<QComboBox *>(dialog.behavior_control("speech_volume"));
        QVERIFY(rate && volume);
        rate->setCurrentIndex(rate->findData(4));
        volume->setCurrentIndex(volume->findData(60));
        QTRY_COMPARE(config_->get_speech_voices(), QStringList({"ko=elsewhere", "zh=" + categories[1] + "\\Tokens\\ZhB"}));
        QCOMPARE(config_->get_speech_rate(), 4);
        QCOMPARE(config_->get_speech_volume(), 60);

        QPushButton *preview = nullptr;
        for (auto *button : chinese->parentWidget()->findChildren<QPushButton *>())
            if (button->toolTip() == settings_tr("Preview")) preview = button;
        QVERIFY(preview);
        // The other combos change before the save, so the preview reads what is shown.
        rate->setCurrentIndex(rate->findData(-3));
        chinese->setCurrentIndex(1);
        preview->click();
        QCOMPARE(engines, 1);
        QCOMPARE(backend->spoken.size(), size_t(1));
        QCOMPARE(backend->spoken.back().voice, QString("Microsoft Alpha - Chinese"));
        QVERIFY(backend->spoken.back().text.contains(QString::fromUtf8("朗读")));
        QCOMPARE(backend->spoken.back().style.rate, -3);
        QCOMPARE(backend->spoken.back().style.volume, 60);
        QCOMPARE(preview->toolTip(), settings_tr("Stop"));
        preview->click();
        QCOMPARE(preview->toolTip(), settings_tr("Preview"));
        QCOMPARE(backend->spoken.size(), size_t(1));

        QPushButton *reset = nullptr;
        for (auto *button : dialog.findChildren<QPushButton *>())
            if (button->text() == settings_tr("Reset This Page")) reset = button;
        QVERIFY(reset);
        answer_next(true);
        reset->click();
        QCOMPARE(chinese->currentIndex(), 0);
        QCOMPARE(rate->currentData().toInt(), 0);
        QTRY_COMPARE(config_->get_speech_voices(), QStringList({"ko=elsewhere"}));
        QCOMPARE(config_->get_speech_rate(), 0);
        QCOMPARE(config_->get_speech_volume(), 100);

        // The window is kept hidden between uses: voices installed or removed meanwhile show when
        // it comes back, and the rebuilt rows save like the first ones.
        dialog.hide();
        QSettings(categories[0], QSettings::NativeFormat).remove("Tokens/ZhA");
        {
            QSettings registry(categories[1], QSettings::NativeFormat);
            registry.setValue("Tokens/Ja/Default", "Microsoft Delta - Japanese");
            registry.setValue("Tokens/Ja/Attributes/Language", "411");
        }
        dialog.show();
        QTRY_VERIFY(!chinese);
        keys.clear();
        for (const auto &[key, combo] : dialog.speech_voice_combos) keys << key;
        keys.sort();
        QCOMPARE(keys, QStringList({"en", "ja", "zh"}));
        QVERIFY(!chinese);
        for (const auto &[key, combo] : dialog.speech_voice_combos)
            if (key == "zh") chinese = combo;
        QVERIFY(chinese);
        QCOMPARE(chinese->count(), 2);
        QCOMPARE(chinese->itemText(1), QString("Beta"));
        chinese->setCurrentIndex(1);
        rate = qobject_cast<QComboBox *>(dialog.behavior_control("speech_rate"));
        QVERIFY(rate);
        rate->setCurrentIndex(rate->findData(2));
        QTRY_COMPARE(config_->get_speech_voices(), QStringList({"ko=elsewhere", "zh=" + categories[1] + "\\Tokens\\ZhB"}));
        QCOMPARE(config_->get_speech_rate(), 2);
    }

    void voiceRefreshPreservesPendingEditsAndUnavailablePreferences() {
        const QString root = QString("HKEY_CURRENT_USER\\Software\\jietuba_tests\\refresh_speech_%1").arg(QCoreApplication::applicationPid());
        const QString category = root + "\\Voices";
        const QString token_a = category + "\\Tokens\\A", token_b = category + "\\Tokens\\B";
        const auto add_voice = [category](const QString &key, const QString &name) {
            QSettings registry(category, QSettings::NativeFormat);
            registry.setValue("Tokens/" + key + "/Default", name);
            registry.setValue("Tokens/" + key + "/Attributes/Language", "804");
        };
        add_voice("A", "Voice A");
        add_voice("B", "Voice B");
        SpeechService::set_voice_categories({category});
        config_->set_speech_voices({"zh=" + token_a});
        const auto restore = qScopeGuard([this, root] {
            SpeechService::reset_instance();
            SpeechService::set_voice_categories({});
            QSettings("HKEY_CURRENT_USER\\Software\\jietuba_tests", QSettings::NativeFormat).remove(root.section('\\', -1));
            config_->set_speech_voices({});
            config_->set_speech_rate(0);
            config_->set_speech_volume(100);
        });
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        dialog.on_nav_changed(4, "translation");
        QTRY_COMPARE(dialog.speech_voice_combos.size(), size_t(1));
        QPointer<QComboBox> chinese = dialog.speech_voice_combos.front().second;
        QCOMPARE(chinese->currentData().toString(), token_a);
        QVERIFY(!dialog.has_unsaved_changes());

        // A refresh uses the selections present when its result arrives, including unsaved edits.
        add_voice("C", "Voice C");
        dialog.refresh_external_state();
        chinese->setCurrentIndex(chinese->findData(token_b));
        auto *rate = qobject_cast<QComboBox *>(dialog.behavior_control("speech_rate"));
        auto *volume = qobject_cast<QComboBox *>(dialog.behavior_control("speech_volume"));
        rate->setCurrentIndex(rate->findData(4));
        volume->setCurrentIndex(volume->findData(60));
        QTRY_VERIFY(!chinese);
        chinese = dialog.speech_voice_combos.front().second;
        QCOMPARE(chinese->currentData().toString(), token_b);
        QCOMPARE(qobject_cast<QComboBox *>(dialog.behavior_control("speech_rate"))->currentData().toInt(), 4);
        QCOMPARE(qobject_cast<QComboBox *>(dialog.behavior_control("speech_volume"))->currentData().toInt(), 60);
        QVERIFY(dialog.apply_settings());
        QCOMPARE(config_->get_speech_voices(), QStringList{"zh=" + token_b});
        QSignalSpy saved(&dialog, &SettingsDialog::settings_applied);

        QSettings(category, QSettings::NativeFormat).remove("Tokens/B");
        dialog.refresh_external_state();
        QTRY_VERIFY(!chinese);
        chinese = dialog.speech_voice_combos.front().second;
        QCOMPARE(chinese->currentIndex(), 0);
        QVERIFY(!dialog.has_unsaved_changes());
        QTest::qWait(350);
        QCOMPARE(config_->get_speech_voices(), QStringList{"zh=" + token_b});
        QCOMPARE(saved.count(), 0);

        // Removing a whole language also keeps its preference, which returns with the voice.
        QSettings(category, QSettings::NativeFormat).remove("Tokens");
        dialog.refresh_external_state();
        QTRY_VERIFY(dialog.speech_voice_combos.empty());
        QVERIFY(!dialog.has_unsaved_changes());
        add_voice("B", "Voice B");
        dialog.refresh_external_state();
        QTRY_COMPARE(dialog.speech_voice_combos.size(), size_t(1));
        QCOMPARE(dialog.speech_voice_combos.front().second->currentData().toString(), token_b);
        QVERIFY(!dialog.has_unsaved_changes());
        QCOMPARE(saved.count(), 0);
        add_voice("A", "Voice A");
        QSettings(category, QSettings::NativeFormat).remove("Tokens/B");
        chinese = dialog.speech_voice_combos.front().second;
        dialog.refresh_external_state();
        QTRY_VERIFY(!chinese);
        chinese = dialog.speech_voice_combos.front().second;
        QCOMPARE(chinese->currentIndex(), 0);
        // Explicitly choosing the already displayed Auto clears the missing preference.
        QVERIFY(QMetaObject::invokeMethod(chinese, "activated", Q_ARG(int, 0)));
        QTRY_VERIFY(config_->get_speech_voices().isEmpty());
        QCOMPARE(saved.count(), 1);
    }

    // The voices are the longest names Windows ships for these languages; the width is the
    // window's minimum.
    void readAloudGroupFitsEveryLanguageInTheNarrowestWindow() {
        // Text widths need real fonts, which the offscreen platform does not have.
        if (QGuiApplication::platformName() != "windows") QSKIP("needs -platform windows");
        const QString root = QString("HKEY_CURRENT_USER\\Software\\jietuba_tests\\speech_width_%1").arg(QCoreApplication::applicationPid());
        const QString category = root + "\\Voices";
        const std::pair<const char *, const char *> voices[] = {
            {"Microsoft Huihui Desktop - Chinese (Simplified)", "804"}, {"Microsoft Kangkang - Chinese (Simplified, PRC)", "804"},
            {"Microsoft Hanhan Desktop - Chinese(Taiwan)", "404"},       {"Microsoft David Desktop - English (United States)", "409"},
            {"Microsoft Hazel Desktop - English (Great Britain)", "809"}, {"Microsoft Haruka Desktop - Japanese", "411"},
            {"Microsoft Heami Desktop - Korean", "412"},                 {"Microsoft An - Vietnamese (Vietnam)", "42A"},
            {"Microsoft Hortense Desktop - French", "40C"}};
        {
            QSettings registry(category, QSettings::NativeFormat);
            int index = 0;
            for (const auto &[name, language] : voices) {
                const QString key = QString("Tokens/Voice%1/").arg(index++);
                registry.setValue(key + "Default", name);
                registry.setValue(key + "Attributes/Language", language);
            }
        }
        SpeechService::set_voice_categories({category});
        auto &scale = get_dialog_scale();
        const int percent_before = scale.percent();
        const QString language = I18nManager::get_current_language();
        const auto restore = qScopeGuard([&scale, percent_before, language, root] {
            SpeechService::reset_instance();
            SpeechService::set_voice_categories({});
            QSettings("HKEY_CURRENT_USER\\Software\\jietuba_tests", QSettings::NativeFormat).remove(root.section('\\', -1));
            scale.set_percent(percent_before);
            I18nManager::load_language(language);
        });

        for (const int percent : {100, 150, 200}) {
            scale.set_percent(percent);
            for (const char *code : {"zh", "zh_TW", "en", "ja", "ko"}) {
                QVERIFY(I18nManager::load_language(code));
                const QString where = QString("%1 at %2%").arg(code).arg(percent);
                SettingsDialog dialog(config_, config_->get_hotkey());
                // Wider than any screen, so the minimum is not capped at this one's width.
                dialog.fit_to_available({4000, dialog.height()});
                const int narrowest = dialog.minimumWidth();
                dialog.resize(narrowest, dialog.height());
                dialog.show();
                dialog.on_nav_changed(4, "translation");
                QTRY_COMPARE(dialog.speech_voice_combos.size(), size_t(6));
                QApplication::processEvents();
                auto *group = dialog.attr<SettingCardGroup>("speech_group");
                QVERIFY(group);
                auto *scroll = qobject_cast<QScrollArea *>(dialog.content_stack->currentWidget());
                QVERIFY(scroll);
                QVERIFY2(dialog.width() == narrowest, qPrintable(QString("%1: the window is %2 wide, not %3").arg(where).arg(dialog.width()).arg(narrowest)));
                QVERIFY2(scroll->horizontalScrollBar()->maximum() == 0, qPrintable(where + ": the page scrolls sideways"));

                // Empty when the part gets its whole width.
                const auto squeezed = [&where](QWidget *part, const QString &name) {
                    if (part->width() >= part->sizeHint().width()) return QString();
                    return QString("%1: %2 is %3 wide, needs %4").arg(where, name).arg(part->width()).arg(part->sizeHint().width());
                };
                QString problem = squeezed(group->titleLabel, group->titleLabel->text());
                QVERIFY2(problem.isEmpty(), qPrintable(problem));
                QWidget *viewport = scroll->viewport();
                QStringList voice_items;
                for (auto *card : group->findChildren<SettingCard *>()) {
                    problem = squeezed(card->titleLabel, card->titleLabel->text());
                    QVERIFY2(problem.isEmpty(), qPrintable(problem));
                    if (card->contentLabel->isVisible())
                        QVERIFY2(card->contentLabel->height() >= card->contentLabel->heightForWidth(card->contentLabel->width()),
                                 qPrintable(QString("%1: the description of %2 is cut off").arg(where, card->titleLabel->text())));
                    const QRect in_page(card->mapTo(viewport, QPoint()), card->size());
                    QVERIFY2(in_page.left() >= 0 && in_page.right() < viewport->width(), qPrintable(QString("%1: %2 sticks out of the page").arg(where, card->titleLabel->text())));
                    for (auto *control : card->controlContainer->findChildren<QWidget *>(Qt::FindDirectChildrenOnly)) {
                        if (!control->isVisible()) continue;
                        QString name = control->property("text").toString();
                        if (auto *combo = qobject_cast<QComboBox *>(control)) {
                            QStringList items;
                            for (int i = 0; i < combo->count(); ++i) items << combo->itemText(i);
                            name = items.join(" / ");
                            voice_items << name;
                        }
                        // The preview button is an icon of fixed size.
                        if (name.isEmpty()) name = control->accessibleName();
                        else problem = squeezed(control, QString("%1's %2").arg(card->titleLabel->text(), name));
                        QVERIFY2(problem.isEmpty(), qPrintable(problem));
                        QVERIFY2(card->rect().contains(QRect(control->mapTo(card, QPoint()), control->size())),
                                 qPrintable(QString("%1: %2 sticks out of %3").arg(where, name, card->titleLabel->text())));
                    }
                }
                // Each voice row, the speed and volume combos and the Open Settings button were checked.
                QPushButton *open = nullptr;
                for (auto *button : group->findChildren<QPushButton *>())
                    if (button->text() == settings_tr("Open Settings")) open = button;
                QVERIFY2(open && open->isVisible(), qPrintable(where));
                QCOMPARE(voice_items.size(), qsizetype(6 + 2));
                QVERIFY2(voice_items.contains(settings_tr("Auto") + " / Hanhan Desktop / Huihui Desktop / Kangkang"), qPrintable(voice_items.join(", ")));
                QVERIFY2(voice_items.contains(settings_tr("Auto") + " / Hortense Desktop"), qPrintable(voice_items.join(", ")));
            }
        }
    }

    void aboutFeedbackAndStarOpenTheirGitHubPages() {
        const QString language = I18nManager::get_current_language();
        const auto restore_language = qScopeGuard([&] { I18nManager::load_language(language); });
        QVERIFY(I18nManager::load_language("zh"));
        UrlCatcher catcher;
        QDesktopServices::setUrlHandler("https", &catcher, "open");
        const auto unset = qScopeGuard([] { QDesktopServices::unsetUrlHandler("https"); });
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.on_nav_changed(8, "about");
        auto *page = dialog.content_stack->currentWidget();
        auto *feedback = page->findChild<QPushButton *>("aboutFeedbackButton");
        auto *star = page->findChild<QPushButton *>("aboutRepositoryLink");
        QVERIFY(feedback && star);
        QVERIFY(!feedback->icon().isNull());
        QCOMPARE(star->text(), QStringLiteral("☆ 截图吧用着顺手？给个 Star"));
        feedback->click();
        star->click();
        const QList<QUrl> expected{QUrl("https://github.com/1003129155/jietuba/issues"), QUrl("https://github.com/1003129155/jietuba")};
        QCOMPARE(catcher.urls, expected);
    }

    // Every page in every language at the window's minimum width; the shortcut pages are tables that
    // scroll sideways below the default width.
    void everyPageFitsEveryLanguageAtTheMinimumWidth() {
        // Text widths need real fonts, which the offscreen platform does not have.
        if (QGuiApplication::platformName() != "windows") QSKIP("needs -platform windows");
        auto &scale = get_dialog_scale();
        const int percent_before = scale.percent();
        const QString language = I18nManager::get_current_language();
        // With the updater helper, so the update switch is measured too.
        set_helper_source_for_test(QCoreApplication::applicationFilePath());
        const auto restore = qScopeGuard([&scale, percent_before, language] {
            set_helper_source_for_test({});
            scale.set_percent(percent_before);
            I18nManager::load_language(language);
        });
        // The developer page has no navigation item.
        const std::pair<int, const char *> pages[] = {{1, "capture"}, {11, "pin"}, {12, "screen_annotation"}, {2, "clipboard"}, {4, "translation"},
                                                      {3, "appearance"}, {5, "log"}, {6, "other"}, {8, "about"}, {7, nullptr}};
        QStringList problems;
        for (const int percent : {100, 150, 200}) {
            scale.set_percent(percent);
            for (const char *code : {"zh", "zh_TW", "en", "ja", "ko"}) {
                QVERIFY(I18nManager::load_language(code));
                SettingsDialog dialog(config_, config_->get_hotkey());
                // Wider than any screen, so the minimum is not capped at this one's width.
                dialog.fit_to_available({4000, dialog.height()});
                dialog.build_all_pages();
                dialog.resize(0, dialog.height());
                dialog.show();
                QCOMPARE(dialog.width(), dialog.minimumWidth());
                for (const auto &[index, route] : pages) {
                    if (route) dialog.on_nav_changed(index, route);
                    else dialog.content_stack->setCurrentIndex(index);
                    QApplication::processEvents();
                    const QString where = QString("%1 at %2%, page %3").arg(code).arg(percent).arg(index);
                    // A layout given less than its minimum cuts a title short or pushes controls out of their card.
                    const auto check = [&problems, &where](QWidget *widget, const QString &name) {
                        const int needs = widget->layout()->totalMinimumSize().width();
                        if (needs > widget->width()) problems << QString("%1: %2 needs %3, has %4").arg(where, name).arg(needs).arg(widget->width());
                    };
                    check(&dialog, "the window");
                    auto *scroll = qobject_cast<QScrollArea *>(dialog.content_stack->currentWidget());
                    QVERIFY(scroll);
                    if (scroll->horizontalScrollBar()->maximum() > 0) problems << where + ": the page scrolls sideways";
                    if (index == 8) {
                        auto *feedback = scroll->widget()->findChild<QPushButton *>("aboutFeedbackButton");
                        auto *license = scroll->widget()->findChild<QPushButton *>("aboutLicenseButton");
                        auto *star = scroll->widget()->findChild<QPushButton *>("aboutRepositoryLink");
                        QVERIFY(feedback && license && star);
                        if (feedback->width() < feedback->sizeHint().width() || feedback->y() <= license->geometry().bottom() ||
                            feedback->x() != license->x() || feedback->width() != license->width() ||
                            feedback->mapTo(scroll->widget(), QPoint(feedback->width(), 0)).x() >= scroll->viewport()->width()) {
                            problems << where + ": the feedback button does not fit under the license button";
                        }
                        if (star->width() < star->sizeHint().width() || star->geometry().right() >= scroll->viewport()->width()) {
                            problems << where + ": the Star link does not fit";
                        }
                    }
                    for (auto *widget : scroll->widget()->findChildren<QWidget *>()) {
                        if (!widget->layout() || !widget->isVisible()) continue;
                        auto *card = qobject_cast<SettingCard *>(widget);
                        check(widget, card ? card->titleLabel->text() : QString(widget->metaObject()->className()));
                    }
                }
            }
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    // The Explorer entry's card in every language, at every window scale and the minimum width: the
    // title and description keep their lines and the dropdown shows each choice whole.
    void explorerMenuCardFitsEveryLanguageAtEveryScale() {
        // Text widths need real fonts, which the offscreen platform does not have.
        if (QGuiApplication::platformName() != "windows") QSKIP("needs -platform windows");
        auto &scale = get_dialog_scale();
        const int percent_before = scale.percent();
        const QString language = I18nManager::get_current_language();
        const auto restore = qScopeGuard([&scale, percent_before, language] {
            set_packaged_for_test(std::nullopt);
            scale.set_percent(percent_before);
            I18nManager::load_language(language);
        });
        QStringList problems;
        for (const bool packaged : {false, true})
            for (const int percent : {100, 150, 200}) {
                scale.set_percent(percent);
                set_packaged_for_test(packaged);
                for (const char *code : {"zh", "zh_TW", "en", "ja", "ko"}) {
                    QVERIFY(I18nManager::load_language(code));
                    SettingsDialog dialog(config_, config_->get_hotkey());
                    dialog.fit_to_available({4000, dialog.height()});
                    dialog.build_all_pages();
                    dialog.resize(0, dialog.height());
                    dialog.show();
                    dialog.on_nav_changed(11, "pin");
                    QApplication::processEvents();
                    const QString where = QString("%1 at %2%%3").arg(code).arg(percent).arg(packaged ? ", packaged" : "");
                    auto *combo = qobject_cast<QComboBox *>(dialog.behavior_control("explorer_context_menu"));
                    QVERIFY(combo);
                    SettingCard *card = nullptr;
                    for (QWidget *w = combo; w && !card; w = w->parentWidget()) card = qobject_cast<SettingCard *>(w);
                    QVERIFY(card);
                    if (combo->width() < combo->sizeHint().width()) problems << where + ": the dropdown is narrower than its content";
                    const QFontMetrics metrics(combo->font());
                    for (int i = 0; i < combo->count(); ++i)
                        if (metrics.horizontalAdvance(combo->itemText(i)) > combo->width()) problems << where + ": " + combo->itemText(i) + " does not fit the dropdown";
                    if (card->titleLabel->width() < card->titleLabel->sizeHint().width()) problems << where + ": the title is cut";
                    if (card->layout()->totalMinimumSize().width() > card->width()) problems << where + ": the card needs more width than it has";
                    // A description that wraps keeps every line.
                    auto *label = card->contentLabel;
                    if (label->wordWrap() && label->height() < label->heightForWidth(label->width())) problems << where + ": the description loses lines";
                    if (dialog.width() != dialog.minimumWidth()) problems << where + ": the window grew";
                }
            }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    // The translation page's tabs and every provider card in every language at the window's minimum
    // width: the chips, the name with its tags and the buttons keep their whole text, wrapped text
    // keeps every line, and nothing scrolls sideways.
    void translationApiTabFitsEveryLanguageAtTheMinimumWidth() {
        // Text widths need real fonts, which the offscreen platform does not have.
        if (QGuiApplication::platformName() != "windows") QSKIP("needs -platform windows");
        auto &scale = get_dialog_scale();
        const int percent_before = scale.percent();
        const QString language = I18nManager::get_current_language();
        const QString engine = config_->get_translation_provider();
        const auto restore = qScopeGuard([this, &scale, percent_before, language, engine] {
            scale.set_percent(percent_before);
            I18nManager::load_language(language);
            config_->set_translation_provider(engine);
            for (const char *key : {"baidu_translate_appid", "baidu_translate_secret_key"}) config_->set_accessor_value(key, "");
        });
        // With Google as the engine, Baidu filled in shows Use This.
        config_->set_translation_provider("google");
        QStringList problems;
        const auto check = [&problems](QWidget *widget, const QString &where, const QString &name) {
            if (!widget->isVisible()) return;
            if (auto *label = qobject_cast<QLabel *>(widget); label && label->wordWrap()) {
                if (label->height() < label->heightForWidth(label->width())) problems << QString("%1: \"%2\" is cut off").arg(where, name);
            } else if (widget->width() < widget->sizeHint().width()) {
                problems << QString("%1: \"%2\" is %3 wide, needs %4").arg(where, name).arg(widget->width()).arg(widget->sizeHint().width());
            }
        };
        for (const int percent : {100, 150, 200}) {
            scale.set_percent(percent);
            for (const char *code : {"zh", "zh_TW", "en", "ja", "ko"}) {
                QVERIFY(I18nManager::load_language(code));
                SettingsDialog dialog(config_, config_->get_hotkey());
                // Wider than any screen, so the minimum is not capped at this one's width.
                dialog.fit_to_available({4000, dialog.height()});
                dialog.resize(0, dialog.height());
                dialog.show();
                dialog.on_nav_changed(4, "translation");
                qobject_cast<QLineEdit *>(dialog.provider_field_widgets->value("baidu_translate_appid").data())->setText("app");
                qobject_cast<QLineEdit *>(dialog.provider_field_widgets->value("baidu_translate_secret_key").data())->setText("secret");
                const QString at = QString("%1 at %2%").arg(code).arg(percent);
                for (const auto &meta : dialog.translation_registry->available_providers()) {
                    show_translation_provider(&dialog, meta.provider_id);
                    QApplication::processEvents();
                    const QString where = at + ", " + meta.provider_id;
                    auto *scroll = qobject_cast<QScrollArea *>(dialog.content_stack->currentWidget());
                    QVERIFY(scroll);
                    if (scroll->horizontalScrollBar()->maximum() > 0) problems << where + ": the page scrolls sideways";
                    check(provider_chip(dialog, meta.provider_id), where, provider_chip(dialog, meta.provider_id)->text());
                    QWidget *card = dialog.provider_sections.value(meta.provider_id);
                    const QRect in_page(card->mapTo(scroll->viewport(), QPoint()), card->size());
                    if (in_page.left() < 0 || in_page.right() >= scroll->viewport()->width()) problems << where + ": the card sticks out of the page";
                    for (auto *label : card->findChildren<QLabel *>()) check(label, where, label->text());
                    for (auto *button : card->findChildren<QPushButton *>()) check(button, where, button->text());
                    for (auto *widget : card->findChildren<QWidget *>())
                        if (widget->layout() && widget->isVisible() && widget->layout()->totalMinimumSize().width() > widget->width())
                            problems << QString("%1: a %2 needs %3, has %4").arg(where, widget->metaObject()->className()).arg(widget->layout()->totalMinimumSize().width()).arg(widget->width());
                }
                QVERIFY2(card_button(dialog, "baidu", settings_tr("Use This"))->isVisibleTo(dialog.provider_sections.value("baidu")), qPrintable(at));
            }
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    // The clipboard theme menu marks the chosen swatch with an icon, not a ✓ character.
    void clipboardThemeMenuMarksTheChosenSwatch() {
        const auto restore = qScopeGuard([this, theme = config_->get_clipboard_theme()] { config_->set_clipboard_theme(theme); });
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.build_all_pages();
        dialog.show();
        auto *theme_btn = dialog.attr<QPushButton>("_clip_theme_btn");
        QVERIFY(theme_btn);
        const auto open_swatches = [theme_btn] {
            theme_btn->click();
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            return menu ? menu->findChildren<QPushButton *>() : QList<QPushButton *>();
        };
        const auto marked = [](const QList<QPushButton *> &swatches) {
            QList<int> rows;
            for (int row = 0; row < swatches.size(); ++row) {
                if (!swatches[row]->text().isEmpty()) return QList<int>{-1};
                if (!swatches[row]->icon().isNull()) rows << row;
            }
            return rows;
        };
        auto swatches = open_swatches();
        QVERIFY(swatches.size() > 1);
        QCOMPARE(marked(swatches).size(), 1);
        const int last = static_cast<int>(swatches.size()) - 1;
        QVERIFY(marked(swatches).first() != last);
        swatches.last()->click();
        QTRY_VERIFY(!QApplication::activePopupWidget());
        QCOMPARE(marked(open_swatches()), QList<int>{last});
        QApplication::activePopupWidget()->close();
    }

    // The logo's context menu opens the developer page; the pin page links to its shortcut tabs.
    void developerPageAndPinShortcutLinks() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        auto *title_bar = qobject_cast<FluentTitleBar *>(dialog.titleBar());
        QVERIFY(title_bar);
        QWidget *logo = title_bar->iconLabel;
        QCOMPARE(logo->contextMenuPolicy(), Qt::CustomContextMenu);
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
        QVERIFY(dialog.attr_widget("mouse_pin_group"));
        dialog.show_pin_shortcuts(false);
        QCOMPARE(dialog.content_stack->currentIndex(), 0);
        QVERIFY(dialog.attr_widget("inapp_pin_group"));
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

    void theClipboardSearchKeyIsAClipboardShortcutThatTakesNoTypingKeys() {
        const auto restore = qScopeGuard([this] { config_->set_inapp_shortcut("inapp_clipboard_search", "ctrl+f"); });
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.build_all_pages();
        auto *edit = dialog.inapp_edit("inapp_clipboard_search");
        QVERIFY(edit);
        QCOMPARE(edit->text(), QString("ctrl+f"));
        // In the clipboard group, so it is checked against the other clipboard keys and direct pick.
        QCOMPARE(dialog.inapp_groups.value("inapp_clipboard_search"), QString("clipboard"));
        // It also works while typing in the search box, so a plain letter or caret key is not taken.
        for (const auto &[key, text] : {std::pair{Qt::Key_A, "a"}, std::pair{Qt::Key_Left, ""}}) {
            QKeyEvent typing(QEvent::KeyPress, key, Qt::NoModifier, QString(text));
            QApplication::sendEvent(edit, &typing);
            QCOMPARE(edit->text(), QString("ctrl+f"));
        }
        QKeyEvent with_ctrl(QEvent::KeyPress, Qt::Key_K, Qt::ControlModifier);
        QApplication::sendEvent(edit, &with_ctrl);
        QCOMPARE(edit->text(), QString("ctrl+k"));
    }

    // Save and quick save sit in the screenshot group, and their defaults take no other capture key.
    void saveKeysSitInTheScreenshotGroup() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        auto *group = group_titled(dialog, i18n_tr("Screenshot Window", "ShortcutTabs"));
        QVERIFY(group);
        for (const auto &[key, value] : {std::pair{"inapp_save", "ctrl+s"}, std::pair{"inapp_quick_save", "ctrl+shift+s"}}) {
            auto *edit = dialog.inapp_edit(key);
            QVERIFY(edit);
            QVERIFY(group->isAncestorOf(edit));
            QCOMPARE(edit->text(), QString(value));
            QCOMPARE(dialog.inapp_groups.value(key), QString("screenshot"));
            for (const auto &[other_key, other] : dialog.inapp_edits)
                if (other_key != key && dialog.inapp_groups.value(other_key) == "screenshot") QVERIFY2(other->text() != value, qPrintable(other_key));
        }
    }

    void undoAndRedoBelongToAnnotationTools() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        auto *screenshot_page = group_titled(dialog, i18n_tr("Screenshot Window", "ShortcutTabs"));
        auto *tools_page = group_titled(dialog, i18n_tr("Annotation Tools", "ShortcutTabs"));
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
        config_->set_accessor_value("amazon_translate_region", "eu-west-1");
        config_->set_accessor_value("azure_translate_endpoint", "https://example.test");
        config_->set_accessor_value("baidu_translate_appid", "app");
        config_->set_accessor_value("deepseek_model", "deepseek-chat");
        config_->set_accessor_value("custom_llm_2_model", "local");
        QCOMPARE(config_->get_translation_provider_config("amazon").value("region").toString(), QString("eu-west-1"));
        QCOMPARE(config_->get_translation_provider_config("azure").value("endpoint").toString(), QString("https://example.test"));
        QCOMPARE(config_->get_translation_provider_config("baidu").value("appid").toString(), QString("app"));
        QCOMPARE(config_->get_translation_provider_config("deepseek").value("model").toString(), QString("deepseek-chat"));
        QVERIFY(config_->get_translation_provider_config("google").contains("api_key"));
        const auto custom = config_->get_translation_provider_config("custom_llm_2");
        QCOMPARE(custom.value("model").toString(), QString("local"));
        QVERIFY(custom.contains("json_mode"));
        QVERIFY(config_->get_translation_provider_config("unknown").isEmpty());
        QVERIFY(!config_->set_accessor_value("custom_llm_2_unknown_key", "x"));
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
        QTRY_COMPARE(config_->accessor_value("custom_llm_base_url")->toString(), QString("http://127.0.0.1:9/v1"));
    }
    void searchRanksTitlesAboveOtherFields() {
        using Kind = SettingsSearchEntry::Kind;
        const auto make = [](Kind kind, const QString &title, const QString &path = {}, const QString &description = {}, const QStringList &options = {}) {
            SettingsSearchEntry entry;
            entry.kind = kind;
            entry.title = title;
            entry.path = path;
            entry.description = description;
            entry.options = options;
            return entry;
        };
        const std::vector<SettingsSearchEntry> entries{
            make(Kind::Item, "Opacity", "Pin / Behavior", "Changes with the zoom wheel"),
            make(Kind::Item, "Fine Zoom Step", "Pin / Zoom and Size"),
            make(Kind::Item, "Zoom Step", "Pin / Zoom and Size"),
            make(Kind::Item, "Save Format", "Screenshot / Save", {}, {"PNG", "JPG"}),
            make(Kind::Page, "Zoom"),
            make(Kind::Group, "Zoom", "Pin"),
            make(Kind::Item, "Magnifier", "Screenshot / Zoom Tools"),
            make(Kind::Item, QString::fromUtf8("默认透明度"), QString::fromUtf8("钉图 / 行为")),
            make(Kind::Item, QString::fromUtf8("透明度步长"), QString::fromUtf8("钉图 / 缩放")),
            make(Kind::Item, QString::fromUtf8("窗口透明度"), QString::fromUtf8("剪贴板 / 外观")),
        };
        const auto titles = [&entries](const QString &query) {
            QStringList found;
            for (const auto &entry : rank_settings_search(entries, query))
                found << entry.title + (entry.kind == Kind::Page ? " (page)" : entry.kind == Kind::Group ? " (group)" : "");
            return found;
        };
        // The whole title, then the title's start, a word in it, the path, the description; equal
        // whole titles put the page before the group.
        QCOMPARE(titles("zoom"), (QStringList{"Zoom (page)", "Zoom (group)", "Zoom Step", "Fine Zoom Step", "Magnifier", "Opacity"}));
        // Every word has to match, each in its best field.
        QCOMPARE(titles("zoom step"), (QStringList{"Zoom Step", "Fine Zoom Step"}));
        QVERIFY(titles("zoom png").isEmpty());
        QCOMPARE(titles("png"), QStringList{"Save Format"});
        QCOMPARE(titles(QString::fromUtf8("钉图 透明")), (QStringList{QString::fromUtf8("透明度步长"), QString::fromUtf8("默认透明度")}));
        // Width, case and spacing do not matter; nothing typed finds nothing.
        QCOMPARE(titles(QString::fromUtf8("  ＺＯＯＭ \t STEP ")), titles("zoom step"));
        QVERIFY(titles("").isEmpty());
        QVERIFY(titles("   ").isEmpty());
    }

    // Pages are built on first visit; searching builds the others and finds their rows.
    void searchFindsRowsOfPagesNotOpenedYet() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        dialog.resize(dialog.width(), 600);
        dialog.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&dialog));
        // Opening the window builds only the first page; the search builds the rest when used.
        QTest::qWait(100);
        QVERIFY(!dialog.has_attr("pin_auto_toolbar_toggle"));
        auto *box = dialog.search_box();
        QVERIFY(box);
        const QString title = settings_tr("Show Lock Hints");
        box->edit()->setText(title);
        QTRY_VERIFY(box->results_visible() && !box->results().empty() && box->results().front().title == title);
        const auto result = box->results().front();
        QCOMPARE(result.route_key, QString("pin"));
        QCOMPARE(result.path, settings_tr("Pin") + " / " + settings_tr("Lock"));
        QPointer<QWidget> target = result.target;
        QVERIFY(target);

        QTest::keyClick(box->edit(), Qt::Key_Return);
        QVERIFY(!box->results_visible());
        QVERIFY(dialog.isVisible());
        QCOMPARE(box->edit()->text(), title);
        QCOMPARE(dialog.content_stack->currentIndex(), 11);
        QVERIFY(dialog.findChild<NavigationInterface *>("SettingsNavigation")->widget("pin")->isChecked());
        // Near the end of a page taller than the window: scrolled into view, then outlined for a moment.
        auto *scroll = qobject_cast<QScrollArea *>(dialog.content_stack->currentWidget());
        QVERIFY(scroll);
        QTRY_VERIFY(flash_over(target));
        QVERIFY(scroll->verticalScrollBar()->value() > 0);
        const QRect in_view(target->mapTo(scroll->viewport(), QPoint()), target->size());
        QVERIFY2(scroll->viewport()->rect().contains(in_view), qPrintable(QString("row at %1,%2 in a %3 px view").arg(in_view.top()).arg(in_view.bottom()).arg(scroll->viewport()->height())));
        QTRY_VERIFY_WITH_TIMEOUT(!flash_over(target), 4000);
    }

    // A row of a window's shortcuts is found under that window's group; opening it outlines the row and marks the group.
    void searchJumpsToTheGroupHoldingTheRow() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.resize(1050, 600);
        dialog.show();
        auto *box = dialog.search_box();
        auto *nav = dialog.findChild<NavigationInterface *>("SettingsNavigation");
        QTRY_VERIFY(nav->subItemCount("shortcuts") > 1);
        const QString pin_group = i18n_tr("Pinned Window", "ShortcutTabs");
        QCOMPARE(nav->currentSubItem(), 0);
        const QString lock = inapp_label(&dialog, "inapp_pin_lock");
        box->edit()->setText(lock);
        QTRY_VERIFY(box->results_visible() && !box->results().empty());
        const auto result = box->results().front();
        QCOMPARE(result.title, lock);
        QCOMPARE(result.path, (QStringList{settings_tr("Shortcuts"), pin_group}.join(" / ")));
        QPointer<QWidget> target = result.target;
        QVERIFY(target);
        QTest::keyClick(box->edit(), Qt::Key_Return);
        QTRY_COMPARE(nav->subItem(nav->currentSubRoute(), nav->currentSubItem())->text(), pin_group);
        QTRY_VERIFY(flash_over(target));
        auto *scroll = qobject_cast<QScrollArea *>(dialog.content_stack->currentWidget());
        const QRect in_view(target->mapTo(scroll->viewport(), QPoint()), target->size());
        QVERIFY(scroll->viewport()->rect().contains(in_view));

        // The mouse page, not built yet.
        box->edit()->clear();
        const QString select_region = settings_tr("Select Thumbnail Region");
        box->edit()->setText(select_region);
        QTRY_VERIFY(box->results_visible() && !box->results().empty() && box->results().front().title == select_region);
        QCOMPARE(box->results().front().path, (QStringList{settings_tr("Mouse Shortcuts"), pin_group}.join(" / ")));
        target = box->results().front().target;
        QTest::keyClick(box->edit(), Qt::Key_Return);
        QCOMPARE(dialog.content_stack->currentIndex(), 10);
        QTRY_COMPARE(nav->currentSubRoute(), QString("mouse"));
        QTRY_COMPARE(nav->subItem(nav->currentSubRoute(), nav->currentSubItem())->text(), pin_group);
        QTRY_VERIFY(target && flash_over(target));
    }

    // The groups of the two shortcut pages are listed under their navigation items: the global group, then one per window.
    void shortcutPagesListOneGroupPerWindow() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.resize(1050, 750);
        dialog.show();
        auto *nav = dialog.findChild<NavigationInterface *>("SettingsNavigation");
        const auto window = [](const char *name) { return i18n_tr(name, "ShortcutTabs"); };
        dialog.on_nav_changed(0, "shortcuts");
        QTRY_COMPARE(nav->subItemCount("shortcuts"), 5);
        QCOMPARE(nav->subItemTitles("shortcuts"), (QStringList{dialog.tr_text("Global Hotkeys"), window("Screenshot Window"), window("Annotation Tools"), window("Pinned Window"), window("Clipboard Window")}));
        dialog.on_nav_changed(10, "mouse");
        QTRY_COMPARE(nav->subItemCount("mouse"), 5);
        QCOMPARE(nav->subItemTitles("mouse"),
                 (QStringList{window("Global Mouse Shortcuts"), window("Screenshot Window"), window("Long Screenshot Window"), window("Pinned Window"), window("Clipboard Window")}));
        // Each action row sits in its window's group, and no row is left outside the groups.
        auto *scroll = qobject_cast<QScrollArea *>(dialog.content_stack->currentWidget());
        const auto groups = page_groups(scroll);
        QCOMPARE(int(groups.size()), 5);
        for (const auto &action : LONG_SCREENSHOT_MOUSE_ACTIONS()) QVERIFY(groups[2]->isAncestorOf(dialog.behavior_control(QString("mouse_long_") + action.key)));
        for (const auto &action : PIN_MOUSE_ACTIONS()) QVERIFY(groups[3]->isAncestorOf(dialog.behavior_control(QString("mouse_pin_") + action.key)));
    }

    // The pin page's buttons to the pin shortcuts scroll to the pin group and mark it in the navigation.
    void pinShortcutEntriesScrollToThePinGroup() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.resize(1050, 600);
        dialog.show();
        auto *nav = dialog.findChild<NavigationInterface *>("SettingsNavigation");
        const QString pin_group = i18n_tr("Pinned Window", "ShortcutTabs");
        for (const bool mouse : {false, true}) {
            dialog.on_nav_changed(11, "pin");
            dialog.show_pin_shortcuts(mouse);
            QCOMPARE(dialog.content_stack->currentIndex(), mouse ? 10 : 0);
            auto *scroll = qobject_cast<QScrollArea *>(dialog.content_stack->currentWidget());
            auto *group = dialog.attr_widget(mouse ? "mouse_pin_group" : "inapp_pin_group");
            QVERIFY(group);
            QTRY_COMPARE(nav->currentSubRoute(), QString(mouse ? "mouse" : "shortcuts"));
            QTRY_COMPARE(nav->subItem(nav->currentSubRoute(), nav->currentSubItem())->text(), pin_group);
            QTRY_VERIFY(!dialog.page_outline()->animating());
            // The group's title is at the top of the view, unless the page ends before it can get there.
            const auto *bar = scroll->verticalScrollBar();
            QVERIFY(bar->value() > 0);
            const int top = group_top_of(scroll, group) - bar->value();
            QVERIFY2(top >= 0 && (top <= dialog_scaled(PageOutline::TOP_MARGIN) + 2 || bar->value() == bar->maximum()), qPrintable(QString::number(top)));
        }
    }

    // Whatever search lists can be brought on screen: its page, its tab, scrolled into view.
    void everySearchResultCanBeShown() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        while (dialog.build_next_page()) {}
        int rows = 0;
        for (const auto &entry : dialog.search_entries()) {
            const QString where = entry.path + " > " + entry.title;
            dialog.show_search_result(entry);
            QVERIFY2(dialog.content_stack->currentIndex() == entry.stack_index, qPrintable(where));
            if (!entry.target) continue;
            ++rows;
            QPointer<QWidget> target = entry.target;
            QTRY_VERIFY2_WITH_TIMEOUT(flash_over(target), qPrintable(where), 1000);
            QVERIFY2(target->isVisible(), qPrintable(where + " is not shown"));
            auto *scroll = qobject_cast<QScrollArea *>(dialog.content_stack->currentWidget());
            QWidget *view = scroll->viewport();
            // Vertically: the shortcut pages scroll sideways in a narrow window. A row that fits is
            // wholly in view; a taller group starts in it.
            const int top = target->mapTo(view, QPoint()).y();
            const QString position = QString(": %1..%2 in %3").arg(top).arg(top + target->height()).arg(view->height());
            if (target->height() <= view->height()) QVERIFY2(top >= 0 && top + target->height() <= view->height(), qPrintable(where + position));
            else QVERIFY2(top >= 0 && top < view->height(), qPrintable(where + position));
        }
        QVERIFY2(rows > 150, qPrintable(QString::number(rows)));
    }

    // Every provider's card is listed under its name, folded rows included; a result opens
    // that provider's cards and unfolds Advanced, without changing the engine.
    void searchReachesEveryProvidersCard() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        dialog.on_nav_changed(4, "translation");
        QCoreApplication::processEvents();
        const auto entries = dialog.search_entries();
        const auto listed = [&entries](QWidget *inside) {
            return std::count_if(entries.begin(), entries.end(), [inside](const SettingsSearchEntry &entry) { return entry.target && inside->isAncestorOf(entry.target); });
        };
        // Its name, at least one setting to work and the two pacing rows.
        for (auto it = dialog.provider_sections.begin(); it != dialog.provider_sections.end(); ++it) QVERIFY2(listed(it.value()) >= 4, qPrintable(it.key()));
        // The options' rows as well.
        QVERIFY(listed(dialog.attr_widget("translation_options_group")) >= 4);

        auto *combo = dialog.attr<QComboBox>("translation_provider_combo");
        const QString engine = combo->currentData().toString();
        const QString provider = engine == "amazon" ? "azure" : "amazon";
        QWidget *card = dialog.provider_sections.value(provider);
        QVERIFY(!card->isVisible());
        const auto pacing = dialog.translation_registry->metadata(provider).pacing.front();
        QWidget *edit = dialog.provider_field_widgets->value(pacing.config_key);
        const auto found = std::find_if(entries.begin(), entries.end(), [edit](const SettingsSearchEntry &entry) { return entry.kind == SettingsSearchEntry::Kind::Item && entry.target && entry.target->isAncestorOf(edit); });
        QVERIFY(found != entries.end());
        QCOMPARE(found->title, settings_tr(pacing.label.toUtf8().constData()));
        QVERIFY2(found->path.endsWith(" / " + provider_chip(dialog, provider)->text()), qPrintable(found->path));
        dialog.show_search_result(*found);
        QTRY_VERIFY(edit->isVisible());
        QVERIFY(card->isVisible());
        QVERIFY(provider_chip(dialog, provider)->isChecked());
        QCOMPARE(combo->currentData().toString(), engine);
    }

    // The engine row's gear scrolls the page to the API services group and shows the engine's cards.
    void configureButtonScrollsToTheApiServicesOfTheEngine() {
        const QString provider = config_->get_translation_provider();
        const auto restore = qScopeGuard([this, provider] { config_->set_translation_provider(provider); });
        config_->set_translation_provider("deepseek");
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.resize(1050, 600);
        dialog.show();
        dialog.on_nav_changed(4, "translation");
        auto *nav = dialog.findChild<NavigationInterface *>("SettingsNavigation");
        auto *scroll = qobject_cast<QScrollArea *>(dialog.content_stack->currentWidget());
        QTRY_COMPARE(nav->subItemCount("translation"), 3);
        auto *group = dialog.attr_widget("translation_api_group");
        auto *bar = scroll->verticalScrollBar();
        provider_chip(dialog, "deepl")->click();
        QCOMPARE(bar->value(), 0);
        QVERIFY(group_top_of(scroll, group) > bar->value() + scroll->viewport()->height() / 2);

        dialog.attr<QPushButton>("translation_configure_button")->click();
        QVERIFY(provider_chip(dialog, "deepseek")->isChecked());
        QVERIFY(dialog.provider_sections.value("deepseek")->isVisible());
        QVERIFY(!dialog.provider_sections.value("deepl")->isVisible());
        // A smooth scroll that ends with the group at the top, or at the end of the page.
        const auto expected = [&] { return std::clamp(group_top_of(scroll, group) - dialog_scaled(PageOutline::TOP_MARGIN), 0, bar->maximum()); };
        QTRY_VERIFY(bar->value() > 0);
        QTRY_COMPARE(bar->value(), expected());
        QTRY_COMPARE(nav->currentSubItem(), 2);
        QVERIFY(provider_chip(dialog, "deepseek")->isVisible());

        // Again from the top, through the helper the other pages use.
        bar->setValue(0);
        QTRY_COMPARE(nav->currentSubItem(), 0);
        show_translation_provider(&dialog, "baidu");
        QTRY_COMPARE(bar->value(), expected());
        QVERIFY(provider_chip(dialog, "baidu")->isChecked());
    }

    // A search result inside the API services shows its provider and the field in the view.
    void searchJumpToAnApiFieldBringsItIntoView() {
        const QString provider = config_->get_translation_provider();
        const auto restore = qScopeGuard([this, provider] { config_->set_translation_provider(provider); });
        config_->set_translation_provider("deepl");
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.resize(1050, 600);
        dialog.show();
        dialog.on_nav_changed(4, "translation");
        auto *nav = dialog.findChild<NavigationInterface *>("SettingsNavigation");
        auto *scroll = qobject_cast<QScrollArea *>(dialog.content_stack->currentWidget());
        QTRY_COMPARE(nav->subItemCount("translation"), 3);
        QCOMPARE(scroll->verticalScrollBar()->value(), 0);
        QWidget *field = dialog.provider_field_widgets->value("custom_llm_temperature");
        QVERIFY(!field->isVisible());
        const auto entries = dialog.search_entries();
        const auto found = std::find_if(entries.begin(), entries.end(), [field](const SettingsSearchEntry &entry) { return entry.kind == SettingsSearchEntry::Kind::Item && entry.target && entry.target->isAncestorOf(field); });
        QVERIFY(found != entries.end());
        dialog.show_search_result(*found);
        QTRY_VERIFY(field->isVisible());
        QVERIFY(provider_chip(dialog, "custom_llm")->isChecked());
        const auto in_view = [&] {
            const int top = field->mapTo(scroll->viewport(), QPoint()).y();
            return top >= 0 && top + field->height() <= scroll->viewport()->height();
        };
        QTRY_VERIFY(in_view());
        QCOMPARE(nav->currentSubItem(), 2);
    }

    void searchKeysStayInTheField() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        while (dialog.build_next_page()) {}
        auto *box = dialog.search_box();
        const QString query = settings_tr("Pin");
        box->edit()->setText(query);
        QVERIFY(box->results_visible());
        auto *list = box->result_list();
        QVERIFY(list->count() > 2);
        QCOMPARE(list->currentRow(), 0);
        QTest::keyClick(box->edit(), Qt::Key_Down);
        QCOMPARE(list->currentRow(), 1);
        QTest::keyClick(box->edit(), Qt::Key_Up);
        QTest::keyClick(box->edit(), Qt::Key_Up);
        QCOMPARE(list->currentRow(), 0);

        // Esc closes the list, then clears the field; the window stays open.
        QTest::keyClick(box->edit(), Qt::Key_Escape);
        QVERIFY(!box->results_visible());
        QVERIFY(dialog.isVisible());
        QCOMPARE(box->edit()->text(), query);
        QTest::keyClick(box->edit(), Qt::Key_Down);
        QVERIFY(box->results_visible());
        // A press elsewhere closes it too.
        QTest::mousePress(dialog.content_stack, Qt::LeftButton, {}, QPoint(5, 5));
        QVERIFY(!box->results_visible());
        QTest::mouseRelease(dialog.content_stack, Qt::LeftButton, {}, QPoint(5, 5));
        QTest::keyClick(box->edit(), Qt::Key_Escape);
        QVERIFY(box->edit()->text().isEmpty());
        QVERIFY(dialog.isVisible());

        // Nothing found says so, once no page is left to build.
        QTest::keyClicks(box->edit(), "qqzzxxjj");
        QTRY_VERIFY(box->results_visible());
        QVERIFY(box->empty_label()->isVisible());
        QVERIFY(!list->isVisible());

        // Focus coming back with the window leaves a closed list closed; focus from a click opens it.
        box->edit()->setText(query);
        QVERIFY(box->results_visible());
        box->hide_results();
        QFocusEvent back(QEvent::FocusIn, Qt::ActiveWindowFocusReason);
        QApplication::sendEvent(box->edit(), &back);
        QVERIFY(!box->results_visible());
        QFocusEvent clicked(QEvent::FocusIn, Qt::MouseFocusReason);
        QApplication::sendEvent(box->edit(), &clicked);
        QVERIFY(box->results_visible());

        // A click opens a result.
        box->edit()->clear();
        box->edit()->setText(settings_tr("Zoom Anchor"));
        QVERIFY(list->isVisible());
        QCOMPARE(box->results().front().title, settings_tr("Zoom Anchor"));
        const QPoint first = list->visualItemRect(list->item(0)).center();
        QTest::mousePress(list->viewport(), Qt::LeftButton, {}, first);
        QVERIFY(box->results_visible());
        QTest::mouseRelease(list->viewport(), Qt::LeftButton, {}, first);
        QVERIFY(!box->results_visible());
        QCOMPARE(dialog.content_stack->currentIndex(), 11);

        // An empty field leaves Esc to the window.
        box->edit()->clear();
        QTest::keyClick(box->edit(), Qt::Key_Escape);
        QVERIFY(!dialog.isVisible());
    }

    // Ctrl+F reaches the search from a combo box, which would otherwise search its own items; a
    // shortcut editor records it instead.
    void ctrlFOpensTheSearchUnlessAShortcutIsBeingRecorded() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        dialog.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&dialog));
        auto *combo = dialog.attr<QComboBox>("cursor_move_combo");
        combo->setFocus();
        QTRY_VERIFY(combo->hasFocus());
        const int combo_index = combo->currentIndex();
        QTest::keyClick(combo, Qt::Key_F, Qt::ControlModifier);
        QVERIFY(dialog.search_box()->edit()->hasFocus());
        QCOMPARE(combo->currentIndex(), combo_index);

        auto *edit = dialog.inapp_edits.front().second;
        const QString before = edit->text();
        const auto restore = qScopeGuard([&dialog, edit, before] {
            edit->setText(before);
            dialog.apply_settings();
        });
        edit->setFocus();
        QTRY_VERIFY(edit->hasFocus());
        QTest::keyClick(edit, Qt::Key_F, Qt::ControlModifier);
        QCOMPARE(edit->text(), QString("ctrl+f"));
        QVERIFY(edit->hasFocus());
    }

    // The field and its list follow the window scale, not the panel scale.
    void searchFollowsTheWindowScale() {
        const auto restore = qScopeGuard([] {
            get_dialog_scale().set_percent(100);
            get_ui_scale().set_percent(100);
        });
        const auto measure = [this] {
            SettingsDialog dialog(config_, config_->get_hotkey());
            dialog.show();
            dialog.resize(1800, 1000);
            while (dialog.build_next_page()) {}
            auto *box = dialog.search_box();
            box->edit()->setText(settings_tr("Zoom Step"));
            return QList<int>{box->edit()->height(), box->popup()->width(), box->result_list()->sizeHintForRow(0)};
        };
        get_dialog_scale().set_percent(100);
        const auto normal = measure();
        QCOMPARE(normal.mid(1), (QList<int>{480, 56}));
        get_dialog_scale().set_percent(150);
        const auto large = measure();
        QCOMPARE(large.mid(1), (QList<int>{720, 84}));
        QVERIFY(large[0] > normal[0]);
        get_ui_scale().set_percent(200);
        QCOMPARE(measure(), large);
    }

    // The field's hint, the empty result and every result title in every language, in the
    // narrowest window at each window scale.
    void searchTextsFitEveryLanguageInTheNarrowestWindow() {
        // Text widths need real fonts, which the offscreen platform does not have.
        if (QGuiApplication::platformName() != "windows") QSKIP("needs -platform windows");
        auto &scale = get_dialog_scale();
        const int percent_before = scale.percent();
        const QString language = I18nManager::get_current_language();
        const auto restore = qScopeGuard([&scale, percent_before, language] {
            scale.set_percent(percent_before);
            I18nManager::load_language(language);
        });
        QStringList problems;
        for (const int percent : {100, 150, 200}) {
            scale.set_percent(percent);
            for (const char *code : {"zh", "zh_TW", "en", "ja", "ko"}) {
                QVERIFY(I18nManager::load_language(code));
                const QString where = QString("%1 at %2%").arg(code).arg(percent);
                SettingsDialog dialog(config_, config_->get_hotkey());
                dialog.setAttribute(Qt::WA_DontShowOnScreen);
                dialog.fit_to_available({4000, dialog.height()});
                dialog.resize(0, dialog.height());
                dialog.show();
                while (dialog.build_next_page()) {}
                QApplication::processEvents();
                auto *edit = dialog.search_box()->edit();
                // Between the search icon and the clear button.
                int left = edit->inputMethodQuery(Qt::ImCursorRectangle).toRect().left(), right = edit->contentsRect().right();
                for (auto *button : edit->findChildren<QToolButton *>()) {
                    if (button->geometry().center().x() < edit->width() / 2) left = std::max(left, button->geometry().right());
                    else right = std::min(right, button->geometry().left());
                }
                const int hint = edit->fontMetrics().horizontalAdvance(edit->placeholderText());
                if (hint > right - left) problems << QString("%1: the search hint needs %2, has %3").arg(where).arg(hint).arg(right - left);

                auto *box = dialog.search_box();
                QTest::keyClicks(edit, "qqzzxxjj");
                QApplication::processEvents();
                auto *empty = box->empty_label();
                if (!empty->isVisible() || empty->height() < empty->heightForWidth(empty->width()) || empty->width() < empty->minimumSizeHint().width())
                    problems << where + ": the no-result text is cut off";
                if (!dialog.rect().contains(QRect(box->popup()->mapTo(&dialog, QPoint()), box->popup()->size()))) problems << where + ": the list leaves the window";

                // Titles are never shortened; page results keep their tag.
                edit->clear();
                edit->setText(settings_tr("Pin"));
                const int row_width = box->result_list()->viewport()->width();
                for (const auto &entry : dialog.search_entries()) {
                    const QString tag = entry.kind == SettingsSearchEntry::Kind::Page ? settings_tr("Page") : entry.path;
                    const auto widths = search_row_widths(row_width, entry.title, tag);
                    if (widths.title_room < widths.title_needed) problems << QString("%1: \"%2\" needs %3, has %4").arg(where, entry.title).arg(widths.title_needed).arg(widths.title_room);
                    if (entry.kind == SettingsSearchEntry::Kind::Page && widths.tag == 0) problems << QString("%1: the page tag of \"%2\" is dropped").arg(where, entry.title);
                }
            }
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }
    // Saves each page as PNG for visual review when JIETUBA_SETTINGS_SHOTS names a folder;
    // needs -platform windows for real fonts.
    void renderPagesForReview() {
        const QString out = qEnvironmentVariable("JIETUBA_SETTINGS_SHOTS");
        if (out.isEmpty()) QSKIP("JIETUBA_SETTINGS_SHOTS is not set.");
        // JIETUBA_SETTINGS_SHOTS_LANGUAGE renders another interface language.
        const QString language = I18nManager::get_current_language();
        const auto restore_language = qScopeGuard([language] { I18nManager::load_language(language); });
        if (const QString shown = qEnvironmentVariable("JIETUBA_SETTINGS_SHOTS_LANGUAGE"); !shown.isEmpty()) QVERIFY(I18nManager::load_language(shown));
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.setAttribute(Qt::WA_DontShowOnScreen);
        dialog.show();
        const std::pair<int, const char *> pages[] = {{0, "shortcuts"}, {10, "mouse"}, {1, "capture"}, {11, "pin"}, {12, "screen_annotation"},
                                                      {4, "translation"}, {3, "appearance"}, {5, "log"}, {6, "other"}, {8, "about"}};
        for (const auto &[index, route] : pages) {
            dialog.on_nav_changed(index, route);
            QTest::qWait(200);
            QVERIFY(dialog.grab().save(QDir(out).filePath(QString("%1_%2.png").arg(index, 2, 10, QChar('0')).arg(route))));
            // A page taller than the window is saved scrolled to the end as well.
            auto *scroll = qobject_cast<QScrollArea *>(dialog.content_stack->currentWidget());
            if (!scroll || scroll->verticalScrollBar()->maximum() == 0) continue;
            scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
            QTest::qWait(200);
            QVERIFY(dialog.grab().save(QDir(out).filePath(QString("%1_%2_end.png").arg(index, 2, 10, QChar('0')).arg(route))));
        }
        // The translation page's API services, on a provider of each kind.
        dialog.on_nav_changed(4, "translation");
        for (const char *provider : {"deepl", "deepseek", "volcengine", "custom_llm"}) {
            show_translation_provider(&dialog, provider);
            QTest::qWait(200);
            QVERIFY(dialog.grab().save(QDir(out).filePath(QString("04_translation_api_%1.png").arg(provider))));
            // The whole card, below the window's height as well.
            auto *scroll = qobject_cast<QScrollArea *>(dialog.content_stack->currentWidget());
            QVERIFY(scroll->widget()->grab().save(QDir(out).filePath(QString("04_translation_api_%1_full.png").arg(provider))));
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
    // Saves the dialogs opened from the settings window, light and dark, when JIETUBA_SETTINGS_SHOTS names a folder.
    void renderDialogsForReview() {
        const QString out = qEnvironmentVariable("JIETUBA_SETTINGS_SHOTS");
        if (out.isEmpty()) QSKIP("JIETUBA_SETTINGS_SHOTS is not set.");
        const QString language = I18nManager::get_current_language();
        const auto mode = get_ui_theme()->mode();
        const auto restore = qScopeGuard([language, mode] {
            I18nManager::load_language(language);
            get_ui_theme()->set_mode(ui_theme_mode_value(mode), false);
        });
        const auto save = [&out](QWidget &widget, const QString &name) {
            widget.setAttribute(Qt::WA_DontShowOnScreen);
            widget.show();
            QTest::qWait(100);
            QVERIFY(widget.grab().save(QDir(out).filePath(name + ".png")));
            widget.hide();
        };
        for (const char *theme : {"light", "dark"}) {
            get_ui_theme()->set_mode(theme, false);
            for (const char *lang : {"zh", "en", "ja"}) {
                QVERIFY(I18nManager::load_language(lang));
                FileNameDialog names(file_names::defaults());
                save(names, QString("dialog_filename_%1_%2").arg(lang, theme));
                StandardDialog confirm(nullptr, settings_tr("Reset This Page"), settings_tr("Restore the default settings of this page?"));
                confirm.button_box->addButton(i18n_tr("Yes", "StandardDialog"), QDialogButtonBox::YesRole);
                confirm.button_box->addButton(i18n_tr("No", "StandardDialog"), QDialogButtonBox::NoRole);
                save(confirm, QString("dialog_confirm_%1_%2").arg(lang, theme));
            }
            QVERIFY(I18nManager::load_language("zh"));
            FileNameDialog broken(file_names::defaults());
            broken.findChild<QLineEdit *>("filename_rule_5")->setText("bad?");
            save(broken, QString("dialog_filename_error_%1").arg(theme));
            StandardDialog choice(nullptr, settings_tr("Existing Clipboard Data"),
                                  settings_tr("The selected folder already contains clipboard data.\n"
                                              "Use That Data: switch to it and leave the current data where it is.\n"
                                              "Replace: move the current data there; the data already in that folder will be deleted."));
            choice.button_box->addButton(settings_tr("Use That Data"), QDialogButtonBox::AcceptRole);
            choice.button_box->addButton(settings_tr("Replace"), QDialogButtonBox::DestructiveRole);
            choice.button_box->addButton(settings_tr("Cancel"), QDialogButtonBox::RejectRole);
            save(choice, QString("dialog_choice_%1").arg(theme));
            StandardDialog info(nullptr, settings_tr("Log"), settings_tr("No log files yet. Please start and use the app first."));
            info.button_box->addButton(i18n_tr("OK", "StandardDialog"), QDialogButtonBox::AcceptRole);
            save(info, QString("dialog_info_%1").arg(theme));
            ColorFormatDialog formats(color_formats::default_formats());
            save(formats, QString("dialog_color_formats_%1").arg(theme));
            SpecialPasteDialog paste(special_paste::default_entries());
            save(paste, QString("dialog_special_paste_%1").arg(theme));
            PpocrModelDialog models;
            save(models, QString("dialog_models_%1").arg(theme));
            PpocrImportForm import_form;
            save(import_form, QString("dialog_model_import_%1").arg(theme));
            upload::Target blank;
            blank.id = "new";
            UploadTargetDialog upload_dialog(blank, {}, true);
            save(upload_dialog, QString("dialog_upload_%1").arg(theme));
            QMap<QString, QIcon> icons;
            for (const auto &key : toolbar_layout::default_order()) icons[key] = QIcon();
            ToolbarLayoutDialog toolbar(toolbar_layout::default_layout(), icons);
            save(toolbar, QString("dialog_toolbar_%1").arg(theme));
            ColorPickerDialog picker(QColor(230, 153, 60));
            save(picker, QString("dialog_color_picker_%1").arg(theme));
        }
    }
    void fileNameRulesValidateAndSaveWithoutOverwriting() {
        QTemporaryDir temp;
        QSettings settings(temp.filePath("names.ini"), QSettings::IniFormat);
        ToolSettingsManager config(settings);
        QCOMPARE(file_names::load(config), file_names::defaults());
        const auto date = QDateTime(QDate(2026, 11, 15), QTime(12, 34, 56, 789));
        QCOMPARE(file_names::render("shot_{time}_{ms}", {}, date), QString("shot_20261115_123456_789"));
        QCOMPARE(file_names::render("{prefix}_{time}", "long_horizontal", date), QString("long_horizontal_20261115_123456"));
        QCOMPARE(file_names::render("article_{direction}_{time}", "long_horizontal", date), QString("article_horizontal_20261115_123456"));
        for (const QString bad : {"", "../escape", "a\\b", "bad:name", "tail.", "tail ", "CON", "lpt1.png", "{unknown}"})
            QVERIFY2(!file_names::problem(bad, file_names::Screenshot).isEmpty(), qPrintable(bad));
        QVERIFY(!file_names::problem(QString(121, 'a'), file_names::Screenshot).isEmpty());
        QVERIFY(!file_names::problem(QString("{time}").repeated(20), file_names::Screenshot).isEmpty());
        QCOMPARE(file_names::problem(QString(106, 'a') + "{time}", file_names::Screenshot), QString("The generated file name is too long (maximum 120 characters)."));
        QCOMPARE(file_names::problem("capture.PNG", file_names::Screenshot), QString("Leave out the file extension; it is added automatically."));
        QVERIFY(!file_names::problem("{prefix}", file_names::Screenshot).isEmpty());
        QVERIFY(!file_names::problem("{direction}", file_names::Screenshot).isEmpty());
        QVERIFY(!file_names::problem("{time}", file_names::OriginalSuffix).isEmpty());
        for (const auto &reserved : {QString::fromUtf8("COM¹"), QString::fromUtf8("lpt².txt"), QString::fromUtf8("CoM³")})
            QVERIFY(!file_names::problem(reserved, file_names::Screenshot).isEmpty());
        QVERIFY(file_names::problem("CON", file_names::OriginalSuffix).isEmpty());
        QVERIFY(file_names::problem("{prefix}_{time}", file_names::LongScreenshot).isEmpty());
        auto rules = QStringList{"shot", "pin", "article_{direction}", "animation", "video", "source"};
        config.set_save_filename_patterns(rules);
        QCOMPARE(file_names::load(config), rules);
        config.set_screenshot_save_path(temp.path());
        SaveService service(config);
        QImage image(2, 2, QImage::Format_RGB32);
        image.fill(Qt::red);
        const auto first = service.save_qimage(image, {}, {}, {}, "PNG", {});
        QVERIFY(first.first);
        QCOMPARE(QFileInfo(*first.second).fileName(), QString("shot.png"));
        const auto second = service.save_qimage(image, {}, {}, {}, "PNG", {});
        QVERIFY(second.first);
        QCOMPARE(QFileInfo(*second.second).fileName(), QString("shot_1.png"));
        QCOMPARE(QImage(*first.second).pixelColor(0, 0), QColor(Qt::red));
        const auto pin = service.save_qimage(image, {}, "pinned", {}, "PNG", {});
        QVERIFY(pin.first);
        QCOMPARE(QFileInfo(*pin.second).fileName(), QString("pin.png"));
        const auto long_shot = service.save_qimage(image, {}, "horizontal", {}, "PNG", {});
        QVERIFY(long_shot.first);
        QCOMPARE(QFileInfo(*long_shot.second).fileName(), QString("article_horizontal.png"));
        QCOMPARE(file_names::stem(config, file_names::Gif), QString("animation"));
        QCOMPARE(file_names::stem(config, file_names::Video), QString("video"));
        const auto original = SaveService::save_beside(image, *first.second, file_names::load(config).at(file_names::OriginalSuffix), {});
        QVERIFY(original);
        QCOMPARE(QFileInfo(*original).fileName(), QString("shot_source.png"));
        rules[0] = "../bad";
        config.set_save_filename_patterns(rules);
        QCOMPARE(file_names::load(config)[0], file_names::defaults()[0]);
        rules[file_names::LongScreenshot] = "{prefix}_{time}";
        config.set_save_filename_patterns(rules);
        QCOMPARE(file_names::load(config)[file_names::LongScreenshot], file_names::defaults()[file_names::LongScreenshot]);
        QVERIFY(!file_names::load(config)[file_names::LongScreenshot].contains("{prefix}"));
        rules[file_names::Screenshot] = "report_{time}.PNG";
        config.set_save_filename_patterns(rules);
        QCOMPARE(file_names::load(config)[file_names::Screenshot], QString("report_{time}"));
    }

    void fileNameDialogValidatesAndReturnsEdits() {
        FileNameDialog editor(file_names::defaults());
        auto *edit = editor.findChild<QLineEdit *>("filename_rule_0");
        auto *ok = editor.findChild<QPushButton *>("filename_ok");
        QVERIFY(edit && ok);
        edit->setText("bad/name");
        QVERIFY(!ok->isEnabled());
        edit->setText("my_capture_{time}");
        QVERIFY(ok->isEnabled());
        QCOMPARE(editor.patterns()[0], QString("my_capture_{time}"));
        auto *long_edit = editor.findChild<QLineEdit *>("filename_rule_2");
        QVERIFY(long_edit);
        QVERIFY(!long_edit->text().contains("{prefix}"));
        long_edit->setText("article_{direction}_{time}");
        QVERIFY(ok->isEnabled());
        bool custom_preview = false;
        for (auto *label : editor.findChildren<QLabel *>()) custom_preview |= label->toolTip().contains("article_") && !label->toolTip().contains("{direction}");
        QVERIFY(custom_preview);
        auto *pin_edit = editor.findChild<QLineEdit *>("filename_rule_1");
        auto *suffix_edit = editor.findChild<QLineEdit *>("filename_rule_5");
        auto *error = editor.findChild<QLabel *>("filename_error");
        QVERIFY(pin_edit && suffix_edit && error);
        pin_edit->setText("bad?");
        suffix_edit->setText("bad?");
        // The suffix row is shown above the pin row, so its problem is reported first.
        QVERIFY2(error->text().startsWith(suffix_edit->accessibleName()), qPrintable(error->text()));
        for (auto *button : editor.findChildren<QPushButton *>())
            if (button->text() == i18n_tr("Restore defaults", "FileNameDialog")) button->click();
        QCOMPARE(editor.patterns(), file_names::defaults());
        edit->setText("accepted_{time}");
        ok->click();
        QCOMPARE(editor.result(), int(QDialog::Accepted));
    }

    void fileNameSettingsButtonSavesAndCancelKeepsRules() {
        QSettings settings(dir_.filePath("name-button.ini"), QSettings::IniFormat);
        ToolSettingsManager config(settings);
        SettingsDialog dialog(&config, config.get_hotkey());
        dialog.build_all_pages();
        auto *button = dialog.attr<QPushButton>("filename_settings_button");
        QVERIFY(button);
        QTimer::singleShot(0, [] {
            auto *modal = QApplication::activeModalWidget();
            QVERIFY(modal);
            auto *edit = modal->findChild<QLineEdit *>("filename_rule_0");
            QVERIFY(edit);
            edit->setText("custom_{time}");
            modal->findChild<QPushButton *>("filename_ok")->click();
        });
        button->click();
        QVERIFY(dialog.apply_settings());
        QCOMPARE(file_names::load(config)[0], QString("custom_{time}"));
        QTimer::singleShot(0, [] {
            auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(modal);
            modal->findChild<QLineEdit *>("filename_rule_0")->setText("discarded");
            modal->reject();
        });
        button->click();
        QVERIFY(dialog.apply_settings());
        QCOMPARE(file_names::load(config)[0], QString("custom_{time}"));
    }

    void fileNameDialogPreviewsStayStableAndUseTheSaveFormat() {
        FileNameDialog editor(file_names::defaults(), nullptr, "WEBP");
        editor.show();
        QApplication::processEvents();
        auto *field = editor.findChild<QLineEdit *>("filename_rule_0");
        auto *preview = editor.findChild<QLabel *>("filename_preview_0");
        auto *video = editor.findChild<QLabel *>("filename_preview_4");
        auto *original = editor.findChild<QLabel *>("filename_preview_5");
        auto *error = editor.findChild<QLabel *>("filename_error");
        QVERIFY(field && preview && video && original && error);
        int previous_top = -1;
        for (const int kind : {file_names::Screenshot, file_names::OriginalSuffix, file_names::Pin, file_names::LongScreenshot, file_names::Gif, file_names::Video}) {
            const int top = editor.findChild<QLineEdit *>(QString("filename_rule_%1").arg(kind))->mapTo(&editor, QPoint()).y();
            QVERIFY2(top > previous_top, qPrintable(QString::number(kind)));
            previous_top = top;
        }
        QVERIFY(preview->toolTip().endsWith(".webp"));
        QVERIFY(original->toolTip().endsWith(".webp"));
        QVERIFY(editor.findChild<QLabel *>("filename_preview_3")->toolTip().endsWith(".gif"));
        const auto video_before = video->toolTip();
        QTest::qWait(25);
        field->setText("changed_{time}");
        QCOMPARE(video->toolTip(), video_before);
        field->setText("<b>plain text</b>");
        QCOMPARE(preview->textFormat(), Qt::PlainText);
        QVERIFY(preview->toolTip().contains("<b>plain text</b>"));
        QVERIFY(!error->text().isEmpty());
        editor.accept();
        QVERIFY(editor.isVisible());
        QTest::keyClick(field, Qt::Key_Return);
        QVERIFY(editor.isVisible());
        field->setText(QString(110, 'W'));
        QVERIFY(preview->toolTip().contains(QString(110, 'W')));
        QVERIFY(preview->text() != preview->toolTip());
        QVERIFY(preview->fontMetrics().horizontalAdvance(preview->text()) <= preview->contentsRect().width());
        field->setText("keep_this_{time}");
        QTest::keyClick(field, Qt::Key_Return);
        QCOMPARE(editor.result(), int(QDialog::Accepted));
        QCOMPARE(editor.patterns()[0], QString("keep_this_{time}"));
    }

    void fileNameDialogErrorsStayVisibleAndKeepPastedText() {
        const auto language = I18nManager::get_current_language();
        const auto scale = get_dialog_scale().percent();
        const auto restore = qScopeGuard([&] { get_dialog_scale().set_percent(scale); I18nManager::load_language(language); });
        for (const QString lang : {"zh", "zh_TW", "en", "ja", "ko"}) {
            QVERIFY(I18nManager::load_language(lang));
            for (const int percent : {100, 150, 200}) {
                get_dialog_scale().set_percent(percent);
                FileNameDialog editor(file_names::defaults());
                editor.resize(editor.minimumWidth(), dialog_scaled(300));
                editor.show();
                QApplication::processEvents();
                auto *scroll = editor.findChild<QScrollArea *>("filename_scroll");
                auto *error = editor.findChild<QLabel *>("filename_error");
                auto *ok = editor.findChild<QPushButton *>("filename_ok");
                auto *first = editor.findChild<QLineEdit *>("filename_rule_0");
                auto *last = editor.findChild<QLineEdit *>("filename_rule_4");
                QVERIFY(scroll && error && ok && first && last);
                QVERIFY(scroll->verticalScrollBar()->maximum() > 0);
                first->setText("bad?");
                QApplication::processEvents();
                QVERIFY(!ok->isEnabled());
                QVERIFY(error->text().startsWith(first->accessibleName()));
                QVERIFY(!first->accessibleDescription().isEmpty());
                QCOMPARE(first->toolTip(), first->accessibleDescription());
                QVERIFY(!scroll->isAncestorOf(error));
                QVERIFY(editor.rect().contains(QRect(error->mapTo(&editor, QPoint()), error->size())));
                first->setText("capture_{time}");
                QVERIFY(first->accessibleDescription().isEmpty());
                QVERIFY(first->toolTip().isEmpty());
                QVERIFY(ok->isEnabled());
                scroll->ensureWidgetVisible(last);
                last->selectAll();
                last->insert(QString(121, 'x'));
                QCOMPARE(last->text(), QString(121, 'x'));
                QVERIFY(!ok->isEnabled());
                QVERIFY(error->text().startsWith(last->accessibleName()));
                QVERIFY(editor.rect().contains(QRect(error->mapTo(&editor, QPoint()), error->size())));
                for (auto *field : editor.findChildren<QLineEdit *>()) {
                    bool has_label = false;
                    for (auto *label : editor.findChildren<QLabel *>()) has_label |= label->buddy() == field;
                    QVERIFY(has_label);
                    QVERIFY(!field->accessibleName().isEmpty());
                }
            }
        }
    }

    void fileNameDialogDoesNotMoveWhileTyping() {
        const auto language = I18nManager::get_current_language();
        const auto scale = get_dialog_scale().percent();
        const auto restore = qScopeGuard([&] { get_dialog_scale().set_percent(scale); I18nManager::load_language(language); });
        for (const QString lang : {"zh", "zh_TW", "en", "ja", "ko"}) {
            QVERIFY(I18nManager::load_language(lang));
            for (const int percent : {100, 150, 200}) {
                get_dialog_scale().set_percent(percent);
                FileNameDialog editor(file_names::defaults());
                editor.show();
                QApplication::processEvents();
                const auto geometry = editor.geometry();
                const auto fields = editor.findChildren<QLineEdit *>();
                const auto buttons = editor.findChildren<QPushButton *>();
                QMap<QWidget *, QRect> positions;
                for (auto *field : fields) positions[field] = QRect(field->mapTo(&editor, QPoint()), field->size());
                for (auto *button : buttons) positions[button] = QRect(button->mapTo(&editor, QPoint()), button->size());
                auto *field = editor.findChild<QLineEdit *>("filename_rule_0");
                auto *ok = editor.findChild<QPushButton *>("filename_ok");
                QVERIFY(field && ok);
                const auto unchanged = [&] {
                    QApplication::processEvents();
                    if (editor.geometry() != geometry) return false;
                    for (auto it = positions.cbegin(); it != positions.cend(); ++it)
                        if (QRect(it.key()->mapTo(&editor, QPoint()), it.key()->size()) != it.value()) return false;
                    return true;
                };
                field->setFocus();
                field->selectAll();
                for (const char ch : QString("page_{time}").toLatin1()) {
                    QTest::keyClicks(field, QString(QChar(ch)));
                    QVERIFY2(unchanged(), qPrintable(QString("%1 %2%% typing %3").arg(lang).arg(percent).arg(field->text())));
                }
                QVERIFY(ok->isEnabled());
                QTest::keyClicks(field, "?");
                QVERIFY(!ok->isEnabled());
                QVERIFY(unchanged());
                QTest::keyClick(field, Qt::Key_Backspace);
                QVERIFY(ok->isEnabled());
                QVERIFY(unchanged());
                field->setText(QString(110, 'W'));
                QVERIFY(ok->isEnabled());
                QVERIFY(unchanged());
                QInputMethodEvent preedit(QString::fromUtf8("中文输入"), {});
                QApplication::sendEvent(field, &preedit);
                QVERIFY(unchanged());
                field->selectAll();
                QInputMethodEvent commit;
                commit.setCommitString(QString::fromUtf8("网页收藏_{time}"));
                QApplication::sendEvent(field, &commit);
                QVERIFY(ok->isEnabled());
                QVERIFY(unchanged());
                QTest::keyClick(field, Qt::Key_Escape);
                QCOMPARE(editor.result(), int(QDialog::Rejected));
            }
        }
    }

    void fileNameDialogFitsEveryLanguageAndFollowsWindowScale() {
        if (QGuiApplication::platformName() != "windows") QSKIP("needs desktop fonts");
        const auto language = I18nManager::get_current_language();
        const auto scale = get_dialog_scale().percent();
        const auto panel_scale = get_ui_scale().percent();
        const auto restore = qScopeGuard([&] {
            get_dialog_scale().set_percent(scale);
            get_ui_scale().set_percent(panel_scale);
            I18nManager::load_language(language);
        });
        for (const QString lang : {"zh", "zh_TW", "en", "ja", "ko"}) {
            QVERIFY(I18nManager::load_language(lang));
            get_dialog_scale().set_percent(100);
            FileNameDialog base(file_names::defaults());
            const auto width = base.width();
            get_ui_scale().set_percent(150);
            FileNameDialog panel(file_names::defaults());
            QCOMPARE(panel.width(), width);
            get_dialog_scale().set_percent(150);
            FileNameDialog large(file_names::defaults());
            large.show();
            QTest::qWait(10);
            const auto shots = qEnvironmentVariable("JIETUBA_SETTINGS_SHOTS");
            if (!shots.isEmpty()) QVERIFY(large.grab().save(QDir(shots).filePath(QString("filename_%1.png").arg(lang))));
            QVERIFY(large.width() >= width * 1.49);
            for (auto *button : large.findChildren<QPushButton *>()) QVERIFY(button->width() >= button->sizeHint().width());
            for (auto *label : large.findChildren<QLabel *>()) {
                if (!label->isVisible()) continue;
                if (!label->wordWrap()) QVERIFY2(label->width() >= label->sizeHint().width(), qPrintable(label->text()));
                auto *body = large.findChild<QScrollArea *>("filename_scroll")->widget();
                auto *container = body->isAncestorOf(label) ? body : static_cast<QWidget *>(&large);
                QVERIFY(container->rect().contains(QRect(label->mapTo(container, QPoint()), label->size())));
            }
            auto *scroll = large.findChild<QScrollArea *>("filename_scroll");
            QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
            QVERIFY(large.height() <= large.screen()->availableGeometry().height());
        }
    }



    // A new target starts from the first provider; a mistake shows beside its own field, and the link a screenshot
    // would get follows the fields.
    void uploadDialogChecksFieldsBesideThemAndPreviewsTheLink() {
        upload::Target existing;
        existing.id = "old";
        existing.name = "Main";
        upload::Target blank;
        blank.id = "new";
        UploadTargetDialog dialog(blank, {existing}, true);
        QCOMPARE(dialog.endpoint_edit->text(), QString("https://s3.us-east-1.amazonaws.com"));
        QCOMPARE(dialog.region_edit->text(), QString("us-east-1"));
        QCOMPARE(dialog.addressing_combo->currentData().toString(), QString("virtual"));
        const auto shown = [&dialog](const char *field) {
            auto *label = dialog.errors.value(field);
            return label && !label->isHidden() && !label->text().isEmpty();
        };
        QVERIFY(!dialog.validate_fields());
        QVERIFY(shown("name") && shown("bucket") && shown("access_key_id") && shown("secret_access_key"));
        QVERIFY(!shown("endpoint") && !shown("region") && !shown("public_url"));
        QCOMPARE(dialog.errors.value("name")->text(), i18n_tr("Enter a name.", "UploadTargetDialog"));
        // A name used by another target in any letter case, an endpoint that is no http address, a bucket with a slash.
        dialog.name_edit->setText("main");
        dialog.endpoint_edit->setText("ftp://x");
        dialog.bucket_edit->setText("a/b");
        QVERIFY(!dialog.validate_fields());
        QVERIFY(shown("name") && shown("endpoint") && shown("bucket"));
        QCOMPARE(dialog.errors.value("name")->text(), i18n_tr("This name is already used.", "UploadTargetDialog"));
        // Editing a field stops showing what was said about it.
        dialog.name_edit->setText("Other");
        QVERIFY(!shown("name") && shown("endpoint"));
        dialog.endpoint_edit->setText("  https://s3.us-east-1.amazonaws.com ");
        dialog.bucket_edit->setText(" my-bucket ");
        dialog.access_key_edit->setText("AKID");
        dialog.secret_edit->setText("secret");
        QVERIFY(dialog.validate_fields());
        const auto target = dialog.target();
        QCOMPARE(target.id, QString("new"));
        QCOMPARE(target.name, QString("Other"));
        QCOMPARE(target.endpoint, QString("https://s3.us-east-1.amazonaws.com"));
        QCOMPARE(target.bucket, QString("my-bucket"));
        QCOMPARE(target.access_key_id, QString("AKID"));
        QCOMPARE(target.addressing, upload::Addressing::Virtual);
        // The preview is the link of an example name, with the prefix, and follows the addressing and the public address.
        const int year = QDate::currentDate().year();
        QCOMPARE(dialog.preview_text(), QString("https://my-bucket.s3.us-east-1.amazonaws.com/Screenshot-1a2b3c4d.png"));
        dialog.prefix_edit->setText("shots/{yyyy}");
        QCOMPARE(dialog.preview_text(), QString("https://my-bucket.s3.us-east-1.amazonaws.com/shots/%1/Screenshot-1a2b3c4d.png").arg(year));
        dialog.addressing_combo->setCurrentIndex(0);
        QCOMPARE(dialog.preview_text(), QString("https://s3.us-east-1.amazonaws.com/my-bucket/shots/%1/Screenshot-1a2b3c4d.png").arg(year));
        dialog.addressing_combo->setCurrentIndex(1);
        dialog.bucket_edit->setText("my.bucket");
        QVERIFY2(dialog.preview_text().startsWith("https://s3.us-east-1.amazonaws.com/my.bucket/"), qPrintable(dialog.preview_text()));
        dialog.public_edit->setText("https://cdn.test/img/");
        QCOMPARE(dialog.preview_text(), QString("https://cdn.test/img/shots/%1/Screenshot-1a2b3c4d.png").arg(year));
        dialog.public_edit->setText("cdn.test");
        QVERIFY(!dialog.validate_fields());
        QVERIFY(shown("public_url"));
        QVERIFY(!dialog.preview_text().startsWith("http"));
    }

    // Picking a provider fills its endpoint, region and addressing; the region of a provider that carries it in the
    // endpoint carries it along until the endpoint is changed by hand.
    void uploadDialogProvidersFillTheirTemplates() {
        upload::Target blank;
        blank.id = "new";
        UploadTargetDialog dialog(blank, {}, true);
        const auto pick = [&dialog](int index) {
            dialog.preset_combo->setCurrentIndex(index);
            emit dialog.preset_combo->activated(index);
        };
        pick(2);
        QCOMPARE(dialog.endpoint_edit->text(), QString("https://s3.oss-cn-hangzhou.aliyuncs.com"));
        QCOMPARE(dialog.region_edit->text(), QString("cn-hangzhou"));
        QCOMPARE(dialog.addressing_combo->currentData().toString(), QString("virtual"));
        pick(3);
        QCOMPARE(dialog.endpoint_edit->text(), QString("https://cos.ap-guangzhou.myqcloud.com"));
        QCOMPARE(dialog.region_edit->text(), QString("ap-guangzhou"));
        pick(1);
        QCOMPARE(dialog.endpoint_edit->text(), QString("https://<ACCOUNT_ID>.r2.cloudflarestorage.com"));
        QCOMPARE(dialog.region_edit->text(), QString("auto"));
        QCOMPARE(dialog.addressing_combo->currentData().toString(), QString("path"));
        // The R2 template is not an address until the account id is put in.
        QVERIFY(!dialog.validate_fields());
        QVERIFY(!dialog.errors.value("endpoint")->isHidden());
        dialog.endpoint_edit->setText("https://0123abcd.r2.cloudflarestorage.com");
        QVERIFY(dialog.errors.value("endpoint")->isHidden());
        pick(4);
        QCOMPARE(dialog.endpoint_edit->text(), QString("http://localhost:9000"));
        QCOMPARE(dialog.region_edit->text(), QString("us-east-1"));
        QCOMPARE(dialog.addressing_combo->currentData().toString(), QString("path"));
        pick(0);
        QCOMPARE(dialog.endpoint_edit->text(), QString("https://s3.us-east-1.amazonaws.com"));
        dialog.region_edit->setText("eu-west-1");
        emit dialog.region_edit->textEdited("eu-west-1");
        QCOMPARE(dialog.endpoint_edit->text(), QString("https://s3.eu-west-1.amazonaws.com"));
        dialog.endpoint_edit->setText("https://custom.example.com");
        dialog.region_edit->setText("ap-1");
        emit dialog.region_edit->textEdited("ap-1");
        QCOMPARE(dialog.endpoint_edit->text(), QString("https://custom.example.com"));
        // An existing target shows what it holds and the provider its endpoint belongs to.
        upload::Target saved;
        saved.id = "s";
        saved.name = "Oss";
        saved.endpoint = "https://s3.oss-cn-beijing.aliyuncs.com";
        saved.region = "cn-beijing";
        saved.bucket = "b";
        saved.access_key_id = "AK";
        saved.secret_access_key = "SK";
        saved.addressing = upload::Addressing::Virtual;
        UploadTargetDialog edit(saved, {}, false);
        QCOMPARE(edit.preset_combo->currentIndex(), 2);
        QCOMPARE(edit.name_edit->text(), QString("Oss"));
        QCOMPARE(edit.endpoint_edit->text(), saved.endpoint);
        QCOMPARE(edit.addressing_combo->currentData().toString(), QString("virtual"));
        QCOMPARE(edit.secret_edit->echoMode(), QLineEdit::Password);
        QVERIFY(edit.validate_fields());
        saved.endpoint = "https://storage.internal.example";
        UploadTargetDialog other(saved, {}, false);
        QCOMPARE(other.preset_combo->currentIndex(), 4);
        // An endpoint carrying the account is still the provider's, by its domain.
        saved.endpoint = "https://0123456789abcdef.r2.cloudflarestorage.com";
        UploadTargetDialog r2(saved, {}, false);
        QCOMPARE(r2.preset_combo->currentIndex(), 1);
    }

    void uploadDialogFollowsTheWindowScaleAndNotThePanelScale() {
        const auto restore = qScopeGuard([] {
            get_dialog_scale().set_percent(100);
            get_ui_scale().set_percent(100);
        });
        upload::Target blank;
        blank.id = "new";
        get_dialog_scale().set_percent(100);
        get_ui_scale().set_percent(100);
        UploadTargetDialog normal(blank, {}, true);
        normal.show();
        QCOMPARE(normal.width(), 600);
        const int edit_height = normal.name_edit->height();
        const int button_width = normal.save_button->width();
        get_ui_scale().set_percent(200);
        UploadTargetDialog panels(blank, {}, true);
        panels.show();
        QCOMPARE(panels.width(), normal.width());
        QCOMPARE(panels.name_edit->height(), edit_height);
        get_ui_scale().set_percent(100);
        get_dialog_scale().set_percent(150);
        UploadTargetDialog large(blank, {}, true);
        large.show();
        // The offscreen screen is narrower than 900 plus its margins, so the width is the screen's.
        QVERIFY(large.width() > normal.width());
        QVERIFY(large.name_edit->height() > edit_height);
        QVERIFY(large.save_button->width() > button_width);
        QVERIFY(large.height() <= large.screen()->availableGeometry().height());
    }

    // The dialogs in the settings window's look grow with the window scale and ignore the panel scale.
    void settingsStyleDialogsFollowTheWindowScale() {
        const auto restore = qScopeGuard([] {
            get_dialog_scale().set_percent(100);
            get_ui_scale().set_percent(100);
        });
        QMap<QString, QIcon> icons;
        for (const auto &key : toolbar_layout::default_order()) icons[key] = QIcon();
        const std::vector<std::pair<const char *, std::function<std::unique_ptr<QDialog>()>>> dialogs = {
            {"colour formats", [] { return std::make_unique<ColorFormatDialog>(color_formats::default_formats()); }},
            {"special paste", [] { return std::make_unique<SpecialPasteDialog>(special_paste::default_entries()); }},
            {"model import", [] { return std::make_unique<PpocrImportForm>(); }},
            {"toolbar layout", [icons] { return std::make_unique<ToolbarLayoutDialog>(toolbar_layout::default_layout(), icons); }},
            {"message", [] { return std::make_unique<StandardDialog>(nullptr, "Title", "Message"); }},
        };
        // A dialog that does not size itself takes its size hint when shown.
        const auto width_of = [](const std::unique_ptr<QDialog> &dialog) { return dialog->testAttribute(Qt::WA_Resized) ? dialog->width() : dialog->sizeHint().width(); };
        for (const auto &[name, make] : dialogs) {
            get_dialog_scale().set_percent(100);
            get_ui_scale().set_percent(100);
            const int width = width_of(make());
            get_ui_scale().set_percent(150);
            QVERIFY2(width_of(make()) == width, name);
            get_ui_scale().set_percent(100);
            get_dialog_scale().set_percent(150);
            QVERIFY2(width_of(make()) >= width * 1.49, name);
        }
    }

    // Needs real fonts: the dialogs in the settings window's look show every text whole in every language.
    void settingsStyleDialogsFitEveryLanguage() {
        if (QGuiApplication::platformName() != "windows") QSKIP("needs desktop fonts");
        const auto language = I18nManager::get_current_language();
        const auto restore = qScopeGuard([language] {
            get_dialog_scale().set_percent(100);
            I18nManager::load_language(language);
        });
        QMap<QString, QIcon> icons;
        for (const auto &key : toolbar_layout::default_order()) icons[key] = QIcon();
        QStringList problems;
        for (const QString lang : {"zh", "zh_TW", "en", "ja", "ko"}) {
            QVERIFY(I18nManager::load_language(lang));
            for (const int percent : {100, 150}) {
                get_dialog_scale().set_percent(percent);
                std::vector<std::unique_ptr<QDialog>> dialogs;
                dialogs.push_back(std::make_unique<ColorFormatDialog>(color_formats::default_formats()));
                dialogs.push_back(std::make_unique<SpecialPasteDialog>(special_paste::default_entries()));
                dialogs.push_back(std::make_unique<PpocrImportForm>());
                dialogs.push_back(std::make_unique<ToolbarLayoutDialog>(toolbar_layout::default_layout(), icons));
                auto choice = std::make_unique<StandardDialog>(nullptr, settings_tr("Existing Clipboard Data"), settings_tr("Clipboard module not available"));
                choice->button_box->addButton(settings_tr("Use That Data"), QDialogButtonBox::AcceptRole);
                choice->button_box->addButton(settings_tr("Replace"), QDialogButtonBox::DestructiveRole);
                choice->button_box->addButton(settings_tr("Cancel"), QDialogButtonBox::RejectRole);
                dialogs.push_back(std::move(choice));
                for (auto &dialog : dialogs) {
                    dialog->setAttribute(Qt::WA_DontShowOnScreen);
                    dialog->show();
                    QApplication::processEvents();
                    const auto where = QString("%1 %2%% %3").arg(lang).arg(percent).arg(dialog->windowTitle());
                    for (auto *button : dialog->findChildren<QPushButton *>())
                        if (button->isVisible() && button->width() < button->sizeHint().width()) problems << where + ": " + button->text();
                    for (auto *combo : dialog->findChildren<QComboBox *>())
                        if (combo->isVisible() && combo->width() < combo->minimumSizeHint().width()) problems << where + ": " + combo->currentText();
                    for (auto *label : dialog->findChildren<QLabel *>()) {
                        if (!label->isVisible() || label->wordWrap() || label->text().isEmpty()) continue;
                        if (label->width() < label->sizeHint().width()) problems << where + ": " + label->text();
                    }
                    if (dialog->width() > dialog->screen()->availableGeometry().width()) problems << where + ": wider than the screen";
                }
            }
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    // A message names the dialog in a large title; its buttons share the footer, the accepting one drawn as primary.
    void messageDialogButtonsShareTheFooter() {
        StandardDialog dialog(nullptr, "Reset", "Restore the defaults?");
        auto *no = dialog.button_box->addButton("No, keep the current settings", QDialogButtonBox::NoRole);
        auto *yes = dialog.button_box->addButton("Yes", QDialogButtonBox::YesRole);
        dialog.show();
        QApplication::processEvents();
        QCOMPARE(dialog.title_label->text(), QString("Reset"));
        QVERIFY(dialog.title_label->font().pixelSize() > dialog.label->font().pixelSize() || dialog.title_label->height() > dialog.label->height());
        // Yes comes first whatever the order added, as in the platform's message boxes.
        QCOMPARE(dialog.button_box->buttons(), (QList<QAbstractButton *>{yes, no}));
        QVERIFY(qobject_cast<PrimaryPushButton *>(yes));
        QVERIFY(!qobject_cast<PrimaryPushButton *>(no));
        QVERIFY(yes->isDefault());
        QVERIFY(!no->isDefault());
        QCOMPARE(yes->width(), no->width());
        QVERIFY(no->width() >= no->sizeHint().width());
        QVERIFY(yes->x() < no->x());
        dialog.setWindowTitle("Renamed");
        QCOMPARE(dialog.title_label->text(), QString("Renamed"));
        QCOMPARE(dialog.button_box->buttonRole(no), QDialogButtonBox::NoRole);
    }

    // Targets are added, edited and deleted from the page; the default and the list follow at once.
    void uploadGroupAddsEditsAndDeletesTargets() {
        const auto reset = qScopeGuard([this] {
            config_->set_upload_targets("");
            config_->set_upload_default_target("");
            config_->set_upload_link_format("url");
        });
        config_->set_upload_targets("");
        config_->set_upload_default_target("");
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        dialog.on_nav_changed(1, "capture");
        const auto combo = [&dialog] { return dialog.attr<QComboBox>("upload_default_combo"); };
        const auto stored = [this] { return upload::parse_targets(config_->get_upload_targets()); };
        QVERIFY(combo());
        QCOMPARE(combo()->count(), 1);
        QCOMPARE(combo()->currentData().toString(), QString());
        const auto add = [&](const QString &name, const QString &bucket, int items) {
            drive_upload_dialog([&](UploadTargetDialog *editor) {
                fill_upload_target(editor, name, bucket);
                editor->save_button->click();
            });
            dialog.findChild<QPushButton *>("upload_add_button")->click();
            QTRY_COMPARE(combo()->count(), items);
        };
        add("Main", "bucket-one", 2);
        QCOMPARE(stored().size(), size_t(1));
        const QString first = stored()[0].id;
        QVERIFY(!first.isEmpty());
        // The first target added is the default.
        QCOMPARE(config_->get_upload_default_target(), first);
        QCOMPARE(combo()->currentData().toString(), first);
        QCOMPARE(combo()->itemText(1), QString("Main"));
        auto *card = dialog.findChild<SettingCard *>("upload_target_" + first);
        QVERIFY(card);
        QCOMPARE(card->titleLabel->text(), QString("Main"));
        // The bucket and the host, which may break after its dots.
        QCOMPARE(card->contentLabel->text().remove(QChar(0x200B)), QString::fromUtf8("bucket-one · s3.us-east-1.amazonaws.com"));
        add("Second", "bucket-two", 3);
        QCOMPARE(stored().size(), size_t(2));
        const QString second = stored()[1].id;
        QCOMPARE(stored()[1].bucket, QString("bucket-two"));
        QCOMPARE(config_->get_upload_default_target(), first);
        QCOMPARE(combo()->currentData().toString(), first);
        // Editing shows what was saved; a name another target has stays refused.
        drive_upload_dialog([&](UploadTargetDialog *editor) {
            QCOMPARE(editor->name_edit->text(), QString("Second"));
            QCOMPARE(editor->bucket_edit->text(), QString("bucket-two"));
            QCOMPARE(editor->secret_edit->text(), QString("secret"));
            editor->name_edit->setText("MAIN");
            editor->save_button->click();
            QVERIFY(editor->isVisible());
            QVERIFY(!editor->errors.value("name")->isHidden());
        });
        dialog.findChild<QPushButton *>("upload_edit_" + second)->click();
        QCOMPARE(stored()[1].name, QString("Second"));
        drive_upload_dialog([&](UploadTargetDialog *editor) {
            editor->name_edit->setText("Renamed");
            editor->prefix_edit->setText("p/{dd}");
            editor->save_button->click();
        });
        dialog.findChild<QPushButton *>("upload_edit_" + second)->click();
        QTRY_COMPARE(combo()->itemText(2), QString("Renamed"));
        QCOMPARE(stored()[1].path_prefix, QString("p/{dd}"));
        QCOMPARE(stored()[1].id, second);
        QCOMPARE(dialog.findChild<SettingCard *>("upload_target_" + second)->titleLabel->text(), QString("Renamed"));
        // Cancelling changes nothing.
        drive_upload_dialog([](UploadTargetDialog *editor) {
            editor->name_edit->setText("Never saved");
            editor->cancel_button->click();
        });
        dialog.findChild<QPushButton *>("upload_edit_" + second)->click();
        QCOMPARE(stored()[1].name, QString("Renamed"));
        // A delete is asked about first.
        answer_next(false);
        dialog.findChild<QPushButton *>("upload_delete_" + second)->click();
        QCOMPARE(stored().size(), size_t(2));
        answer_next(true);
        dialog.findChild<QPushButton *>("upload_delete_" + second)->click();
        QTRY_COMPARE(combo()->count(), 2);
        QCOMPARE(stored().size(), size_t(1));
        QCOMPARE(config_->get_upload_default_target(), first);
        // Deleting the default clears it.
        answer_next(true);
        dialog.findChild<QPushButton *>("upload_delete_" + first)->click();
        QTRY_COMPARE(combo()->count(), 1);
        QVERIFY(stored().empty());
        QCOMPARE(config_->get_upload_default_target(), QString());
        QCOMPARE(combo()->currentData().toString(), QString());
        QVERIFY(!dialog.findChild<SettingCard *>("upload_target_" + first));
    }

    // The default and the link format are chosen in their combos and saved like any setting; the page's reset keeps
    // the targets, whose keys would be lost.
    void uploadGroupSavesTheDefaultAndTheLinkFormat() {
        const auto reset = qScopeGuard([this] {
            config_->set_upload_targets("");
            config_->set_upload_default_target("");
            config_->set_upload_link_format("url");
        });
        upload::Target a, b;
        a.id = "a";
        a.name = "A";
        a.endpoint = "http://localhost:9000";
        b = a;
        b.id = "b";
        b.name = "B";
        config_->set_upload_targets(upload::serialize_targets({a, b}));
        config_->set_upload_default_target("a");
        config_->set_upload_link_format("url");
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        dialog.on_nav_changed(1, "capture");
        auto *targets = dialog.attr<QComboBox>("upload_default_combo");
        QCOMPARE(targets->count(), 3);
        QCOMPARE(targets->currentData().toString(), QString("a"));
        targets->setCurrentIndex(targets->findData("b"));
        auto *format = dialog.attr<QComboBox>("upload_format_combo");
        QCOMPARE(format->count(), 3);
        QCOMPARE(format->currentData().toString(), QString("url"));
        format->setCurrentIndex(format->findData("markdown"));
        dialog.apply_settings();
        QCOMPARE(config_->get_upload_default_target(), QString("b"));
        QCOMPARE(config_->get_upload_link_format(), QString("markdown"));
        reset_page(dialog);
        dialog.apply_settings();
        QCOMPARE(config_->get_upload_link_format(), QString("url"));
        QCOMPARE(upload::parse_targets(config_->get_upload_targets()).size(), size_t(2));
        QCOMPARE(config_->get_upload_default_target(), QString("b"));
    }

    // What changed in the settings from outside, or came from an imported file, shows in the list.
    void uploadGroupFollowsTheSettingsReadFromElsewhere() {
        const auto reset = qScopeGuard([this] {
            config_->set_upload_targets("");
            config_->set_upload_default_target("");
        });
        config_->set_upload_targets("");
        config_->set_upload_default_target("");
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        dialog.on_nav_changed(1, "capture");
        upload::Target a, b;
        a.id = "a";
        a.name = "A";
        a.endpoint = "http://localhost:9000";
        b = a;
        b.id = "b";
        b.name = "B";
        config_->set_upload_targets(upload::serialize_targets({a, b}));
        config_->set_upload_default_target("b");
        dialog.refresh_settings();
        auto *combo = dialog.attr<QComboBox>("upload_default_combo");
        QCOMPARE(combo->count(), 3);
        QCOMPARE(combo->currentData().toString(), QString("b"));
        QVERIFY(dialog.findChild<SettingCard *>("upload_target_a"));
        QVERIFY(dialog.findChild<SettingCard *>("upload_target_b"));
        // An import writes the list and the default, and the window shows them.
        upload::Target c = a;
        c.id = "c";
        c.name = "C";
        dialog.load_imported_settings({{"upload/targets", upload::serialize_targets({c})}, {"upload/default_target", "c"}});
        QCOMPARE(config_->get_upload_default_target(), QString("c"));
        QCOMPARE(upload::parse_targets(config_->get_upload_targets()).size(), size_t(1));
        combo = dialog.attr<QComboBox>("upload_default_combo");
        QCOMPARE(combo->count(), 2);
        QCOMPARE(combo->currentData().toString(), QString("c"));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!dialog.findChild<SettingCard *>("upload_target_a"));
        QVERIFY(dialog.findChild<SettingCard *>("upload_target_c"));
    }

    // The upload shortcut is a row of the screenshot window's keys, empty by default, saved like the others.
    void uploadShortcutHasARowAndSavesWithTheOtherKeys() {
        const auto reset = qScopeGuard([this] { config_->set_inapp_shortcut("inapp_upload", ""); });
        config_->set_inapp_shortcut("inapp_upload", "");
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        dialog.on_nav_changed(0, "shortcuts");
        auto *edit = dialog.inapp_edit("inapp_upload");
        QVERIFY(edit);
        QCOMPARE(edit->text(), QString());
        QCOMPARE(dialog.inapp_groups.value("inapp_upload"), QString("screenshot"));
        edit->setText("ctrl+u");
        dialog.apply_settings();
        QCOMPARE(config_->get_inapp_shortcut("inapp_upload"), QString("ctrl+u"));
    }

    // The upload group and its dialog in every language and scale: needs real fonts.
    void uploadGroupAndDialogFitEveryLanguage() {
        if (QGuiApplication::platformName() != "windows") QSKIP("needs -platform windows");
        auto &scale = get_dialog_scale();
        const int percent_before = scale.percent();
        const QString language = I18nManager::get_current_language();
        const auto restore = qScopeGuard([this, &scale, percent_before, language] {
            scale.set_percent(percent_before);
            I18nManager::load_language(language);
            config_->set_upload_targets("");
            config_->set_upload_default_target("");
        });
        upload::Target target;
        target.id = "a";
        target.name = "Cloudflare R2 team screenshots";
        target.endpoint = "https://0123456789abcdef.r2.cloudflarestorage.com";
        const auto targets = upload::serialize_targets({target});
        QStringList problems;
        for (const int percent : {100, 150, 200}) {
            scale.set_percent(percent);
            for (const char *code : {"zh", "zh_TW", "en", "ja", "ko"}) {
                QVERIFY(I18nManager::load_language(code));
                const QString where = QString("%1 at %2%").arg(code).arg(percent);
                {
                    config_->set_upload_targets("");
                    config_->set_upload_default_target("");
                    SettingsDialog dialog(config_, config_->get_hotkey());
                    dialog.fit_to_available({4000, dialog.height()});
                    dialog.build_all_pages();
                    dialog.resize(0, dialog.height());
                    dialog.show();
                    dialog.on_nav_changed(1, "capture");
                    auto *default_combo = dialog.attr<QComboBox>("upload_default_combo");
                    QVERIFY(default_combo);
                    QCOMPARE(default_combo->count(), 1);
                    if (qstrcmp(code, "zh") == 0) QCOMPARE(default_combo->currentText(), QString::fromUtf8("未配置"));
                    QApplication::processEvents();
                    if (default_combo->width() < default_combo->sizeHint().width())
                        problems << where + ": combo " + default_combo->currentText() + " is cut";
                    config_->set_upload_targets(targets);
                    config_->set_upload_default_target(target.id);
                    dialog.refresh_settings();
                    QApplication::processEvents();
                    auto *group = dialog.attr<QWidget>("upload_group");
                    QVERIFY(group);
                    for (auto *widget : group->findChildren<QWidget *>()) {
                        if (!widget->isVisible()) continue;
                        if (auto *button = qobject_cast<QPushButton *>(widget); button && button->width() < button->sizeHint().width())
                            problems << where + ": button " + button->text() + " is cut";
                        if (!widget->layout()) continue;
                        const int needs = widget->layout()->totalMinimumSize().width();
                        if (needs > widget->width()) problems << QString("%1: %2 needs %3, has %4").arg(where, widget->metaObject()->className()).arg(needs).arg(widget->width());
                    }
                    auto *scroll = qobject_cast<QScrollArea *>(dialog.content_stack->currentWidget());
                    if (scroll->horizontalScrollBar()->maximum() > 0) problems << where + ": the page scrolls sideways";
                }
                UploadTargetDialog editor(target, {}, false);
                editor.show();
                QApplication::processEvents();
                for (auto *button : editor.findChildren<QPushButton *>())
                    if (button->width() < button->sizeHint().width()) problems << where + ": dialog button " + button->text() + " is cut";
                for (auto *label : editor.findChildren<QLabel *>()) {
                    if (!label->isVisible() || label->wordWrap()) continue;
                    if (label->width() < label->sizeHint().width() && label->objectName() != "upload_preview") problems << where + ": label " + label->text() + " is cut";
                }
                for (auto *box : editor.findChildren<QComboBox *>())
                    if (box->width() < box->sizeHint().width()) problems << where + ": combo " + box->currentText() + " is cut";
                editor.validate_fields();
                if (editor.scroll->horizontalScrollBar()->maximum() > 0) problems << where + ": the dialog scrolls sideways";
                if (editor.scroll->widget()->width() > editor.scroll->viewport()->width()) problems << where + ": the form is wider than its view";
                if (editor.height() > editor.screen()->availableGeometry().height()) problems << where + ": the dialog is taller than the screen";
            }
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    // The second navigation level: the pages' groups, listed under their items.
    void outlineListsTheGroupsOfThePageInOrder() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.resize(1050, 750);
        dialog.show();
        dialog.on_nav_changed(1, "capture");
        auto *nav = dialog.findChild<NavigationInterface *>("SettingsNavigation");
        auto *scroll = qobject_cast<QScrollArea *>(dialog.content_stack->currentWidget());
        QVERIFY(nav && scroll);
        QTRY_VERIFY(nav->subItemCount("capture") > 1);
        auto groups = page_groups(scroll);
        QStringList titles;
        for (auto *group : groups) titles << group_title_of(group);
        QCOMPARE(nav->subItemTitles("capture"), titles);
        QCOMPARE(titles.size(), 9);

        // A hidden group is not listed, and comes back when shown.
        groups[1]->hide();
        QStringList without = titles;
        without.removeAt(1);
        QTRY_COMPARE(nav->subItemTitles("capture"), without);
        groups[1]->show();
        QTRY_COMPARE(nav->subItemTitles("capture"), titles);
    }

    void outlineHighlightFollowsTheScroll() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.resize(1050, 600);
        dialog.show();
        dialog.on_nav_changed(1, "capture");
        auto *nav = dialog.findChild<NavigationInterface *>("SettingsNavigation");
        auto *scroll = qobject_cast<QScrollArea *>(dialog.content_stack->currentWidget());
        QTRY_VERIFY(nav->subItemCount("capture") > 1);
        auto *bar = scroll->verticalScrollBar();
        QVERIFY(bar->maximum() > 0);
        QCOMPARE(nav->currentSubItem(), 0);
        const auto groups = page_groups(scroll);
        const int margin = dialog_scaled(PageOutline::TOP_MARGIN);
        int reached = 0;
        for (int k = 0; k < static_cast<int>(groups.size()); ++k) {
            const int value = group_top_of(scroll, groups[k]) - margin;
            if (value >= bar->maximum()) break;
            bar->setValue(std::max(0, value));
            QCOMPARE(nav->currentSubItem(), k);
            // Still the same group a little further down, until the next one reaches the top.
            bar->setValue(std::max(0, value) + dialog_scaled(4));
            QCOMPARE(nav->currentSubItem(), k);
            ++reached;
        }
        QVERIFY(reached >= 3);
        // A group whose title has nearly reached the top already counts, not the sliver of the one above.
        const int second = group_top_of(scroll, groups[1]) - margin;
        bar->setValue(second - dialog_scaled(PageOutline::REACH) + dialog_scaled(4));
        QCOMPARE(nav->currentSubItem(), 1);
        bar->setValue(second - dialog_scaled(PageOutline::REACH) - dialog_scaled(4));
        QCOMPARE(nav->currentSubItem(), 0);
        // At the end the last group is current, also when it is too short to reach the top.
        bar->setValue(bar->maximum());
        QCOMPARE(nav->currentSubItem(), static_cast<int>(groups.size()) - 1);
        bar->setValue(0);
        QCOMPARE(nav->currentSubItem(), 0);
        QCOMPARE(nav->currentSubRoute(), QString("capture"));
    }

    void clickingASubItemScrollsToItWithoutTheHighlightJumping() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.resize(1050, 600);
        dialog.show();
        dialog.on_nav_changed(1, "capture");
        auto *nav = dialog.findChild<NavigationInterface *>("SettingsNavigation");
        auto *scroll = qobject_cast<QScrollArea *>(dialog.content_stack->currentWidget());
        QTRY_VERIFY(nav->subItemCount("capture") > 3);
        QTRY_VERIFY(!nav->animating());
        auto *outline = dialog.page_outline();
        auto *bar = scroll->verticalScrollBar();
        const auto groups = page_groups(scroll);
        const int margin = dialog_scaled(PageOutline::TOP_MARGIN);

        const int target = 3;
        const int expected = std::clamp(group_top_of(scroll, groups[target]) - margin, 0, bar->maximum());
        QVERIFY(expected > 0);
        nav->subItem("capture", target)->click();
        QVERIFY(outline->animating());
        int last = bar->value();
        bool passed_other_group = false;
        while (outline->animating()) {
            // The groups the scroll passes do not take the highlight.
            QCOMPARE(nav->currentSubItem(), target);
            QVERIFY(bar->value() >= last);
            last = bar->value();
            if (group_top_of(scroll, groups[1]) - bar->value() < margin) passed_other_group = true;
            QTest::qWait(10);
        }
        QVERIFY(passed_other_group);
        QCOMPARE(bar->value(), expected);
        QCOMPARE(nav->currentSubItem(), target);

        // A group that cannot reach the top ends at the bottom and stays the chosen one.
        const int count = nav->subItemCount("capture");
        nav->subItem("capture", count - 2)->click();
        QTRY_VERIFY(!outline->animating());
        QCOMPARE(bar->value(), std::clamp(group_top_of(scroll, groups[count - 2]) - margin, 0, bar->maximum()));
        QCOMPARE(nav->currentSubItem(), count - 2);
        // Scrolling by other means takes over again.
        bar->setValue(0);
        QCOMPARE(nav->currentSubItem(), 0);
        bar->setValue(bar->maximum());
        QCOMPARE(nav->currentSubItem(), count - 1);
        bar->setValue(0);
        QCOMPARE(nav->currentSubItem(), 0);
    }

    void outlineHighlightFollowsASearchJump() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.resize(1050, 750);
        dialog.show();
        dialog.on_nav_changed(1, "capture");
        auto *nav = dialog.findChild<NavigationInterface *>("SettingsNavigation");
        auto *scroll = qobject_cast<QScrollArea *>(dialog.content_stack->currentWidget());
        QTRY_VERIFY(nav->subItemCount("capture") > 4);
        const auto groups = page_groups(scroll);
        const auto entries = dialog.search_entries();
        // A row in the group just below the top, which the scroll alone would not mark, and one far below.
        for (const int index : {1, 4}) {
            dialog.on_nav_changed(1, "capture");
            scroll->verticalScrollBar()->setValue(0);
            QCOMPARE(nav->currentSubItem(), 0);
            const SettingsSearchEntry *found = nullptr;
            for (const auto &entry : entries)
                if (entry.kind == SettingsSearchEntry::Kind::Item && entry.route_key == "capture" && entry.target && groups[index]->isAncestorOf(entry.target)) {
                    found = &entry;
                    break;
                }
            QVERIFY(found);
            dialog.show_search_result(*found);
            QTRY_COMPARE(nav->currentSubItem(), index);
            QTest::qWait(30);
            QCOMPARE(nav->currentSubItem(), index);
            // Scrolling by hand lets go of it.
            scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
            QCOMPARE(nav->currentSubItem(), nav->subItemCount("capture") - 1);
            scroll->verticalScrollBar()->setValue(0);
        }
    }

    // At most one page is unfolded: opening a page or unfolding one with its arrow folds the one before.
    void onlyOnePageIsUnfoldedAtATime() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.resize(1050, 750);
        dialog.show();
        auto *nav = dialog.findChild<NavigationInterface *>("SettingsNavigation");
        QVERIFY(nav);
        const auto unfolded = [nav] {
            QStringList open;
            for (const auto *route : {"shortcuts", "mouse", "capture", "pin", "screen_annotation", "clipboard", "translation", "appearance", "log", "other"})
                if (nav->isExpanded(route)) open << route;
            return open;
        };
        QTRY_COMPARE(unfolded(), QStringList{"shortcuts"});
        dialog.on_nav_changed(1, "capture");
        QCOMPARE(unfolded(), QStringList{"capture"});
        QTRY_VERIFY(nav->subItemCount("capture") > 1);
        QTRY_VERIFY(!nav->animating());
        QVERIFY(nav->subItem("shortcuts", 0) == nullptr || !nav->subItem("shortcuts", 0)->isVisible());

        // An arrow unfolds a page that was never opened, building it; the window stays on its page.
        const int before = dialog.content_stack->currentIndex();
        emit nav->arrowClicked("clipboard");
        QCOMPARE(unfolded(), QStringList{"clipboard"});
        QCOMPARE(dialog.content_stack->currentIndex(), before);
        QVERIFY(nav->subItemCount("clipboard") > 1);
        QTRY_VERIFY(!nav->animating());
        QVERIFY(nav->subItem("clipboard", 0)->isVisible());
        QVERIFY(!nav->subItem("capture", 0)->isVisible());

        // Opening a page folds one unfolded by its arrow too.
        dialog.on_nav_changed(11, "pin");
        QCOMPARE(unfolded(), QStringList{"pin"});
        // Folded and unfolded again by clicks on its row, the page shown is still folded when left.
        auto *item = nav->widget("pin");
        const QPoint on_text(dialog_scaled(60), item->height() / 2);
        QTest::mouseClick(item, Qt::LeftButton, Qt::NoModifier, on_text);
        QVERIFY(unfolded().isEmpty());
        QTest::mouseClick(item, Qt::LeftButton, Qt::NoModifier, on_text);
        QCOMPARE(unfolded(), QStringList{"pin"});
        dialog.on_nav_changed(5, "log");
        QCOMPARE(unfolded(), QStringList{"log"});

        // A page folded by the user while on it stays folded until it is opened again.
        emit nav->arrowClicked("log");
        QVERIFY(unfolded().isEmpty());
        dialog.page_outline()->refresh();
        QTest::qWait(30);
        QVERIFY(unfolded().isEmpty());
        dialog.on_nav_changed(1, "capture");
        QCOMPARE(unfolded(), QStringList{"capture"});
        dialog.on_nav_changed(5, "log");
        QCOMPARE(unfolded(), QStringList{"log"});
        QTRY_VERIFY(!nav->animating());
        QVERIFY(!nav->subItem("capture", 0)->isVisible());
    }

    void theArrowOnlyUnfoldsAndTheRestOfTheItemOpensThePage() {
        auto &scale = get_dialog_scale();
        const int before = scale.percent();
        const auto restore = qScopeGuard([&] { scale.set_percent(before); });
        // At 150% the arrow's press area is wider, so a point 40 px from the edge is on it only there.
        for (const int percent : {100, 150}) {
            scale.set_percent(percent);
            SettingsDialog dialog(config_, config_->get_hotkey());
            dialog.fit_to_available({2560, 1392});
            dialog.show();
            auto *nav = dialog.findChild<NavigationInterface *>("SettingsNavigation");
            QTRY_VERIFY(nav->isExpanded("shortcuts"));
            auto *item = nav->widget("capture");
            QVERIFY(item);
            const QString where = QString("at %1%%").arg(percent);
            const int width = item->width();

            QTest::mouseClick(item, Qt::LeftButton, Qt::NoModifier, QPoint(width - dialog_scaled(12), item->height() / 2));
            QVERIFY2(nav->isExpanded("capture"), qPrintable(where));
            QVERIFY2(dialog.content_stack->currentIndex() == 0, qPrintable(where));
            QVERIFY2(!nav->isExpanded("shortcuts"), qPrintable(where));
            QTest::mouseClick(item, Qt::LeftButton, Qt::NoModifier, QPoint(width - dialog_scaled(12), item->height() / 2));
            QVERIFY2(!nav->isExpanded("capture"), qPrintable(where));
            QVERIFY2(dialog.content_stack->currentIndex() == 0, qPrintable(where));

            // Left of the arrow the click opens the page, which unfolds.
            const bool on_arrow = percent == 150;
            QTest::mouseClick(item, Qt::LeftButton, Qt::NoModifier, QPoint(width - 40, item->height() / 2));
            if (on_arrow) {
                QVERIFY2(nav->isExpanded("capture") && dialog.content_stack->currentIndex() == 0, qPrintable(where));
            } else {
                QVERIFY2(dialog.content_stack->currentIndex() == 1, qPrintable(where));
                QTRY_VERIFY2(nav->isExpanded("capture"), qPrintable(where));
            }
            QTest::mouseClick(item, Qt::LeftButton, Qt::NoModifier, QPoint(dialog_scaled(60), item->height() / 2));
            QVERIFY2(dialog.content_stack->currentIndex() == 1, qPrintable(where));
        }
    }

    // Clicked again, the row of the page shown folds it and then unfolds it, like its arrow; the
    // page stays. Another page's row still opens that page.
    void clickingTheOpenPagesRowFoldsAndUnfoldsIt() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.resize(1050, 750);
        dialog.show();
        auto *nav = dialog.findChild<NavigationInterface *>("SettingsNavigation");
        QVERIFY(nav);
        QTRY_VERIFY(nav->isExpanded("shortcuts"));
        auto *item = nav->widget("capture");
        const QPoint on_text(dialog_scaled(60), item->height() / 2);
        QTest::mouseClick(item, Qt::LeftButton, Qt::NoModifier, on_text);
        QCOMPARE(dialog.content_stack->currentIndex(), 1);
        QTRY_VERIFY(nav->subItemCount("capture") > 1);
        QVERIFY(nav->isExpanded("capture"));
        QTRY_VERIFY(!nav->animating());

        // The current page's mark: a group's row while unfolded, the page's own row while folded.
        const auto row_marked = [item] {
            const QColor bar = item->grab().toImage().pixelColor(1, item->height() / 2);
            const QColor accent = css_color(ACCENT);
            return std::abs(bar.red() - accent.red()) + std::abs(bar.green() - accent.green()) + std::abs(bar.blue() - accent.blue()) < 8;
        };
        QCOMPARE(nav->currentSubRoute(), QString("capture"));
        QVERIFY(!row_marked());

        QTest::mouseClick(item, Qt::LeftButton, Qt::NoModifier, on_text);
        QVERIFY(!nav->isExpanded("capture"));
        QCOMPARE(dialog.content_stack->currentIndex(), 1);
        QVERIFY(item->isChecked());
        QTRY_VERIFY(!nav->animating());
        QVERIFY(!nav->subItem("capture", 0)->isVisible());
        QVERIFY(row_marked());
        QTest::mouseClick(item, Qt::LeftButton, Qt::NoModifier, on_text);
        QVERIFY(nav->isExpanded("capture"));
        QCOMPARE(dialog.content_stack->currentIndex(), 1);
        QTRY_VERIFY(!nav->animating());
        QVERIFY(nav->subItem("capture", 0)->isVisible());
        QVERIFY(!row_marked());

        QTest::mouseClick(nav->widget("pin"), Qt::LeftButton, Qt::NoModifier, on_text);
        QCOMPARE(dialog.content_stack->currentIndex(), 11);
        QTRY_VERIFY(nav->isExpanded("pin"));
    }

    void foldingMovesTheHeightOfTheListAndEndsAtItsNaturalSize() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.resize(1050, 900);
        dialog.show();
        dialog.on_nav_changed(1, "capture");
        auto *nav = dialog.findChild<NavigationInterface *>("SettingsNavigation");
        QTRY_VERIFY(nav->subItemCount("capture") > 1);
        QTRY_VERIFY(!nav->animating());
        QWidget *list = nav->subItem("capture", 0)->parentWidget()->parentWidget();
        QVERIFY(list->isVisible());
        const int full = list->height();
        QVERIFY(full >= nav->subItemCount("capture") * dialog_scaled(36));
        QVERIFY(list->maximumHeight() >= QWIDGETSIZE_MAX);
        // The last row ends inside it.
        auto *last = nav->subItem("capture", nav->subItemCount("capture") - 1);
        QVERIFY(last->geometry().bottom() < list->height());

        emit nav->arrowClicked("capture");
        QVERIFY(nav->animating());
        bool between = false;
        for (int i = 0; i < 60 && nav->animating(); ++i) {
            between = between || (list->height() > 0 && list->height() < full);
            QTest::qWait(5);
        }
        QVERIFY(between);
        QTRY_VERIFY(!nav->animating());
        QVERIFY(!list->isVisible());

        emit nav->arrowClicked("capture");
        bool grows = false;
        for (int i = 0; i < 60 && nav->animating(); ++i) {
            grows = grows || (list->height() > 0 && list->height() < full);
            QTest::qWait(5);
        }
        QVERIFY(grows);
        QTRY_VERIFY(!nav->animating());
        QCOMPARE(list->height(), full);
        QVERIFY(list->maximumHeight() >= QWIDGETSIZE_MAX);
        // The highlight kept following the scroll through all of it.
        QCOMPARE(nav->currentSubRoute(), QString("capture"));
    }

    // Scrolled to the bottom, a list folding above the rows pulls the scroll position up with the
    // content: every paint shows the rows at the scroll position that goes with them, so none of
    // them moves back and forth.
    void foldingInAScrolledNavigationMovesEveryRowOneWay() {
        struct PaintSpy : QObject {
            std::function<void()> on_paint;
            bool eventFilter(QObject *, QEvent *event) override {
                if (event->type() == QEvent::Paint && on_paint) on_paint();
                return false;
            }
        };
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.resize(1050, 600);
        dialog.show();
        auto *nav = dialog.findChild<NavigationInterface *>("SettingsNavigation");
        QScrollArea *area = nav->scrollArea();
        // The log page's list is built first, so only folding moves the rows below.
        dialog.on_nav_changed(5, "log");
        QTRY_VERIFY(nav->subItemCount("log") > 0);
        dialog.on_nav_changed(1, "capture");
        QTRY_VERIFY(nav->subItemCount("capture") > 1);
        QTRY_VERIFY(!nav->animating());
        auto *bar = area->verticalScrollBar();
        QVERIFY(bar->maximum() > 0);
        bar->setValue(bar->maximum());

        const QStringList routes{"shortcuts", "mouse", "capture", "pin", "screen_annotation", "clipboard", "translation", "appearance", "log", "other"};
        QHash<QString, QList<int>> tops;
        PaintSpy spy;
        spy.on_paint = [&] {
            for (const auto &route : routes) {
                auto &seen = tops[route];
                const int y = nav->widget(route)->mapTo(area->viewport(), QPoint()).y();
                if (seen.isEmpty() || seen.last() != y) seen << y;
            }
        };
        area->viewport()->installEventFilter(&spy);
        for (const auto &route : routes) nav->widget(route)->installEventFilter(&spy);
        dialog.on_nav_changed(5, "log");
        QTRY_VERIFY(!nav->animating());
        QTest::qWait(50);
        spy.on_paint = nullptr;

        QVERIFY(tops["pin"].size() > 3);
        for (const auto &route : routes) {
            const auto &seen = tops[route];
            QStringList trail;
            for (const int y : seen) trail << QString::number(y);
            for (int i = 2; i < seen.size(); ++i)
                QVERIFY2((seen[i] > seen[i - 1]) == (seen[1] > seen[0]), qPrintable(route + ": " + trail.join(' ')));
        }
    }

    void clickingASubItemOfAnotherPageOpensItAndScrollsToTheGroup() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.resize(1050, 600);
        dialog.show();
        auto *nav = dialog.findChild<NavigationInterface *>("SettingsNavigation");
        QTRY_VERIFY(nav->subItemCount("shortcuts") > 0);
        emit nav->arrowClicked("pin");
        QVERIFY(nav->subItemCount("pin") > 3);
        // Only the page shown has a highlight.
        QCOMPARE(nav->currentSubRoute(), QString("shortcuts"));
        QTRY_VERIFY(!nav->animating());
        const int index = 3;
        const QString title = nav->subItemTitles("pin")[index];
        nav->subItem("pin", index)->click();
        QCOMPARE(dialog.content_stack->currentIndex(), 11);
        auto *scroll = qobject_cast<QScrollArea *>(dialog.content_stack->currentWidget());
        QVERIFY(scroll);
        QTRY_VERIFY(dialog.page_outline()->animating() || scroll->verticalScrollBar()->value() > 0);
        QCOMPARE(nav->currentSubRoute(), QString("pin"));
        QCOMPARE(nav->currentSubItem(), index);
        QTRY_VERIFY(!dialog.page_outline()->animating());
        const auto groups = page_groups(scroll);
        QCOMPARE(group_title_of(groups[index]), title);
        auto *bar = scroll->verticalScrollBar();
        QCOMPARE(bar->value(), std::clamp(group_top_of(scroll, groups[index]) - dialog_scaled(PageOutline::TOP_MARGIN), 0, bar->maximum()));
        QCOMPARE(nav->currentSubItem(), index);
        // The page left behind is not highlighted and stays folded; the pin page is the one unfolded.
        QVERIFY(!nav->isExpanded("shortcuts"));
        QVERIFY(nav->isExpanded("pin"));
    }

    void theTitleBarCarriesTheLogoAndTheDeveloperMenu() {
        auto &scale = get_dialog_scale();
        const int before = scale.percent();
        const auto restore = qScopeGuard([&] { scale.set_percent(before); });
        for (const int percent : {100, 150}) {
            scale.set_percent(percent);
            SettingsDialog dialog(config_, config_->get_hotkey());
            dialog.show();
            auto *bar = qobject_cast<FluentTitleBar *>(dialog.titleBar());
            QVERIFY(bar);
            const QString where = QString("at %1%%").arg(percent);
            QVERIFY2(bar->iconLabel->isVisible() && bar->titleLabel->isVisible(), qPrintable(where));
            QVERIFY2(!bar->iconLabel->pixmap().isNull(), qPrintable(where));
            QCOMPARE(bar->titleLabel->text(), QString("jietuba"));
            QVERIFY2(bar->iconLabel->width() == 22 * percent / 100, qPrintable(where));
            QVERIFY2(bar->iconLabel->pixmap().deviceIndependentSize().width() == 16 * percent / 100, qPrintable(where));
            // The logo sits at the left end of the caption, left of the caption buttons.
            QVERIFY2(bar->iconLabel->x() < bar->titleLabel->x() && bar->titleLabel->x() < bar->minBtn->x(), qPrintable(where));
            // The side panel no longer has a logo row of its own.
            auto *panel = dialog.findChild<QWidget *>("SettingsLeftPanel");
            QVERIFY(panel);
            for (auto *label : panel->findChildren<QLabel *>()) QVERIFY2(label->text() != "jietuba", qPrintable(where));
        }
    }

    void theDeveloperMenuOpensFromTheNameAsWellAsTheLogo() {
        SettingsDialog dialog(config_, config_->get_hotkey());
        dialog.show();
        auto *bar = qobject_cast<FluentTitleBar *>(dialog.titleBar());
        auto *nav = dialog.findChild<NavigationInterface *>("SettingsNavigation");
        QVERIFY(bar && nav);
        for (QLabel *label : {bar->titleLabel, bar->iconLabel}) {
            QCOMPARE(label->contextMenuPolicy(), Qt::CustomContextMenu);
            dialog.on_nav_changed(1, "capture");
            QTimer::singleShot(0, &dialog, [] {
                auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                if (!menu) return;
                menu->setActiveAction(menu->actions().value(0));
                QTest::keyClick(menu, Qt::Key_Return);
            });
            emit label->customContextMenuRequested(QPoint(2, 2));
            QCOMPARE(dialog.content_stack->currentIndex(), 7);
            // No item stands for the developer page: nothing is current there, and the page left is folded.
            QVERIFY(nav->currentSubRoute().isEmpty());
            QTRY_VERIFY(!nav->isExpanded("capture"));
        }
    }

    void navigationRowsFollowTheWindowScaleButNotThePanelScale() {
        auto &scale = get_dialog_scale();
        const int window_before = scale.percent(), panel_before = get_ui_scale().percent();
        const auto restore = qScopeGuard([&] {
            scale.set_percent(window_before);
            get_ui_scale().set_percent(panel_before);
        });
        for (const auto &[window, panel] : {std::pair{100, 100}, std::pair{150, 100}, std::pair{100, 150}, std::pair{150, 150}}) {
            scale.set_percent(window);
            get_ui_scale().set_percent(panel);
            SettingsDialog dialog(config_, config_->get_hotkey());
            dialog.fit_to_available({2560, 1392});
            dialog.show();
            dialog.on_nav_changed(1, "capture");
            auto *nav = dialog.findChild<NavigationInterface *>("SettingsNavigation");
            QTRY_VERIFY(nav->subItemCount("capture") > 1);
            QTRY_VERIFY(!nav->animating());
            const QString where = QString("window %1%%, panel %2%%").arg(window).arg(panel);
            QVERIFY2(nav->widget("capture")->height() == 36 * window / 100, qPrintable(where));
            QVERIFY2(nav->subItem("capture", 0)->height() == 36 * window / 100, qPrintable(where));
            QVERIFY2(dialog.search_box()->edit()->height() == 32 * window / 100, qPrintable(where));
        }
    }

    // Every item and group title is shown whole in each language, with each page unfolded in turn:
    // long ones wrap, none is cut or pushes the window wider, and an item's text stops before its arrow.
    void navigationFitsEveryLanguageAtEveryScale() {
        if (QGuiApplication::platformName() != "windows") QSKIP("needs desktop fonts");
        const auto language = I18nManager::get_current_language();
        const int scale = get_dialog_scale().percent();
        const auto restore = qScopeGuard([&] { get_dialog_scale().set_percent(scale); I18nManager::load_language(language); });
        for (int percent : {100, 150, 200}) {
            get_dialog_scale().set_percent(percent);
            for (const auto *code : {"zh", "zh_TW", "en", "ja", "ko"}) {
                QVERIFY(I18nManager::load_language(code));
                SettingsDialog dialog(config_, config_->get_hotkey());
                dialog.fit_to_available({4000, 2400});
                dialog.show();
                auto *nav = dialog.findChild<NavigationInterface *>("SettingsNavigation");
                const int beside_text = dialog_scaled(12 + 16 + 12) + dialog_scaled(12);
                const QStringList routes{"shortcuts", "mouse", "capture", "pin", "screen_annotation", "clipboard", "translation", "appearance", "log", "other"};
                for (const auto &route : routes) {
                    if (!nav->isExpanded(route)) emit nav->arrowClicked(route);
                    QTRY_VERIFY(!nav->animating());
                    QApplication::processEvents();
                    const auto where = [&](const QString &text) { return qPrintable(QString("%1 at %2%%, %3: %4").arg(code).arg(percent).arg(route, text)); };
                    auto *item = nav->widget(route);
                    // The item's size hint holds its text and the room for the arrow.
                    QVERIFY2(item->sizeHint().width() <= item->width(), where(item->text()));
                    for (int i = 0; i < nav->subItemCount(route); ++i) {
                        auto *sub = nav->subItem(route, i);
                        QVERIFY2(sub->height() >= sub->heightForWidth(sub->width()), where(sub->text()));
                        // The shortcut pages' groups are named to keep to one line.
                        if (route == "shortcuts" || route == "mouse") QVERIFY2(sub->heightForWidth(sub->width()) == dialog_scaled(36), where(sub->text()));
                        // Chinese and Japanese break between characters; words of other scripts must each fit a line.
                        static const QRegularExpression breaks_anywhere(QStringLiteral("[\\p{Han}\\p{Hiragana}\\p{Katakana}]"));
                        for (const auto &word : sub->text().split(' ', Qt::SkipEmptyParts))
                            if (!word.contains(breaks_anywhere))
                                QVERIFY2(sub->fontMetrics().horizontalAdvance(word) <= sub->width() - beside_text, where(word));
                    }
                }
                QVERIFY(nav->widget("about")->sizeHint().width() <= nav->widget("about")->width());
                QCOMPARE(nav->scrollArea()->horizontalScrollBar()->maximum(), 0);
            }
        }
    }

};

QTEST_MAIN(SettingsWindowTests)
#include "test_settings_window.moc"
