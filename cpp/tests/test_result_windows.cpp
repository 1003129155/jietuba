#include "barcode/reader.h"
#include "barcode/result_window.h"
#include "ocr/ocr_manager.h"
#include "settings/tool_settings.h"
#include "text_recognition/recognizer.h"
#include "text_recognition/result_window.h"
#include "ui/fluent_lite/fluent_lite.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/titlebar.h"
#include "ui/fluent_lite/labels.h"
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFontDatabase>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace jietuba;

namespace {
QString font_family;

QImage text_image(const QString &text, const QSize &size = QSize(420, 90)) {
    QImage image(size, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setPen(Qt::black);
    painter.setFont(QFont(font_family, 28));
    painter.drawText(image.rect(), Qt::AlignCenter, text);
    painter.end();
    return image;
}

QImage blank_image() {
    QImage image(200, 80, QImage::Format_RGB32);
    image.fill(Qt::white);
    return image;
}

// Picks the menu entry whose text starts with label from the next popup menu.
void choose_from_next_menu(const QString &label) {
    QTimer::singleShot(0, qApp, [label] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) return;
        for (auto *action : menu->actions())
            if (action->text().startsWith(label)) menu->setActiveAction(action);
        QTest::keyClick(menu, Qt::Key_Return);
    });
}
}

class ResultWindowTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir dir_;
    QSettings *settings_ = nullptr;
    ToolSettingsManager *config_ = nullptr;
    QImage codes_image_;

