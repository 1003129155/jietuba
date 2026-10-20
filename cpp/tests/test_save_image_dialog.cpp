#include "core/save.h"
#include "settings/tool_settings.h"
#include "ui/dialogs.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/labels.h"
#include "ui/fluent_lite/segmented.h"
#include "ui/save_image_dialog.h"
#include "caption_check.h"
#include <QApplication>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSettings>
#include <QSignalSpy>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QDir>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTranslator>
#include <QWheelEvent>

using namespace jietuba;

namespace {
// Gradients plus noise, so lossy formats change size with quality.
QImage noisy_image(int width = 320, int height = 200) {
    QImage image(width, height, QImage::Format_ARGB32);
    QRandomGenerator random(7);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            image.setPixel(x, y, qRgba((x * 255 / width + random.bounded(40)) & 0xFF, (y * 255 / height + random.bounded(40)) & 0xFF, random.bounded(256), 255));
    return image;
}

qint64 wait_encoded(SaveImageDialog &dialog) {
    QSignalSpy spy(&dialog, &SaveImageDialog::encode_finished);
    if (!spy.wait(10000)) return -2;
    // Later changes may still be queued; settle on the latest settings.
    while (spy.wait(SaveImageDialog::ENCODE_DELAY_MS + 300)) {}
    return dialog.encoded_bytes();
}

// Clicks the button with text in the next confirmation dialog that opens.
void answer_next_dialog(const QString &text) {
    QTimer::singleShot(0, [text] {
        for (auto *widget : QApplication::topLevelWidgets()) {
            auto *standard = qobject_cast<StandardDialog *>(widget);
            if (!standard || !standard->isVisible()) continue;
            for (auto *button : standard->button_box->buttons())
                if (button->text() == text) {
                    button->click();
                    return;
                }
        }
        answer_next_dialog(text);
    });
}

// Types name into the next tag name dialog and saves, or cancels it when name is empty.
void answer_name_dialog(const QString &name) {
    QTimer::singleShot(0, [name] {
        auto *dialog = qobject_cast<PathTagNameDialog *>(QApplication::activeModalWidget());
        if (!dialog || !dialog->isVisible()) return answer_name_dialog(name);
        if (name.isEmpty()) return dialog->reject();
        dialog->name_edit->setText(name);
        dialog->save_button->click();
    });
}

// Saves the next tag name dialog as offered, reporting the selected suggestion.
void accept_name_dialog(QString *offered) {
    QTimer::singleShot(0, [offered] {
        auto *dialog = qobject_cast<PathTagNameDialog *>(QApplication::activeModalWidget());
        if (!dialog || !dialog->isVisible()) return accept_name_dialog(offered);
        *offered = dialog->name_edit->selectedText();
        dialog->save_button->click();
    });
}
}

class SaveImageDialogTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir dir_;
    QSettings *settings_ = nullptr;
    ToolSettingsManager *config_ = nullptr;

    void select_format(SaveImageDialog &dialog, const QString &format) {
        dialog.format_combo->setCurrentIndex(dialog.format_combo->findData(format));
    }

