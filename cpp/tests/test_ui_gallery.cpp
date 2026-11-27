// The development design gallery builds on every page, in both themes, without a horizontal scroll bar. With
// JIETUBA_GALLERY_SHOTS set to a folder it also saves each page as <number>_<page>_<light|dark>.png.
#include "core/i18n.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "gallery_window.h"
#include "settings/tool_settings.h"
#include <QApplication>
#include <QDir>
#include <QImage>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

using namespace jietuba;
using namespace jietuba::gallery;

class UiGalleryTests : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    QSettings *settings_ = nullptr;

private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        settings_ = new QSettings(dir_.filePath("settings.ini"), QSettings::IniFormat, this);
        auto *config = get_tool_settings_manager(settings_);
        init_gallery_runtime(*qobject_cast<QApplication *>(QCoreApplication::instance()), config);
    }

    void cleanup() {
        for (auto *widget : QApplication::topLevelWidgets()) widget->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        GalleryWindow::apply_state({});
    }

    void everyPageBuildsInLightAndDark_data() {
        QTest::addColumn<int>("page");
        QTest::addColumn<bool>("dark");
        for (const bool dark : {false, true})
            for (int page = 0; page < GalleryWindow::pages().size(); ++page)
                QTest::newRow(qPrintable(QString("%1 %2").arg(GalleryWindow::pages()[page].key, dark ? "dark" : "light"))) << page << dark;
    }

    void everyPageBuildsInLightAndDark() {
        QFETCH(int, page);
        QFETCH(bool, dark);
        GalleryState state;
        state.page = page;
        state.dark = dark;
        GalleryWindow window(state);
        QCOMPARE(get_ui_theme()->is_dark(), dark);
        window.show();
        QCoreApplication::processEvents();
        window.show_page(page);
        QCoreApplication::processEvents();

        auto *area = window.page_area(page);
        QVERIFY(area);
        QVERIFY2(area->widget()->findChildren<QWidget *>().size() > 5, "the page is empty");
        QVERIFY(area->widget()->height() > 100);
        // A page wider than its viewport shows a horizontal scroll bar.
        QCOMPARE(area->horizontalScrollBar()->maximum(), 0);
        QVERIFY(!area->horizontalScrollBar()->isVisible());
        const auto image = window.grab().toImage();
        QVERIFY(!image.isNull());
        QVERIFY(image.width() > 0 && image.height() > 0);

        const auto folder = qEnvironmentVariable("JIETUBA_GALLERY_SHOTS");
        if (folder.isEmpty()) return;
        QDir().mkpath(folder);
        const auto name = QString("%1_%2_%3").arg(page + 1, 2, 10, QChar('0')).arg(GalleryWindow::pages()[page].key, dark ? "dark" : "light");
        QVERIFY(image.save(QDir(folder).filePath(name + ".png")));
        // The whole page, which the window shows through a scroll area.
        QPixmap full(area->widget()->size());
        full.fill(css_color(get_ui_theme()->tokens().page_background));
        area->widget()->render(&full);
        QVERIFY(full.save(QDir(folder).filePath(name + "_full.png")));
    }

    void everyPageFitsInEveryLanguageAndScale() {
        for (const int scale : {100, 200}) {
            for (const auto &language : GalleryWindow::language_codes()) {
                GalleryState state;
                state.scale_percent = scale;
                state.language = language;
                GalleryWindow window(state);
                window.show();
                for (int page = 0; page < window.page_count(); ++page) {
                    window.show_page(page);
                    QCoreApplication::processEvents();
                    auto *area = window.page_area(page);
                    const auto label = QString("%1 at %2%, page %3").arg(language).arg(scale).arg(GalleryWindow::pages()[page].key);
                    QVERIFY2(area, qPrintable(label));
                    QVERIFY2(area->horizontalScrollBar()->maximum() == 0, qPrintable(label + ": horizontal scroll bar"));
                }
            }
        }
    }

    void theDialogSamplesBuild() {
        GalleryWindow window{GalleryState{}};
        window.show();
        for (QDialog *dialog : {window.make_fluent_dialog(), window.make_standard_dialog()}) {
            dialog->show();
            QCoreApplication::processEvents();
            QVERIFY(!dialog->grab().isNull());
            QVERIFY(dialog->width() > 0 && dialog->height() > 0);
            delete dialog;
        }
    }

    void applyStateSetsThemeScaleAndLanguage() {
        GalleryState state;
        state.dark = true;
        state.scale_percent = 150;
        state.language = "en";
        GalleryWindow::apply_state(state);
        QVERIFY(get_ui_theme()->is_dark());
        QCOMPARE(get_dialog_scale().percent(), 150);
        QCOMPARE(I18nManager::get_current_language(), QString("en"));
    }

    void aWindowAtAnotherScaleIsLargerAndRebuildsOnRequest() {
        GalleryState state;
        GalleryWindow normal(state);
        state.scale_percent = 150;
        GalleryWindow large(state);
        QVERIFY(large.width() > normal.width());
        QSignalSpy spy(&normal, &GalleryWindow::state_requested);
        emit normal.state_requested(state);
        QCOMPARE(spy.count(), 1);
    }

    void tokenEntriesListEveryColourOnceAndParse() {
        const auto light = ui_token_entries(LIGHT_TOKENS());
        const auto dark = ui_token_entries(DARK_TOKENS());
        QVERIFY(!light.isEmpty());
        QCOMPARE(light.size(), dark.size());
        // The mode, padded to the strings' alignment, then one string per colour: a new field that the
        // list does not name changes the size.
        QCOMPARE(sizeof(UIThemeTokens), alignof(QString) + light.size() * sizeof(QString));
        QSet<QString> names;
        for (int index = 0; index < light.size(); ++index) {
            QVERIFY2(!names.contains(light[index].first), qPrintable(light[index].first));
            names.insert(light[index].first);
            QCOMPARE(dark[index].first, light[index].first);
            for (const auto &entry : {light[index], dark[index]}) {
                QVERIFY2(css_color(entry.second).isValid(), qPrintable(entry.first + " = " + entry.second));
            }
        }
        QVERIFY(!names.contains("mode"));
    }
};

QTEST_MAIN(UiGalleryTests)
#include "test_ui_gallery.moc"