private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        settings_ = new QSettings(dir_.filePath("settings.ini"), QSettings::IniFormat, this);
        config_ = get_tool_settings_manager(settings_);
        // The offscreen platform has no font directory; recognizable text needs a real font.
        const int font_id = QFontDatabase::addApplicationFont(QDir(qEnvironmentVariable("WINDIR")).filePath("Fonts/arial.ttf"));
        QVERIFY(font_id >= 0);
        font_family = QFontDatabase::applicationFontFamilies(font_id).value(0);
        codes_image_ = QImage(JIETUBA_BARCODE_FIXTURE_DIR "/barcodes.png");
        QVERIFY(!codes_image_.isNull());
    }

    void init() { settings_->clear(); }

    void cleanup() {
        for (auto *widget : QApplication::topLevelWidgets()) widget->close();
        shutdown_recognition(10000);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    // ---- Text recognition ----

    void recognizedTextLandsInAnEditableBoxAndCopies() {
        if (!is_ocr_available()) QSKIP("no OCR engine on this machine");
        auto *window = show_text_recognition(text_image("Hello 2026"), QRect(100, 100, 420, 90));
        QVERIFY(window);
        QPointer<TextRecognitionWindow> alive = window;
        // It waits in a recognizing state first.
        QVERIFY(!window->copy_button->isEnabled());
        QVERIFY(!window->layout_button->isEnabled() || window->text_edit->toPlainText().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(window->copy_button->isEnabled(), 30000);
        QVERIFY(window->text_edit->toPlainText().contains("Hello"));
        QVERIFY(!window->text_edit->isReadOnly());
        QVERIFY(window->status_label->text().contains(QString::number(window->text_edit->toPlainText().size())));
        // The edited text is what gets copied.
        window->text_edit->setPlainText("edited text");
        window->copy_button->click();
        QCOMPARE(QApplication::clipboard()->text(), QString("edited text"));
        QCOMPARE(window->copy_button->text(), QString("Copied"));
        // The window lives until it is closed.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(alive);
        window->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!alive);
    }

    void aBlankImageSaysWhyAndLeavesCopyDisabled() {
        if (!is_ocr_available()) QSKIP("no OCR engine on this machine");
        auto *window = show_text_recognition(blank_image());
        QTRY_VERIFY_WITH_TIMEOUT(window->status_label->text().contains("No text found"), 30000);
        QVERIFY(!window->copy_button->isEnabled());
    }

    void closingWhileRecognizingDoesNotBlockOrReport() {
        if (!is_ocr_available()) QSKIP("no OCR engine on this machine");
        QApplication::clipboard()->setText("before");
        config_->set_ocr_copy_on_close(true);
        auto *window = show_text_recognition(text_image("Closing early"));
        window->close();
        shutdown_recognition(30000);
        QApplication::processEvents();
        // Nothing was recognized into the window, so nothing was copied on close.
        QCOMPARE(QApplication::clipboard()->text(), QString("before"));
    }

    void copyOnCloseAndPinStateFollowTheSettings() {
        if (!is_ocr_available()) QSKIP("no OCR engine on this machine");
        auto *window = show_text_recognition(text_image("Pinned text"));
        QTRY_VERIFY_WITH_TIMEOUT(window->copy_button->isEnabled(), 30000);
        // On top by default; the choice is remembered.
        QVERIFY(window->pin_button->isChecked());
        window->pin_button->click();
        QVERIFY(!config_->get_ocr_result_on_top());
        // The layout menu's switch is a setting.
        choose_from_next_menu("Copy text when closing");
        window->layout_button->click();
        QVERIFY(config_->get_ocr_copy_on_close());
        window->text_edit->setPlainText("copied on close");
        QApplication::clipboard()->setText("before");
        window->close();
        QCOMPARE(QApplication::clipboard()->text(), QString("copied on close"));
        // The next window opens with the remembered state.
        auto *next = show_text_recognition(text_image("Again"));
        QApplication::processEvents();
        QVERIFY(!next->pin_button->isChecked());
    }

    void aRelayoutCanBeUndoneBackToTheEdit() {
        if (!is_ocr_available()) QSKIP("no OCR engine on this machine");
        auto *window = show_text_recognition(text_image("Hello World"));
        QTRY_VERIFY_WITH_TIMEOUT(window->copy_button->isEnabled(), 30000);
        const auto recognized = window->text_edit->toPlainText();
        QVERIFY(recognized.contains(' '));
        window->text_edit->selectAll();
        QTest::keyClicks(window->text_edit, "my edit");
        QCOMPARE(window->text_edit->toPlainText(), QString("my edit"));
        choose_from_next_menu("Remove spaces");
        window->layout_button->click();
        QVERIFY(config_->get_ocr_remove_spaces());
        // Spaces are only removed between CJK characters; the layout re-renders from the recognized lines.
        QCOMPARE(window->text_edit->toPlainText(), recognized);
        window->text_edit->undo();
        QCOMPARE(window->text_edit->toPlainText(), QString("my edit"));
        // The first result itself cannot be undone away.
        window->text_edit->undo();
        window->text_edit->undo();
        QVERIFY(!window->text_edit->toPlainText().isEmpty());
    }

    void theCopyDirectlyCheckboxWritesTheSetting() {
        auto *window = new TextRecognitionWindow(blank_image());
        window->setAttribute(Qt::WA_DeleteOnClose);
        window->copy_directly_check->setChecked(true);
        QVERIFY(config_->get_ocr_copy_directly_enabled());
        window->copy_directly_check->setChecked(false);
        QVERIFY(!config_->get_ocr_copy_directly_enabled());
    }

    void theToastCopiesWithoutAWindow() {
        if (!is_ocr_available()) QSKIP("no OCR engine on this machine");
        QApplication::clipboard()->setText("before");
        QVERIFY(copy_text_recognition(text_image("Toast 42")));
        QTRY_VERIFY_WITH_TIMEOUT(QApplication::clipboard()->text().contains("Toast"), 30000);
        // A failure leaves the clipboard alone.
        QApplication::clipboard()->setText("kept");
        copy_text_recognition(blank_image());
        shutdown_recognition(30000);
        QApplication::processEvents();
        QCOMPARE(QApplication::clipboard()->text(), QString("kept"));
    }

    void recognizeAsyncReportsLinesInReadingOrder() {
        if (!is_ocr_available()) QSKIP("no OCR engine on this machine");
        QVector<OcrTextLine> lines;
        QString reason = "pending";
        QObject context;
        QImage image(420, 160, QImage::Format_RGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        painter.setPen(Qt::black);
        painter.setFont(QFont(font_family, 26));
        painter.drawText(QRect(0, 10, 420, 60), Qt::AlignCenter, "First line");
        painter.drawText(QRect(0, 90, 420, 60), Qt::AlignCenter, "Second line");
        painter.end();
        recognize_async(image, &context, [&](const QVector<OcrTextLine> &result, const QString &why) {
            lines = result;
            reason = why;
        });
        QTRY_VERIFY_WITH_TIMEOUT(reason != "pending", 30000);
        QCOMPARE(reason, QString());
        QCOMPARE(lines.size(), 2);
        QVERIFY(lines[0].text.contains("First"));
        QVERIFY(lines[1].text.contains("Second"));
        QVERIFY(lines[0].top < lines[1].top);
        // A thread whose context is gone never calls back.
        bool called = false;
        {
            QObject gone;
            recognize_async(image, &gone, [&](const QVector<OcrTextLine> &, const QString &) { called = true; });
        }
        shutdown_recognition(30000);
        QApplication::processEvents();
        QVERIFY(!called);
        // Shutting down with nothing running returns at once.
        shutdown_recognition(10);
    }

    // ---- Barcode results ----

    void everyCodeGetsACardAndOnlyWebLinksOpen() {
        auto *window = show_barcode_result(codes_image_);
        QVERIFY(window);
        QCOMPARE(window->cards.size(), size_t(4));
        QVERIFY(window->summary_label->text().contains("4"));
        for (auto *card : window->cards) QVERIFY(!card->open_button);
        window->cards[1]->copy_button->click();
        QCOMPARE(QApplication::clipboard()->text(), QString("CODE128-0042"));
        QCOMPARE(window->cards[1]->copy_button->text(), QString("Copied"));
        window->copy_single_check->setChecked(true);
        QVERIFY(config_->get_barcode_copy_single_enabled());
    }

    void pointingAtACodeHighlightsBothSides() {
        auto *window = show_barcode_result(codes_image_);
        window->resize(1000, 700);
        QApplication::processEvents();
        const auto codes = read_codes(codes_image_);
        auto *view = window->image_view;
        // Image pixels to the view: fitted, centred, never enlarged.
        const double scale = std::min({double(view->width()) / codes_image_.width(), double(view->height()) / codes_image_.height(), 1.0});
        const QPointF offset((view->width() - codes_image_.width() * scale) / 2, (view->height() - codes_image_.height() * scale) / 2);
        const auto centre = codes[2].outline.boundingRect().center() * scale + offset;
        QMouseEvent move(QEvent::MouseMove, centre, QPointF(view->mapToGlobal(centre.toPoint())), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(view, &move);
        for (int i = 0; i < 4; ++i) QCOMPARE(window->cards[i]->property("active").toBool(), i == 2);
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(view, &leave);
        for (auto *card : window->cards) QVERIFY(!card->property("active").toBool());
        // Entering a card highlights it too.
        QEnterEvent enter(QPointF(5, 5), QPointF(5, 5), QPointF(5, 5));
        QApplication::sendEvent(window->cards[0], &enter);
        QVERIFY(window->cards[0]->property("active").toBool());
        // Painting with a highlighted code works at any size.
        QVERIFY(!view->grab().isNull());
    }

    void oneCodeIsCopiedWithoutAWindowWhenAsked() {
        const auto codes = read_codes(codes_image_);
        const auto single = codes_image_.copy(codes[1].outline.boundingRect().toAlignedRect().adjusted(-20, -20, 20, 20));
        QCOMPARE(read_codes(single).size(), size_t(1));
        QApplication::clipboard()->setText("before");
        QCOMPARE(show_barcode_result(single, true), nullptr);
        QCOMPARE(QApplication::clipboard()->text(), QString("CODE128-0042"));
        // Several codes, or none, still open the window.
        QVERIFY(show_barcode_result(codes_image_, true));
        auto *empty = show_barcode_result(blank_image(), true);
        QVERIFY(empty);
        QVERIFY(empty->cards.empty());
    }

    void webLinksGetAnOpenButton() {
        // Only http(s) links with a host can be opened; the card decides from the text.
        DecodedCode link;
        link.text = "https://example.com/path";
        link.format_name = "QR Code";
        link.outline = QPolygonF(QRectF(10, 10, 50, 50));
        DecodedCode mail = link;
        mail.text = "mailto:someone@example.com";
        DecodedCode bare = link;
        bare.text = "example.com";
        BarcodeResultWindow window(codes_image_, {link, mail, bare});
        QVERIFY(window.cards[0]->open_button);
        QVERIFY(!window.cards[1]->open_button);
        QVERIFY(!window.cards[2]->open_button);
    }
};

QTEST_MAIN(ResultWindowTests)
#include "test_result_windows.moc"