private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        settings_ = new QSettings(dir_.filePath("settings.ini"), QSettings::IniFormat, this);
        config_ = get_tool_settings_manager(settings_);
    }

    void init() {
        settings_->clear();
        config_->set_screenshot_save_path(dir_.filePath("shots"));
    }

    void cleanup() {
        for (auto *widget : QApplication::topLevelWidgets()) widget->close();
    }

    void encodedBytesAreTheWrittenBytes() {
        const auto image = noisy_image();
        qint64 previous = 0;
        for (int quality : {30, 60, 90}) {
            const auto bytes = SaveService::encode_image(image, "JPG", quality);
            QVERIFY(bytes.size() > previous);
            previous = bytes.size();
            const auto path = dir_.filePath(QString("q%1.jpg").arg(quality));
            QVERIFY(SaveService::save_qimage_to_path(image, path, "JPG", SaveService::DEFAULT_PDF_DPI, quality));
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), bytes);
        }
        QVERIFY(SaveService::encode_image(image, "WEBP", 30).size() < SaveService::encode_image(image, "WEBP", 90).size());
        // Lossless formats ignore the quality.
        QCOMPARE(SaveService::encode_image(image, "PNG", 10), SaveService::encode_image(image, "PNG", 90));
        QCOMPARE(SaveService::encode_image(image, "BMP", 10), SaveService::encode_image(image, "BMP", -1));
        QVERIFY(SaveService::encode_image(image, "PDF").startsWith("%PDF-"));
        const auto pdf = dir_.filePath("page.pdf");
        QVERIFY(SaveService::save_qimage_to_path(image, pdf, "PDF"));
        QCOMPARE(QFileInfo(pdf).size(), static_cast<qint64>(SaveService::encode_image(image, "PDF").size()));
        QVERIFY(SaveService::encode_image(QImage(), "PNG").isEmpty());
        // A JPG side is limited to 65535 pixels.
        QVERIFY(SaveService::encode_image(QImage(70000, 1, QImage::Format_RGB32), "JPG").isEmpty());
    }

    void scalingForSave() {
        const auto image = noisy_image(320, 200);
        QCOMPARE(SaveService::scale_for_save(image, QSize(160, 100)).size(), QSize(160, 100));
        QCOMPARE(SaveService::scale_for_save(image, QSize(640, 400)).size(), QSize(640, 400));
        QCOMPARE(SaveService::scale_for_save(image, image.size()).cacheKey(), image.cacheKey());
        QCOMPARE(SaveService::scale_for_save(image, QSize()).cacheKey(), image.cacheKey());
    }

    void fileNameProblems() {
        QVERIFY(SaveImageDialog::file_name_problem("shot_1").isEmpty());
        QVERIFY(SaveImageDialog::file_name_problem(QString::fromUtf8("截图 1")).isEmpty());
        for (const char *bad : {"", "  ", "a:b", "a/b", "a?b", "a\"b", "end.", "end ", "CON", "com1", "nul.txt"})
            QVERIFY2(!SaveImageDialog::file_name_problem(bad).isEmpty(), bad);
    }

    void theSaveWindowsUseTheStandardCaption() {
        use_test_app_icon();
        SaveImageDialog dialog(noisy_image(), "shot", config_);
        VERIFY_STANDARD_CAPTION(dialog);
        PathTagNameDialog tag_name("name");
        VERIFY_STANDARD_CAPTION(tag_name);
    }

    void startsFromSettingsAndLosslessHasNoQuality() {
        settings_->setValue("app/screenshot_format", "PNG");
        SaveImageDialog dialog(noisy_image(), "screenshot_x", config_);
        QCOMPARE(dialog.current_format(), QString("PNG"));
        QCOMPARE(dialog.current_quality(), 85);
        QCOMPARE(dialog.target_size(), QSize(320, 200));
        QVERIFY(dialog.quality_block->isHidden());
        QVERIFY(!dialog.lossless_hint->isHidden());
        QVERIFY(!dialog.view_mode->isEnabled());
        QCOMPARE(dialog.preview->mode(), SavePreviewView::ENCODED);
        QVERIFY(!dialog.reset_size_button->isEnabled());
        QCOMPARE(dialog.format_combo->currentText(), QString(".PNG"));
        QCOMPARE(QDir::fromNativeSeparators(dialog.target_path()), QDir::fromNativeSeparators(dir_.filePath("shots/screenshot_x.png")));
        QCOMPARE(wait_encoded(dialog), static_cast<qint64>(SaveService::encode_image(noisy_image(), "PNG").size()));
        QVERIFY(dialog.preview->encoded().isNull());
        QVERIFY(dialog.file_size_label->text().endsWith("KB"));
    }

    void resizingLosslessStillOffersTheOriginal() {
        settings_->setValue("app/screenshot_format", "PNG");
        SaveImageDialog dialog(noisy_image(), "shot", config_);
        QVERIFY(!dialog.view_mode->isEnabled());
        dialog.width_spin->setValue(160);
        QVERIFY(dialog.view_mode->isEnabled());
        QCOMPARE(dialog.preview->mode(), SavePreviewView::ENCODED);
        dialog.reset_size_button->click();
        QVERIFY(!dialog.view_mode->isEnabled());
    }

    void qualityAndFormatReencode() {
        SaveImageDialog dialog(noisy_image(), "shot", config_);
        select_format(dialog, "JPG");
        // Saving right after a change is allowed; Save encodes on its own.
        QVERIFY(dialog.save_button->isEnabled());
        QVERIFY(!dialog.quality_block->isHidden());
        QVERIFY(dialog.lossless_hint->isHidden());
        QVERIFY(dialog.view_mode->isEnabled());
        // The preview starts on the saved result, so a quality change shows at once.
        QCOMPARE(dialog.view_mode->currentItem(), QString("encoded"));
        QCOMPARE(dialog.preview->mode(), SavePreviewView::ENCODED);
        dialog.quality_slider->setValue(20);
        const auto low = wait_encoded(dialog);
        QCOMPARE(low, static_cast<qint64>(SaveService::encode_image(noisy_image(), "JPG", 20).size()));
        QCOMPARE(dialog.preview->encoded().size(), QSize(320, 200));
        dialog.quality_slider->setValue(95);
        const auto high = wait_encoded(dialog);
        QVERIFY(high > low);
        // Several changes in a row encode only the last settings.
        QSignalSpy spy(&dialog, &SaveImageDialog::encode_finished);
        for (int quality : {40, 50, 60, 70}) dialog.quality_slider->setValue(quality);
        QVERIFY(spy.wait(10000));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().first().toLongLong(), static_cast<qint64>(SaveService::encode_image(noisy_image(), "JPG", 70).size()));
        select_format(dialog, "PNG");
        QCOMPARE(dialog.preview->mode(), SavePreviewView::ENCODED);
        QVERIFY(dialog.quality_block->isHidden());
    }

    void sizeFollowsTheLockAndTheMode() {
        SaveImageDialog dialog(noisy_image(320, 200), "shot", config_);
        // Unlocked by default: each side on its own.
        QVERIFY(!dialog.lock_button->isChecked());
        dialog.width_spin->setValue(160);
        QCOMPARE(dialog.target_size(), QSize(160, 200));
        QVERIFY(dialog.reset_size_button->isEnabled());
        // Locking keeps the original ratio from the width, and is remembered.
        dialog.lock_button->setChecked(true);
        QCOMPARE(dialog.target_size(), QSize(160, 100));
        QVERIFY(config_->get_app_setting("save_dialog_aspect_locked").toBool());
        QVERIFY(SaveImageDialog(noisy_image(320, 200), "shot", config_).lock_button->isChecked());
        dialog.height_spin->setValue(400);
        QCOMPARE(dialog.target_size(), QSize(640, 400));
        wait_encoded(dialog);
        QCOMPARE(dialog.preview->image().size(), QSize(640, 400));
        // Percentages of the original; the same size shown either way.
        dialog.size_mode->setCurrentItem("percent");
        QVERIFY(dialog.percent_mode());
        QCOMPARE(dialog.width_spin->value(), 200);
        QCOMPARE(dialog.height_spin->value(), 200);
        dialog.width_spin->setValue(50);
        QCOMPARE(dialog.target_size(), QSize(160, 100));
        QCOMPARE(dialog.height_spin->value(), 50);
        dialog.lock_button->setChecked(false);
        dialog.height_spin->setValue(25);
        QCOMPARE(dialog.target_size(), QSize(160, 50));
        dialog.size_mode->setCurrentItem("value");
        QCOMPARE(dialog.width_spin->value(), 160);
        QCOMPARE(dialog.height_spin->value(), 50);
        dialog.reset_size_button->click();
        QCOMPARE(dialog.target_size(), QSize(320, 200));
        QVERIFY(!dialog.reset_size_button->isEnabled());
    }

    void saveWritesTheShownSizeAndRemembers() {
        SaveImageDialog dialog(noisy_image(), "shot", config_);
        const auto folder = dir_.filePath("chosen");
        dialog.directory_edit->setText(QDir::toNativeSeparators(folder));
        dialog.name_edit->setText("result");
        select_format(dialog, "WEBP");
        dialog.quality_slider->setValue(70);
        dialog.lock_button->setChecked(true);
        dialog.width_spin->setValue(160);
        const auto shown = wait_encoded(dialog);
        QVERIFY(shown > 0);
        dialog.save_button->click();
        QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
        const auto path = QDir(folder).filePath("result.webp");
        QCOMPARE(QDir::fromNativeSeparators(dialog.saved_path()), path);
        QCOMPARE(QFileInfo(path).size(), shown);
        QCOMPARE(QImage(path).size(), QSize(160, 100));
        QCOMPARE(config_->get_app_setting("save_dialog_format").toString(), QString("WEBP"));
        QCOMPARE(QDir::fromNativeSeparators(config_->get_app_setting("save_dialog_last_dir").toString()), folder);
        QCOMPARE(config_->get_screenshot_quality(), qint64(70));

        // The next dialog starts where this one ended.
        SaveImageDialog next(noisy_image(), "shot", config_);
        QCOMPARE(next.current_format(), QString("WEBP"));
        QCOMPARE(next.current_quality(), 70);
        QCOMPARE(QDir::fromNativeSeparators(next.directory_edit->text()), folder);
        QCOMPARE(next.target_size(), QSize(320, 200));
    }

    void invalidNameBlocksSave() {
        SaveImageDialog dialog(noisy_image(), "shot", config_);
        dialog.name_edit->setText("a|b");
        QVERIFY(!dialog.save_button->isEnabled());
        QVERIFY(!dialog.name_hint->isHidden());
        dialog.name_edit->setText("ab");
        QVERIFY(dialog.name_hint->isHidden());
        dialog.directory_edit->setText("relative\\folder");
        QVERIFY(!dialog.save_button->isEnabled());
    }

    void typedExtensionIsNotDoubled() {
        SaveImageDialog dialog(noisy_image(), "shot", config_);
        select_format(dialog, "JPG");
        dialog.name_edit->setText("photo.JPG");
        QVERIFY(dialog.target_path().endsWith("photo.jpg"));
        dialog.name_edit->setText("photo.png");
        QVERIFY(dialog.target_path().endsWith("photo.png.jpg"));
    }

    void existingFileAsksBeforeReplacing() {
        const auto folder = dir_.filePath("replace");
        QVERIFY(QDir().mkpath(folder));
        const auto path = QDir(folder).filePath("same.png");
        QFile existing(path);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.write("keep");
        existing.close();
        SaveImageDialog dialog(noisy_image(), "same", config_);
        dialog.directory_edit->setText(folder);
        select_format(dialog, "PNG");
        answer_next_dialog("No");
        dialog.save_button->click();
        QVERIFY(dialog.saved_path().isEmpty());
        QCOMPARE(QFileInfo(path).size(), qint64(4));
        answer_next_dialog("Yes");
        dialog.save_button->click();
        QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
        QCOMPARE(QImage(path).size(), QSize(320, 200));
    }

    void classicLinkSwitchesForGood() {
        QVERIFY(use_modern_save_dialog(config_));
        SaveImageDialog declined(noisy_image(), "shot", config_);
        answer_next_dialog("No");
        emit declined.classic_link->linkActivated("classic");
        QVERIFY(!declined.switched_to_classic());
        QVERIFY(use_modern_save_dialog(config_));

        SaveImageDialog dialog(noisy_image(), "shot", config_);
        answer_next_dialog("Yes");
        emit dialog.classic_link->linkActivated("classic");
        QVERIFY(dialog.switched_to_classic());
        QCOMPARE(dialog.result(), static_cast<int>(QDialog::Rejected));
        QVERIFY(!use_modern_save_dialog(config_));
        QCOMPARE(config_->get_app_setting("save_dialog_style").toString(), QString("classic"));
    }

    void previewZoomsAroundTheCursorAndFits() {
        SavePreviewView preview;
        preview.resize(400, 300);
        preview.show();
        preview.set_image(noisy_image(320, 200));
        QCOMPARE(preview.zoom(), 1.0);
        QSignalSpy zoomed(&preview, &SavePreviewView::zoom_changed);
        const QPointF center(200, 150);
        QWheelEvent wheel(center, preview.mapToGlobal(center), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&preview, &wheel);
        QCOMPARE(preview.zoom(), 1.25);
        QCOMPARE(zoomed.count(), 1);
        // The image point under the cursor stays under it.
        QVERIFY(qAbs((center.x() - preview.image_rect().left()) / preview.zoom() - 160) < 1.0);
        QVERIFY(qAbs((center.y() - preview.image_rect().top()) / preview.zoom() - 100) < 1.0);
        QTest::mouseDClick(&preview, Qt::LeftButton, Qt::NoModifier, QPoint(200, 150));
        QCOMPARE(preview.zoom(), 1.0);
        // A fitted view shrinks with the window but never enlarges the image.
        preview.resize(200, 150);
        QVERIFY(preview.zoom() < 1.0);
        preview.resize(1000, 800);
        QCOMPARE(preview.zoom(), 1.0);

        SaveImageDialog dialog(noisy_image(), "shot", config_);
        QVERIFY(dialog.zoom_label->text().endsWith("%"));
        QVERIFY(dialog.original_label->text().contains("320 x 200"));
    }

    void splitShowsOriginalLeftAndEncodedRight() {
        QImage image(200, 100, QImage::Format_RGB32);
        image.fill(QColor(200, 40, 40));
        SavePreviewView view;
        view.resize(220, 120);
        view.set_image(image);
        QImage encoded(200, 100, QImage::Format_RGB32);
        encoded.fill(QColor(40, 40, 200));
        view.set_encoded(encoded);
        view.set_mode(SavePreviewView::SPLIT);
        const auto shot = view.grab().toImage();
        const auto rect = view.image_rect();
        QCOMPARE(shot.pixelColor(static_cast<int>(rect.left() + 20), static_cast<int>(rect.center().y() + 20)), QColor(200, 40, 40));
        QCOMPARE(shot.pixelColor(static_cast<int>(rect.right() - 20), static_cast<int>(rect.center().y() + 20)), QColor(40, 40, 200));
        // Original is the source even when the result has another size: it fills the same rectangle.
        SavePreviewView resized;
        resized.resize(220, 120);
        resized.set_original(image);
        QImage half(100, 50, QImage::Format_RGB32);
        half.fill(QColor(40, 200, 40));
        resized.set_image(half);
        resized.set_mode(SavePreviewView::ORIGINAL);
        const auto original_shot = resized.grab().toImage();
        const auto half_rect = resized.image_rect();
        QCOMPARE(original_shot.pixelColor(static_cast<int>(half_rect.center().x()), static_cast<int>(half_rect.center().y())), QColor(200, 40, 40));
        resized.set_mode(SavePreviewView::ENCODED);
        QCOMPARE(resized.grab().toImage().pixelColor(static_cast<int>(half_rect.center().x()), static_cast<int>(half_rect.center().y())), QColor(40, 200, 40));
        // An encoded copy of another size is not shown against the original.
        view.set_encoded(QImage(10, 10, QImage::Format_RGB32));
        QVERIFY(view.encoded().isNull());
    }

    void pathTagsFollowTheirNamingRules() {
        const auto tags = load_path_tags(config_);
        QCOMPARE(tags.size(), size_t(1));
        QCOMPARE(QDir::fromNativeSeparators(tags[0].path), QStandardPaths::writableLocation(QStandardPaths::DesktopLocation));
        PathTagBar bar(config_, 330);
        const auto a = dir_.filePath("a"), b = dir_.filePath("b");
        QCOMPARE(bar.add_tag("A", a), PathTagBar::AddResult::DONE);
        QCOMPARE(load_path_tags(config_).size(), size_t(2));
        // The same name for the same folder: told so, nothing changes.
        answer_next_dialog("OK");
        QCOMPARE(bar.add_tag("A", a), PathTagBar::AddResult::CANCELLED);
        // A name another folder uses: agreeing asks for another name.
        answer_next_dialog("OK");
        QCOMPARE(bar.add_tag("A", b), PathTagBar::AddResult::RETRY);
        answer_next_dialog("Cancel");
        QCOMPARE(bar.add_tag("A", b), PathTagBar::AddResult::CANCELLED);
        // A folder already tagged, however it is written: agreeing renames its tag.
        answer_next_dialog("OK");
        QCOMPARE(bar.add_tag("Renamed", QDir::toNativeSeparators(a) + "\\"), PathTagBar::AddResult::DONE);
        QCOMPARE(bar.tags().size(), size_t(2));
        QCOMPARE(bar.tags()[1].name, QString("Renamed"));
        // Renaming to a name in use asks again; the second name is taken.
        QCOMPARE(bar.add_tag("B", b), PathTagBar::AddResult::DONE);
        answer_name_dialog("B");
        answer_next_dialog("OK");
        QTimer::singleShot(300, [] { answer_name_dialog("C"); });
        bar.rename_tag(1);
        QCOMPARE(bar.tags()[1].name, QString("C"));
        bar.remove_tag(1);
        QCOMPARE(load_path_tags(config_).size(), size_t(2));
        // Deleting the last tag brings the Desktop back.
        bar.remove_tag(0);
        bar.remove_tag(0);
        QCOMPARE(bar.tags().size(), size_t(1));
        QCOMPARE(bar.chips.size(), size_t(1));
    }

    void addingAsksANameFromTheFolder() {
        PathTagBar bar(config_, 330);
        const auto folder = dir_.filePath("Screens");
        QString offered;
        accept_name_dialog(&offered);
        bar.set_current_path(folder);
        bar.add_button->click();
        QCOMPARE(offered, QString("Screens"));
        QCOMPARE(bar.tags().back().name, QString("Screens"));
        // The folder being saved to is outlined.
        QVERIFY(bar.chips.back()->property("current").toBool());
        QVERIFY(!bar.chips.front()->property("current").toBool());
    }

    void tagsSetTheSaveFolder() {
        save_path_tags(config_, {{"Shots", dir_.filePath("tagged")}});
        SaveImageDialog dialog(noisy_image(), "shot", config_);
        QCOMPARE(dialog.tag_bar->chips.size(), size_t(1));
        QVERIFY(!dialog.tag_bar->chips[0]->property("current").toBool());
        QTest::mouseClick(dialog.tag_bar->chips[0], Qt::LeftButton);
        QCOMPARE(QDir::fromNativeSeparators(dialog.directory_edit->text()), dir_.filePath("tagged"));
        QVERIFY(dialog.tag_bar->chips[0]->property("current").toBool());
    }

    void translatedInEveryLanguage() {
        const std::vector<std::pair<QString, QString>> languages = {
            {"en", "Save Image"}, {"zh", QString::fromUtf8("保存图片")}, {"zh_TW", QString::fromUtf8("儲存圖片")},
            {"ja", QString::fromUtf8("画像を保存")}, {"ko", QString::fromUtf8("이미지 저장")}};
        for (const auto &[language, title] : languages) {
            QTranslator translator;
            QVERIFY(translator.load(QString(":/translations/app_%1.qm").arg(language)));
            QCoreApplication::installTranslator(&translator);
            SaveImageDialog dialog(noisy_image(), "shot", config_);
            QCoreApplication::removeTranslator(&translator);
            QCOMPARE(dialog.windowTitle(), title);
            if (language == "zh") {
                QCOMPARE(dialog.save_button->text(), QString::fromUtf8("保存"));
                QVERIFY(dialog.classic_link->text().contains(QString::fromUtf8("使用经典对话框")));
            }
            if (language == "zh_TW") {
                QCOMPARE(dialog.save_button->text(), QString::fromUtf8("儲存"));
                QVERIFY(dialog.classic_link->text().contains(QString::fromUtf8("使用經典對話方塊")));
            }
        }
    }
};

QTEST_MAIN(SaveImageDialogTests)
#include "test_save_image_dialog.moc"
