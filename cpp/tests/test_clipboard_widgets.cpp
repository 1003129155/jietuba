#include "clipboard_test_support.h"
#include "clipboard/controllers/foreground_tracker.h"
#include "clipboard/ui/panels/setting_panel.h"
#include "clipboard/ui/theme/theme_styles.h"
#include "clipboard/ui/theme/themes.h"
#include "clipboard/ui/widgets/item_delegate.h"
#include "clipboard/ui/widgets/quick_edit_popup.h"
#include "core/native.h"
#include "core/resource_manager.h"
#include "core/ui_scale.h"
#include "settings/special_paste.h"
#include "ui/fluent_lite/cards.h"
#include "ui/fluent_lite/labels.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/segmented.h"
#include "ui/fluent_lite/theme.h"
#include "ui/settings_ui/dialog.h"
#include "ui/settings_ui/special_paste_dialog.h"
#include <QBuffer>
#include <QDesktopServices>
#include <QListWidget>
#include <QPainter>
#include <QScopeGuard>
#include <QScreen>
#include <QSignalSpy>
#include <QStyleOptionViewItem>
#include <QTest>
#include <QToolButton>
#include <QWidgetAction>
#include <algorithm>
#include <array>
#include <cmath>

using namespace clipboard_test;

namespace {
ClipboardItem make_item(qint64 id, const QString &type, const QString &content) {
    ClipboardItem item;
    item.id = id;
    item.content_type = type;
    item.content = content;
    item.created_at = QDateTime(QDate(2026, 10, 1), QTime(9, 30));
    return item;
}

void add_row(QListWidget &list, const ClipboardItem &item) {
    auto *row = new QListWidgetItem(&list);
    row->setData(ROLE_ITEM_ID, item.id);
    row->setData(ROLE_ITEM_DATA, QVariant::fromValue(item));
}

// One row painted at its own height onto a plain ground.
QImage paint_row_image(const ClipboardItemDelegate &delegate, QListWidget &list, int row, int width, const QColor &ground) {
    QStyleOptionViewItem option;
    option.rect = QRect(0, 0, width, 0);
    const QModelIndex index = list.model()->index(row, 0);
    option.rect.setHeight(delegate.sizeHint(option, index).height());
    QImage image(option.rect.size(), QImage::Format_ARGB32);
    image.fill(ground);
    QPainter painter(&image);
    delegate.paint(&painter, option, index);
    return image;
}

// The pixels in a region that differ from the ground.
int painted_pixels(const QImage &image, const QRect &region, const QColor &ground) {
    int count = 0;
    for (int y = region.top(); y <= region.bottom(); ++y)
        for (int x = region.left(); x <= region.right(); ++x) count += image.pixelColor(x, y) != ground;
    return count;
}

// The pixels in a region where two paintings differ.
int differing_pixels(const QImage &a, const QImage &b, const QRect &region) {
    int count = 0;
    for (int y = region.top(); y <= region.bottom(); ++y)
        for (int x = region.left(); x <= region.right(); ++x) count += a.pixel(x, y) != b.pixel(x, y);
    return count;
}

// How far two colors are apart, the largest difference of any channel.
int color_distance(const QColor &a, const QColor &b) {
    return std::max({std::abs(a.red() - b.red()), std::abs(a.green() - b.green()), std::abs(a.blue() - b.blue())});
}

QString png_data_url(const QSize &size) {
    QImage image(size, QImage::Format_RGB32);
    image.fill(Qt::darkGreen);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return "data:image/png;base64," + QString::fromLatin1(bytes.toBase64());
}
}

class UrlCatcher : public QObject {
    Q_OBJECT
public:
    QList<QUrl> urls;
public slots:
    void open(const QUrl &url) { urls << url; }
};

// The clipboard's smaller pieces: row painting, the hover preview and quick edit popups,
// the gear menu on its own, theme styles, foreground tracking and the settings page's data actions.
class ClipboardWidgetTests : public QObject {
    Q_OBJECT
    Environment env_;
    UrlCatcher urls_;

private slots:
    void initTestCase() {
        QVERIFY(env_.init(this));
        QDesktopServices::setUrlHandler("file", &urls_, "open");
    }

    void cleanupTestCase() { QDesktopServices::unsetUrlHandler("file"); }

    void init() {
        QVERIFY(env_.fresh_database());
        urls_.urls.clear();
    }

    void rowsSizeAndPaintEveryKind() {
        QCOMPARE(image_row_lines("small"), 1);
        QCOMPARE(image_row_lines("unknown"), 1);
        QCOMPARE(image_row_lines("medium"), 2);
        QCOMPARE(image_row_lines("large"), 3);

        const Theme theme = get_theme_manager()->get_current_theme();
        QListWidget list;
        list.resize(360, 400);
        QList<ClipboardItem> items{make_item(1, "text", "a short line"), make_item(2, "text", QString("long ").repeated(80)),
                                   make_item(3, "image", "[300x100]"), make_item(4, "file", file_payload({"C:\\a.txt", "C:\\b.txt"})),
                                   make_item(5, "image", "[10x10]"), make_item(6, "text", "#00a050")};
        items[5].sub_kind = SUB_KIND_COLOR;
        items[5].color = QColor(0, 160, 80);
        items[2].thumbnail = png_data_url(QSize(300, 100));
        items[1].is_pinned = true;
        items[0].source_app = "Notepad";
        for (const auto &item : items) {
            auto *row = new QListWidgetItem;
            row->setData(ROLE_ITEM_ID, item.id);
            row->setData(ROLE_ITEM_DATA, QVariant::fromValue(item));
            list.addItem(row);
        }
        ClipboardItemDelegate delegate(&list, theme, 17, true, 8, true, "small");
        delegate.set_pick_keys("123");
        QStyleOptionViewItem option;
        option.rect = QRect(0, 0, 360, 60);
        const int text_height = delegate.sizeHint(option, list.model()->index(0, 0)).height();
        const int image_small = delegate.sizeHint(option, list.model()->index(2, 0)).height();
        delegate.set_image_size("large");
        QVERIFY(delegate.sizeHint(option, list.model()->index(2, 0)).height() > image_small);
        QCOMPARE(image_small, text_height);
        // The time shares the text's line, so hiding it changes no height.
        delegate.set_show_metadata(false);
        QCOMPARE(delegate.sizeHint(option, list.model()->index(0, 0)).height(), text_height);

        auto paint_row = [&](int row) {
            QImage image(360, 120, QImage::Format_ARGB32);
            image.fill(Qt::white);
            QPainter painter(&image);
            QStyleOptionViewItem row_option;
            row_option.rect = QRect(0, 0, 360, 120);
            delegate.paint(&painter, row_option, list.model()->index(row, 0));
            painter.end();
            return image;
        };
        for (int row = 0; row < list.count(); ++row) QVERIFY(!paint_row(row).isNull());
        // A color item draws its color as a small swatch in the icon slot; a plain text row has none of it.
        const auto swatch_pixels = [&](int row) {
            const QImage image = paint_row(row);
            int count = 0;
            for (int y = 0; y < 40; ++y)
                for (int x = 0; x < 100; ++x) count += image.pixelColor(x, y) == QColor(0, 160, 80);
            return count;
        };
        QVERIFY(swatch_pixels(5) > 100);
        QVERIFY(swatch_pixels(5) < 14 * 14);
        QCOMPARE(swatch_pixels(0), 0);
        // While anything is marked every row has a circle at its start, filled with the accent only when marked;
        // the cursor only washes the row, so an unmarked row under it never looks marked.
        const QColor accent(theme.colors.border_selected);
        const QRectF circle = delegate.mark_circle_rect(QRect(0, 0, 360, 120));
        const QPoint in_circle(qRound(circle.center().x()), qRound(circle.top() + circle.height() * 0.2));
        const QColor plain = paint_row(0).pixelColor(12, 60);
        delegate.set_cursor_id(1);
        const QColor cursor = paint_row(0).pixelColor(12, 60);
        QVERIFY(cursor != plain);
        // The wash is inset from the list's edge.
        QCOMPARE(paint_row(0).pixelColor(1, 60), QColor(Qt::white));
        QVERIFY(paint_row(0).pixelColor(in_circle) != accent);
        delegate.set_marked_ids({1});
        QCOMPARE(paint_row(0).pixelColor(in_circle), accent);
        QVERIFY(paint_row(1).pixelColor(in_circle) != accent);
        // A row past the direct pick keys gets the circle too.
        QVERIFY(paint_row(5).pixelColor(in_circle) != accent);
        delegate.set_marked_ids({6});
        QCOMPARE(paint_row(5).pixelColor(in_circle), accent);
        delegate.set_cursor_id(std::nullopt);
        delegate.set_marked_ids({});
        // Other themes and hidden shortcuts all still paint.
        delegate.set_show_shortcuts(false);
        delegate.set_font_size(13);
        for (const auto &[name, theme_entry] : PRESET_THEMES()) {
            delegate.set_theme(theme_entry);
            QVERIFY(!paint_row(3).isNull());
        }
        delegate.apply_scale();
        ClipboardItemDelegate::clear_thumb_cache();
        QVERIFY(!paint_row(2).isNull());
    }

    void nonImageRowsShareTheLineLimit() {
        QListWidget list;
        list.resize(360, 600);
        ClipboardItemDelegate delegate(&list, get_theme_manager()->get_current_theme(), 15, false);
        auto text = make_item(1, "text", "first\r\n\nthird\nfourth");
        auto link = make_item(2, "text", "https://example.com/" + QString("path/").repeated(100));
        link.sub_kind = SUB_KIND_URL;
        auto file = make_item(3, "file", file_payload({QString("C:/") + QString("filename_").repeated(80) + ".txt"}));
        auto color = make_item(4, "text", "#FAFBFC");
        color.color = QColor("#FAFBFC");
        color.title = "first\nsecond\nthird\nfourth";
        auto image = make_item(5, "image", "[300x100]");
        image.thumbnail = png_data_url(QSize(300, 100));
        auto short_text = make_item(6, "text", "short");
        for (const auto &item : {text, link, file, color, image, short_text}) {
            auto *row = new QListWidgetItem(&list);
            row->setData(ROLE_ITEM_ID, item.id);
            row->setData(ROLE_ITEM_DATA, QVariant::fromValue(item));
        }
        QStyleOptionViewItem option;
        option.rect = QRect(0, 0, 360, 100);
        const auto height = [&](int row) { return delegate.sizeHint(option, list.model()->index(row, 0)).height(); };
        // A line of the 15px font is 1.4 times its size. Rows showing several lines get more room around them,
        // and an image row is one line tall whatever the line setting.
        const int line = 21, padding = 16, multi_line_padding = 24;
        const int single = height(0);
        QCOMPARE(single, line + padding);
        QCOMPARE(height(3), single);
        QCOMPARE(height(4), single);
        delegate.set_display_lines(3);
        QCOMPARE(height(0), 3 * line + multi_line_padding);
        for (int row : {1, 2}) QVERIFY(height(row) > line + multi_line_padding);
        QCOMPARE(height(3), 3 * line + multi_line_padding);
        QCOMPARE(height(4), line + multi_line_padding);
        QCOMPARE(height(5), line + multi_line_padding);
        const int multiline_height = height(0);
        // The time sits on the first line: no extra height.
        delegate.set_show_metadata(true);
        QCOMPARE(height(0), multiline_height);
        delegate.set_show_metadata(false);
        const auto paint = [&] {
            QImage pixels(360, multiline_height + 20, QImage::Format_ARGB32);
            pixels.fill(Qt::white);
            QPainter painter(&pixels);
            option.rect.setHeight(multiline_height);
            delegate.paint(&painter, option, list.model()->index(0, 0));
            return pixels;
        };
        const auto multiline = paint();
        delegate.set_display_lines(1);
        const auto one_line = paint();
        int changed = 0;
        for (int y = single; y < multiline_height - 2; ++y)
            for (int x = 40; x < 300; ++x) changed += multiline.pixel(x, y) != one_line.pixel(x, y);
        QVERIFY(changed > 10);
        delegate.set_display_lines(5);
        QCOMPARE(height(0), 4 * line + multi_line_padding);
    }

    void laterLinesRunUnderTheTime() {
        const Theme theme = *preset_theme("light");
        QListWidget list;
        ClipboardItemDelegate delegate(&list, theme, 15, true, 8, false, "small", 2);
        add_row(list, make_item(1, "text", QString("m").repeated(200)));
        const QColor ground(theme.colors.bg_primary);
        const QImage with_time = paint_row_image(delegate, list, 0, 360, ground);
        delegate.set_show_metadata(false);
        const QImage without_time = paint_row_image(delegate, list, 0, 360, ground);
        // The two lines' halves of the row, and the stretch at the line's end where the first line has the time.
        const QRect first_line(0, 0, 360, with_time.height() / 2), second_line(0, with_time.height() / 2, 360, with_time.height() / 2);
        const QRect line_end(310, 0, 30, with_time.height());
        // The first line gives the time its room, the second runs on to the row's end either way.
        QVERIFY(differing_pixels(with_time, without_time, first_line.intersected(line_end)) > 20);
        QCOMPARE(differing_pixels(with_time, without_time, second_line), 0);
        QVERIFY(painted_pixels(with_time, second_line.intersected(line_end), with_time.pixelColor(200, with_time.height() - 4)) > 20);
    }

    void imageRowsShowAThumbnailByTheImageSize() {
        const Theme theme = *preset_theme("light");
        QListWidget list;
        ClipboardItemDelegate delegate(&list, theme, 15, false, 8, false, "small");
        auto image = make_item(1, "image", "[300x100]");
        image.thumbnail = png_data_url(QSize(300, 100));
        add_row(list, image);
        QStyleOptionViewItem option;
        option.rect = QRect(0, 0, 360, 100);
        const auto height = [&] { return delegate.sizeHint(option, list.model()->index(0, 0)).height(); };
        const auto thumbnail = [&] {
            const QImage painted = paint_row_image(delegate, list, 0, 360, QColor(theme.colors.bg_primary));
            QRect box;
            for (int y = 0; y < painted.height(); ++y)
                for (int x = 0; x < painted.width(); ++x)
                    if (color_distance(painted.pixelColor(x, y), QColor(Qt::darkGreen)) < 40) box |= QRect(x, y, 1, 1);
            return box;
        };
        // One, two and three lines of the 15px font, each with the same room around them: by default a text row's height.
        const int line = 21, padding = 16;
        QCOMPARE(height(), line + padding);
        // A 3:1 picture fills the slot's width, 1.6 times the card's height less a 3px margin above and below.
        const auto slot_width = [&] { return (height() - 4 - 2 * 3) * 1.6; };
        QVERIFY(std::abs(thumbnail().width() - slot_width()) <= 2);
        QVERIFY(std::abs(thumbnail().height() - slot_width() / 3) <= 2);
        delegate.set_image_size("medium");
        QCOMPARE(height(), 2 * line + padding);
        delegate.set_image_size("large");
        QCOMPARE(height(), 3 * line + padding);
        QVERIFY(std::abs(thumbnail().width() - slot_width()) <= 2);
    }

    void plainTextHasNoIconAndOtherKindsDo() {
        const Theme theme = *preset_theme("light");
        QListWidget list;
        ClipboardItemDelegate delegate(&list, theme, 17, false, 8, false);
        auto link = make_item(2, "text", "x");
        link.sub_kind = SUB_KIND_URL;
        auto path = make_item(3, "text", "x");
        path.sub_kind = SUB_KIND_PATH;
        auto color = make_item(4, "text", "x");
        color.color = QColor("#123456");
        for (const auto &item : {make_item(1, "text", "x"), link, path, color, make_item(5, "file", file_payload({"C:\\x"})), make_item(6, "image", "x")})
            add_row(list, item);
        const QColor ground(theme.colors.bg_primary);
        // Every other kind puts an icon where a plain text row has its text, and the text after it.
        const QImage plain = paint_row_image(delegate, list, 0, 360, ground);
        for (int row = 1; row < list.count(); ++row) {
            const QImage other = paint_row_image(delegate, list, row, 360, ground);
            QVERIFY2(differing_pixels(plain, other, QRect(14, 0, 20, plain.height())) > 20, qPrintable(QString::number(row)));
            QVERIFY2(differing_pixels(plain, other, QRect(38, 0, 20, plain.height())) > 10, qPrintable(QString::number(row)));
        }
        // The same text, moved along by the 16px icon and the gap after it.
        const QImage linked = paint_row_image(delegate, list, 1, 360, ground);
        int moved = 0;
        for (int y = 0; y < plain.height(); ++y)
            for (int x = 16; x < 46; ++x) moved += plain.pixel(x, y) != linked.pixel(x + 24, y);
        QCOMPARE(moved, 0);
    }

    void rowsSitOnSeparateCards() {
        for (const char *name : {"light", "dark"}) {
            const Theme theme = *preset_theme(name);
            QListWidget list;
            ClipboardItemDelegate delegate(&list, theme, 17, false, 8, false);
            add_row(list, make_item(1, "text", "x"));
            const QColor ground(theme.colors.bg_primary);
            const QImage row = paint_row_image(delegate, list, 0, 360, ground);
            const QColor card = row.pixelColor(200, row.height() / 2);
            // The card stands off the ground but stays far fainter than the cursor's wash.
            QVERIFY2(card != ground, name);
            delegate.set_cursor_id(1);
            const QColor cursor = paint_row_image(delegate, list, 0, 360, ground).pixelColor(200, row.height() / 2);
            QVERIFY2(color_distance(card, ground) * 3 < color_distance(cursor, ground), name);
            // Two rows' cards are parted by the ground at the top and bottom of each row, and at its sides.
            QCOMPARE(row.pixelColor(200, 0), ground);
            QCOMPARE(row.pixelColor(200, row.height() - 1), ground);
            QCOMPARE(row.pixelColor(2, row.height() / 2), ground);
        }
    }

    void rowIconsAreLookedUpByContentTypeAndSubKind() {
        const QList<std::tuple<QString, QString, QString>> cases{
            {"text", "", "clipboard_text.svg"},
            {"text", SUB_KIND_URL, "clipboard_link.svg"},
            {"text", SUB_KIND_PATH, "clipboard_folder.svg"},
            // A color draws a swatch; text it cannot parse, and sub kinds the table does not know, are plain text.
            {"text", SUB_KIND_COLOR, "clipboard_text.svg"},
            {"text", "sql", "clipboard_text.svg"},
            {"image", "", "clipboard_image.svg"},
            {"file", "", "clipboard_file.svg"},
            {"file", "folder", "clipboard_folder.svg"},
            {"file", "image", "clipboard_file_image.svg"},
            {"file", "spreadsheet", "clipboard_file_spreadsheet.svg"},
            {"file", "document", "clipboard_file_document.svg"},
            {"file", "presentation", "clipboard_file_presentation.svg"},
            {"file", "pdf", "clipboard_file_pdf.svg"},
            {"file", "markdown", "clipboard_file_markdown.svg"},
            {"file", "text", "clipboard_file_text.svg"},
            {"file", "archive", "clipboard_file_archive.svg"},
            {"file", "audio", "clipboard_file_audio.svg"},
            {"file", "video", "clipboard_file_video.svg"},
            // A file the backend could not sort, or a mix of kinds, is a plain file; text sub kinds mean nothing for files.
            {"file", "3mf", "clipboard_file.svg"},
            {"file", SUB_KIND_URL, "clipboard_file.svg"},
            {"unknown", "", "clipboard_text.svg"},
        };
        for (const auto &[type, sub_kind, icon] : cases) {
            QCOMPARE(kind_icon(type, sub_kind), icon);
            // Every icon is in the resources and draws something.
            const QImage image = tinted_icon(ResourceManager::get_icon_path(icon), "#000000").pixmap(QSize(32, 32)).toImage();
            int inked = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x) inked += image.pixelColor(x, y).alpha() > 128;
            QVERIFY2(inked > 20, qPrintable(icon));
        }
    }

    void fileRowsShowTheirCategoryOrTheGenericIcon() {
        const Theme theme = *preset_theme("light");
        QListWidget list;
        ClipboardItemDelegate delegate(&list, theme, 17, false, 8, false);
        for (const QString sub_kind : {"", "spreadsheet", "3mf", "folder"}) {
            auto item = make_item(1, "file", file_payload({"C:\\a"}));
            item.sub_kind = sub_kind;
            add_row(list, item);
        }
        const QColor ground = QColor(theme.colors.bg_primary);
        const auto slot = [&](int row) { return paint_row_image(delegate, list, row, 360, ground).copy(16, 0, 21, 30); };
        QVERIFY(slot(1) != slot(0));
        QCOMPARE(slot(2), slot(0));
        QVERIFY(slot(3) != slot(0));
    }

    void theSourceAndTimeAreShortened() {
        QCOMPARE(row_source_text("chrome.exe"), QString("chrome"));
        QCOMPARE(row_source_text("Code.EXE"), QString("Code"));
        QCOMPARE(row_source_text(" notepad.exe "), QString("notepad"));
        QCOMPARE(row_source_text("WeChat"), QString("WeChat"));
        QCOMPARE(row_source_text(QString()), QString());
        const QDate today(2026, 10, 28);
        QCOMPARE(row_time_text(QDateTime(today, QTime(9, 5)), today), QString("09:05"));
        QCOMPARE(row_time_text(QDateTime(today, QTime(23, 59)), today), QString("23:59"));
        QCOMPARE(row_time_text(QDateTime(QDate(2026, 10, 27), QTime(23, 59)), today), QString("10-27"));
        // Another year still shows the day only.
        QCOMPARE(row_time_text(QDateTime(QDate(2025, 1, 2), QTime(9, 5)), today), QString("01-02"));
    }

    void theTimeShowsAtTheLineEndAndCanBeHidden() {
        const Theme theme = *preset_theme("light");
        QListWidget list;
        ClipboardItemDelegate delegate(&list, theme, 17, true);
        auto item = make_item(1, "text", "x");
        // The source is left to the preview.
        item.source_app = "chrome.exe";
        item.created_at = QDateTime::currentDateTime();
        QCOMPARE(delegate.meta_text(item), item.created_at.time().toString("HH:mm"));
        item.created_at = QDateTime::currentDateTime().addDays(-3);
        QCOMPARE(delegate.meta_text(item), item.created_at.date().toString("MM-dd"));
        item.created_at = QDateTime();
        QVERIFY(delegate.meta_text(item).isEmpty());

        item.created_at = QDateTime(QDate(2026, 1, 2), QTime(9, 5));
        add_row(list, item);
        const QColor ground = QColor(theme.colors.bg_primary);
        const QImage shown = paint_row_image(delegate, list, 0, 360, ground);
        delegate.set_show_metadata(false);
        QVERIFY(delegate.meta_text(item).isEmpty());
        const QImage hidden = paint_row_image(delegate, list, 0, 360, ground);
        // Only the end of the line changes.
        QVERIFY(differing_pixels(shown, hidden, QRect(200, 0, 150, shown.height())) > 20);
        QCOMPARE(differing_pixels(shown, hidden, QRect(0, 0, 200, shown.height())), 0);
    }

    void pickKeysLabelTheFirstRowsAtTheirStart() {
        QListWidget list;
        ClipboardItemDelegate delegate(&list, *preset_theme("light"), 17, false);
        // The default keys run from 1 to 9 and on through a to z, letters as capitals.
        QCOMPARE(delegate.key_label(0), QString("1"));
        QCOMPARE(delegate.key_label(8), QString("9"));
        QCOMPARE(delegate.key_label(9), QString("A"));
        QCOMPARE(delegate.key_label(34), QString("Z"));
        QCOMPARE(delegate.key_label(35), QString());
        delegate.set_pick_keys("12a");
        QCOMPARE(delegate.key_label(1), QString("2"));
        QCOMPARE(delegate.key_label(2), QString("A"));
        QCOMPARE(delegate.key_label(3), QString());
        QCOMPARE(delegate.key_label(-1), QString());
        // While rows are marked the keys paste nothing and show nothing.
        delegate.set_marked_ids({7});
        QCOMPARE(delegate.key_label(0), QString());
        delegate.set_marked_ids({});
        QCOMPARE(delegate.key_label(0), QString("1"));
        delegate.set_show_shortcuts(false);
        QCOMPARE(delegate.key_label(0), QString());
        delegate.set_show_shortcuts(true);

        // The label is drawn at the row's start and pushes the text along.
        add_row(list, make_item(1, "text", "x"));
        delegate.set_pick_keys("w");
        const QColor ground = QColor(preset_theme("light")->colors.bg_primary);
        const QImage labelled = paint_row_image(delegate, list, 0, 360, ground);
        delegate.set_show_shortcuts(false);
        const QImage keys_off = paint_row_image(delegate, list, 0, 360, ground);
        QVERIFY(differing_pixels(labelled, keys_off, QRect(14, 0, 40, labelled.height())) > 20);
        // A row past the pick keys leaves no empty room: it looks just as with the keys off.
        delegate.set_show_shortcuts(true);
        delegate.set_pick_keys("");
        QCOMPARE(paint_row_image(delegate, list, 0, 360, ground), keys_off);
    }

    void aPinnedRowDrawsAPinAtTheEndOfItsLine() {
        const Theme theme = *preset_theme("light");
        QListWidget list;
        ClipboardItemDelegate delegate(&list, theme, 17, false, 8, false);
        auto item = make_item(1, "text", "x");
        add_row(list, item);
        item.id = 2;
        item.is_pinned = true;
        add_row(list, item);
        const QColor ground = QColor(theme.colors.bg_primary);
        // With the time and keys off, the pin is the last thing on the line.
        const QImage plain = paint_row_image(delegate, list, 0, 360, ground);
        const QRect end(320, 4, 26, plain.height() - 8);
        const QColor card = plain.pixelColor(end.topLeft());
        QCOMPARE(painted_pixels(plain, end, card), 0);
        QVERIFY(painted_pixels(paint_row_image(delegate, list, 1, 360, ground), end, card) > 15);
    }

    void theCursorAndMarksWashRowsDifferently() {
        const Theme theme = *preset_theme("light");
        QListWidget list;
        ClipboardItemDelegate delegate(&list, theme, 17, true);
        add_row(list, make_item(1, "text", "x"));
        const QColor ground = QColor(theme.colors.bg_primary);
        // Clear of the key, the text, the mark circle and the cursor's bar, inside the card.
        const QPoint inside(12, 17);
        const auto at = [&](const QPoint &point) { return paint_row_image(delegate, list, 0, 360, ground).pixelColor(point); };
        const QColor plain = at(inside);

        delegate.set_cursor_id(1);
        const QColor cursor = at(inside);
        // The cursor's bar at the card's start, in the theme's accent.
        const int middle = paint_row_image(delegate, list, 0, 360, ground).height() / 2;
        QCOMPARE(at(QPoint(8, middle)), QColor(theme.colors.accent_primary));
        delegate.set_cursor_id(std::nullopt);
        QVERIFY(at(QPoint(8, middle)) != QColor(theme.colors.accent_primary));
        delegate.set_marked_ids({1});
        const QColor marked = at(inside);
        QVERIFY(cursor != plain);
        QVERIFY(marked != plain);
        QVERIFY(cursor != marked);
        // The plain card is the faintest, the cursor next, so a mark never reads as the cursor.
        QVERIFY(color_distance(plain, ground) < color_distance(cursor, ground));
        QVERIFY(color_distance(cursor, ground) < color_distance(marked, ground));
        // The cursor's wash takes the theme's accent.
        QVERIFY(color_distance(cursor, QColor(theme.colors.accent_primary)) < color_distance(ground, QColor(theme.colors.accent_primary)));

        // A marked row under the cursor keeps its mark, gains an outline in the selection color and no bar.
        const QColor outline(theme.colors.border_selected);
        QVERIFY(at(QPoint(6, 17)) != outline);
        delegate.set_cursor_id(1);
        QCOMPARE(at(inside), marked);
        QCOMPARE(at(QPoint(6, 17)), outline);
        QCOMPARE(at(QPoint(8, middle)), marked);

        // The wash has rounded corners, inset from the list's edge.
        delegate.set_marked_ids({});
        QCOMPARE(at(QPoint(1, 17)), ground);
        QVERIFY(color_distance(at(QPoint(6, 1)), ground) < color_distance(cursor, ground) / 2);
    }

    void rowIconsAreDrawnInTheThemesColor() {
        // The icon's most inked pixel mixes the ground and the theme's secondary text color.
        for (const char *name : {"light", "dark", "blue"}) {
            const Theme theme = *preset_theme(name);
            QListWidget list;
            ClipboardItemDelegate delegate(&list, theme, 17, false, 8, false);
            auto link = make_item(1, "text", "x");
            link.sub_kind = SUB_KIND_URL;
            add_row(list, link);
            add_row(list, make_item(2, "file", file_payload({"C:\\a.txt"})));
            const QColor ink_color = QColor(theme.colors.text_secondary);
            for (int row = 0; row < 2; ++row) {
                QImage image = paint_row_image(delegate, list, row, 360, QColor(theme.colors.bg_primary));
                // The icon lies on the row's card.
                const QColor ground = image.pixelColor(200, image.height() / 2);
                QColor ink = ground;
                for (int y = 0; y < image.height(); ++y)
                    for (int x = 16; x < 36; ++x)
                        if (color_distance(image.pixelColor(x, y), ground) > color_distance(ink, ground)) ink = image.pixelColor(x, y);
                QVERIFY2(color_distance(ink, ground) > 40, name);
                // The coverage from the channel that tells ground and ink apart best, then the others must agree with it.
                const std::array<std::pair<int, int>, 3> channels{{{ink.red(), ink_color.red() - ground.red()}, {ink.green(), ink_color.green() - ground.green()}, {ink.blue(), ink_color.blue() - ground.blue()}}};
                const std::array<int, 3> grounds{ground.red(), ground.green(), ground.blue()};
                int best = 0;
                for (int i = 1; i < 3; ++i)
                    if (std::abs(channels[i].second) > std::abs(channels[best].second)) best = i;
                const double coverage = static_cast<double>(channels[best].first - grounds[best]) / channels[best].second;
                QVERIFY2(coverage > 0.4, name);
                for (int i = 0; i < 3; ++i) QVERIFY2(std::abs(channels[i].first - (grounds[i] + coverage * channels[i].second)) <= 12, name);
            }
        }
    }

    void rowsFollowThePanelScale() {
        const auto restore = qScopeGuard([] { get_ui_scale().set_percent(100); });
        QListWidget list;
        ClipboardItemDelegate delegate(&list, *preset_theme("light"), 17, true);
        auto image = make_item(2, "image", "[300x100]");
        image.thumbnail = png_data_url(QSize(300, 100));
        add_row(list, make_item(1, "text", "x"));
        add_row(list, image);
        QStyleOptionViewItem option;
        option.rect = QRect(0, 0, 360, 60);
        const auto height = [&](int row) { return delegate.sizeHint(option, list.model()->index(row, 0)).height(); };
        const QRectF circle = delegate.mark_circle_rect(QRect(0, 0, 360, 100));
        const int text_100 = height(0), image_100 = height(1);
        QCOMPARE(circle.width(), 14.0);
        get_ui_scale().set_percent(150);
        delegate.apply_scale();
        // Heights follow the scale to within the rounding of the font's line.
        QVERIFY(std::abs(height(0) - text_100 * 1.5) <= 4);
        QVERIFY(std::abs(height(1) - image_100 * 1.5) <= 4);
        QCOMPARE(delegate.mark_circle_rect(QRect(0, 0, 360, 100)).width(), 21.0);
        QVERIFY(delegate.mark_circle_rect(QRect(0, 0, 360, 100)).left() > circle.left());
        // The row still paints inside its own height.
        QVERIFY(!paint_row_image(delegate, list, 0, 540, Qt::white).isNull());
    }

    void displayLineSettingsPersistAndClampWithoutChangingFontOrImages() {
        QSettings stored(env_.dir.filePath("display-lines.ini"), QSettings::IniFormat);
        stored.clear();
        ToolSettingsManager settings(stored);
        QCOMPARE(settings.get_clipboard_display_lines(), 1);
        const auto font = settings.get_clipboard_font_size();
        const auto image = settings.get_clipboard_image_size();
        settings.set_clipboard_display_lines(3);
        QCOMPARE(stored.value("clipboard/display_lines").toInt(), 3);
        ToolSettingsManager reopened(stored);
        QCOMPARE(reopened.get_clipboard_display_lines(), 3);
        QCOMPARE(reopened.get_clipboard_font_size(), font);
        QCOMPARE(reopened.get_clipboard_image_size(), image);
        settings.set_clipboard_display_lines(0);
        QCOMPARE(settings.get_clipboard_display_lines(), 1);
        stored.setValue("clipboard/display_lines", 99);
        QCOMPARE(settings.get_clipboard_display_lines(), 5);
    }

    void thePreviewShowsTextFilesAndImages() {
        auto *preview = PreviewPopup::instance();
        preview->set_manager(&ClipboardManager::instance());
        // Disabled: nothing shows.
        preview->set_display_enabled(false);
        preview->show_preview(make_item(1, "text", "hidden"), QPoint(100, 100), 0);
        QVERIFY(!preview->isVisible());
        preview->set_display_enabled(true);
        preview->show_preview(make_item(1, "text", "hello\nworld"), QPoint(100, 100), 0);
        QVERIFY(preview->isVisible());
        QCOMPARE(preview->title_label->text(), QString("2026-10-01 09:30:00"));
        QCOMPARE(preview->content_widget->toPlainText(), QString("hello\nworld"));
        // The source the list rows leave out follows the time.
        auto sourced = make_item(11, "text", "from a program");
        sourced.source_app = "WeChat.exe";
        preview->show_preview(sourced, QPoint(100, 100), 0);
        QCOMPARE(preview->title_label->text(), QString("2026-10-01 09:30:00 \u00B7 WeChat"));
        // A long text wraps at the maximum width.
        preview->show_preview(make_item(2, "text", QString("word ").repeated(400)), QPoint(100, 100), 0);
        QCOMPARE(preview->content_widget->width(), scaled(500));
        // Files: one, several in one folder, several folders, none and broken content. A single file waits for the
        // background check of whether it is a picture.
        preview->show_preview(make_item(3, "file", file_payload({"C:\\docs\\a.txt"})), QPoint(10, 10), 0);
        QTRY_COMPARE(preview->content_widget->toPlainText(), QString("a.txt\n\nC:\\docs\\a.txt"));
        // The window is as big as what it shows, not stretched to a window's usual 200 x 100.
        QTRY_VERIFY(preview->width() < 200 && preview->height() < 100);
        QCOMPARE(preview->size(), preview->layout()->sizeHint());
        preview->show_preview(make_item(4, "file", file_payload({"C:\\docs\\a.txt", "C:\\docs\\b.txt"})), QPoint(10, 10), 0);
        QCOMPARE(preview->content_widget->toPlainText(), QString("a.txt\nb.txt\n\nC:\\docs"));
        preview->show_preview(make_item(5, "file", file_payload({"C:\\x\\a.txt", "C:\\y\\b.txt", "C:\\y\\c.txt"})), QPoint(10, 10), 0);
        QCOMPARE(preview->content_widget->toPlainText(), QString("[1] a.txt\nC:\\x\\a.txt\n\n[2]\n  b.txt\n  c.txt\nC:\\y"));
        preview->show_preview(make_item(6, "file", "{\"files\": []}"), QPoint(10, 10), 0);
        QVERIFY(!preview->content_widget->toPlainText().isEmpty());
        preview->show_preview(make_item(7, "file", "not json"), QPoint(10, 10), 0);
        QCOMPARE(preview->content_widget->toPlainText(), QString("not json"));
        // An image without stored pixels falls back to its thumbnail, decoded in the background.
        auto image = make_item(8, "image", "[30x20]");
        image.thumbnail = png_data_url(QSize(30, 20));
        preview->show_preview(image, QPoint(10, 10), 0);
        QTRY_VERIFY(preview->image_label->isVisible());
        QVERIFY(!preview->content_widget->isVisible());
        // Several pictures copied from Word are kept without pixels: their text shows instead of the last picture.
        preview->show_preview(make_item(10, "image", "[PNG+CF_DIB 7.9 MB]"), QPoint(10, 10), 0);
        QVERIFY(preview->isVisible());
        QVERIFY(!preview->image_label->isVisible());
        QCOMPARE(preview->content_widget->toPlainText(), QString("[PNG+CF_DIB 7.9 MB]"));
        // Delayed: hiding before the delay cancels it; the same item shown twice stays put.
        preview->hide_preview();
        preview->show_preview(make_item(9, "text", "later"), QPoint(10, 10), 50);
        QVERIFY(!preview->isVisible());
        preview->hide_preview();
        QTest::qWait(100);
        QVERIFY(!preview->isVisible());
        preview->show_preview(make_item(9, "text", "later"), QPoint(10, 10), 20);
        QTRY_VERIFY(preview->isVisible());
        preview->show_preview(make_item(9, "text", "later"), QPoint(10, 10), 0);
        preview->show_preview(std::nullopt, QPoint());
        QVERIFY(!preview->isVisible());
        preview->apply_scale();
        preview->set_display_enabled(false);
    }

    void colorPreviewShowsALargeSwatchAndClearsItForOtherItems() {
        auto *preview = PreviewPopup::instance();
        preview->hide_preview();
        preview->set_display_enabled(true);
        auto color = make_item(101, "text", "#FAFBFC");
        color.sub_kind = SUB_KIND_COLOR;
        color.color = QColor("#FAFBFC");
        preview->show_preview(color, QPoint(100, 100), 0);
        QVERIFY(preview->color_label->isVisible());
        QCOMPARE(preview->color_label->size(), QSize(scaled(240), scaled(128)));
        const auto pixels = preview->color_label->pixmap().toImage();
        QCOMPARE(pixels.pixelColor(pixels.rect().center()), QColor("#FAFBFC"));
        QCOMPARE(preview->content_widget->toPlainText(), QString("#FAFBFC"));
        color.id = 102;
        color.color = QColor(20, 40, 60, 0);
        preview->show_preview(color, QPoint(100, 100), 0);
        const auto transparent = preview->color_label->pixmap().toImage();
        QVERIFY(transparent.pixelColor(scaled(5), scaled(5)) != transparent.pixelColor(scaled(17), scaled(5)));
        preview->show_preview(make_item(103, "text", "ordinary text"), QPoint(100, 100), 0);
        QVERIFY(!preview->color_label->isVisible());
        QCOMPARE(preview->content_widget->toPlainText(), QString("ordinary text"));
        preview->set_display_enabled(false);
    }

    void sidePositionsAvoidTheWindowAndStayOnScreen() {
        const QRect screen = QGuiApplication::primaryScreen()->availableGeometry();
        const QSize size(100, 80);
        const QRect middle(screen.center().x() - 50, screen.center().y() - 50, 100, 100);
        // Left preferred and possible.
        QCOMPARE(side_position(size, middle.topLeft(), middle, "left", 10).x(), middle.left() - 100 - 10);
        // Right preferred, or automatic.
        QCOMPARE(side_position(size, middle.topLeft(), middle, "right", 10).x(), middle.right() + 10);
        QCOMPARE(side_position(size, middle.topLeft(), middle, "auto", 10).x(), middle.right() + 10);
        // A window filling the left: the right side.
        const QRect left_edge(screen.left(), screen.top(), 50, 100);
        QCOMPARE(side_position(size, left_edge.topLeft(), left_edge, "left", 10).x(), left_edge.right() + 10);
        // Neither side fits: the roomier one, clamped on screen.
        const QRect wide(screen.left() + 20, screen.top(), screen.width() - 40, 100);
        const QPoint clamped = side_position(size, wide.topLeft(), wide, "left", 10);
        QVERIFY(clamped.x() >= screen.left() && clamped.x() + size.width() <= screen.right() + 1);
        // Without a window: right of the point, or left of it near the screen edge; never below the screen.
        const QPoint near_right(screen.right() - 10, screen.bottom() - 10);
        const QPoint placed = side_position(size, near_right);
        QVERIFY(placed.x() + size.width() <= near_right.x());
        QVERIFY(placed.y() + size.height() <= screen.bottom());
        QVERIFY(side_position(size, QPoint(screen.left() + 10, screen.top() + 10)).x() > screen.left() + 10);
    }

    void quickEditKeysSaveCancelAndPaste() {
        QWidget owner;
        QuickEditPopup popup(&owner);
        QSignalSpy finished(&popup, &QuickEditPopup::finished);
        auto item = make_item(5, "text", "body");
        item.html_content = "<b>body</b>";
        const Theme theme = get_theme_manager()->get_current_theme();
        popup.open_for(item, QPoint(200, 200), QRect(300, 100, 200, 300), theme);
        QVERIFY(popup.is_open());
        QCOMPARE(popup.editor->toPlainText(), QString("body"));
        QVERIFY(!popup.format_hint->text().isEmpty());
        popup.editor->setPlainText("changed");
        QTest::keyClick(popup.editor, Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(finished.size(), 1);
        auto result = finished.last().first().value<QuickEditResult>();
        QCOMPARE(result.item_id, qint64(5));
        QCOMPARE(result.text.value_or(QString()), QString("changed"));
        QVERIFY(!result.paste);
        QVERIFY(!popup.is_open());

        item.html_content.reset();
        popup.open_for(item, QPoint(200, 200), QRect(), theme);
        QVERIFY(popup.format_hint->text().isEmpty());
        QTest::keyClick(popup.editor, Qt::Key_Return, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(finished.last().first().value<QuickEditResult>().paste);

        popup.open_for(item, QPoint(200, 200), QRect(), theme);
        QTest::keyClick(popup.editor, Qt::Key_Escape);
        QVERIFY(!finished.last().first().value<QuickEditResult>().text);

        popup.open_for(item, QPoint(200, 200), QRect(), theme);
        popup.save_and_paste_button->click();
        QVERIFY(finished.last().first().value<QuickEditResult>().paste);
        popup.open_for(item, QPoint(200, 200), QRect(), theme);
        popup.save_button->click();
        QVERIFY(!finished.last().first().value<QuickEditResult>().paste);
        // Focus leaving keeps the text and says so.
        popup.open_for(item, QPoint(200, 200), QRect(), theme);
        popup.close_for_focus_loss();
        QVERIFY(finished.last().first().value<QuickEditResult>().focus_left);
        // Finishing twice reports once.
        const int count = finished.size();
        popup.cancel();
        QCOMPARE(finished.size(), count);
    }

    void theGearMenuReportsEveryChoice() {
        QWidget parent;
        QStringList calls;
        SettingMenuOptions options;
        options.tr = [](const char *text) { return QString::fromUtf8(text); };
        options.paste_with_html = true;
        options.auto_paste = false;
        options.window_opacity = 20;
        options.current_font_size = 15;
        options.current_display_lines = 2;
        options.current_image_size = "medium";
        options.current_theme_name = "light";
        options.current_group_bar_position = "left";
        options.opacity_options = {0, 20};
        options.font_size_options = {15, 17};
        options.on_toggle_paste_html = [&calls](bool checked) { calls << QString("html %1").arg(checked); };
        options.on_toggle_auto_paste = [&calls](bool checked) { calls << QString("auto %1").arg(checked); };
        options.on_toggle_close_after_paste = [&calls](bool) { calls << "close"; };
        options.on_toggle_move_to_top = [&calls](bool) { calls << "top"; };
        options.on_toggle_show_metadata = [&calls](bool) { calls << "meta"; };
        options.on_toggle_preserve_search = [&calls](bool) { calls << "search"; };
        options.on_set_opacity = [&calls](int value) { calls << QString("opacity %1").arg(value); };
        options.on_set_font_size = [&calls](int value) { calls << QString("font %1").arg(value); };
        options.on_set_display_lines = [&calls](int value) { calls << QString("lines %1").arg(value); };
        options.on_set_image_size = [&calls](const QString &value) { calls << "image " + value; };
        options.on_set_theme = [&calls](const QString &value) { calls << "theme " + value; };
        options.on_add_item = [&calls] { calls << "add"; };
        options.on_clear_history = [&calls] { calls << "clear"; };
        options.on_set_group_bar_position = [&calls](const QString &value) { calls << "bar " + value; };
        options.open_position = "previous";
        options.on_set_open_position = [&calls](const QString &value) { calls << "position " + value; };
        options.multi_paste_separator = " | ";
        options.multi_paste_order = "newest_first";
        options.multi_paste_keep_merged = true;
        options.on_set_multi_paste_separator = [&calls](const QString &value) { calls << "separator " + escape_separator(value); };
        options.on_set_multi_paste_order = [&calls](const QString &value) { calls << "order " + value; };
        options.on_toggle_multi_paste_keep_merged = [&calls](bool checked) { calls << QString("keep %1").arg(checked); };
        QPointer<QMenu> menu = show_setting_menu(&parent, options);
        QVERIFY(menu->isVisible());
        QHash<QString, QAction *> actions;
        for (auto *action : menu->findChildren<QAction *>()) actions.insert(action->text(), action);
        // Current values are checked.
        QVERIFY(actions["Paste with Format"]->isChecked());
        QVERIFY(!actions["Auto Paste After Selection"]->isChecked());
        QVERIFY(actions["20%"]->isChecked());
        QVERIFY(actions["15px"]->isChecked());
        QVERIFY(actions["Medium"]->isChecked());
        QAction *three_lines = nullptr;
        for (auto *submenu : menu->findChildren<QMenu *>()) {
            if (submenu->title() != "Max Display Lines") continue;
            QCOMPARE(submenu->actions().size(), 5);
            QVERIFY(submenu->actions()[1]->isChecked());
            three_lines = submenu->actions()[2];
        }
        QVERIFY(three_lines);
        three_lines->trigger();
        QVERIFY(calls.contains("lines 3"));
        QVERIFY(actions["Left"]->isChecked());
        QVERIFY(actions["Last Position"]->isChecked());
        QVERIFY(!actions["Follow Mouse"]->isChecked());
        QVERIFY(actions["Last Selected First"]->isChecked());
        QVERIFY(actions["Record Content"]->isChecked());
        // A custom separator shows in the field; no preset is checked.
        auto *custom = menu->findChild<LineEdit *>();
        QVERIFY(custom);
        QCOMPARE(custom->text(), QString(" | "));
        for (const char *label : {"New Line", "Comma", "No Separator"}) QVERIFY(!actions[label]->isChecked());
        for (const char *label : {"Paste with Format", "Auto Paste After Selection", "Close After Paste", "Move to Top After Paste", "Show Time",
                                  "Preserve Search on Reopen", "Opaque", "17px", "Large", "First Selected First", "Record Content", "Comma", "Add Content",
                                  "Clear History", "Follow Mouse"})
            actions[label]->trigger();
        actions["Top"]->trigger();
        QVERIFY(calls.contains("html 0"));
        QVERIFY(calls.contains("auto 1"));
        QVERIFY(calls.contains("opacity 0"));
        QVERIFY(calls.contains("font 17"));
        QVERIFY(calls.contains("image large"));
        QVERIFY(calls.contains("order oldest_first"));
        QVERIFY(calls.contains("keep 0"));
        QVERIFY(calls.contains("separator , "));
        QVERIFY(calls.contains("add"));
        QVERIFY(calls.contains("clear"));
        QVERIFY(calls.contains("bar top"));
        QVERIFY(calls.contains("position cursor"));
        // Theme swatches are buttons inside the menu; the current one is marked by an icon, not a ✓ character.
        QPushButton *swatch = nullptr;
        int marked = 0;
        for (auto *button : menu->findChildren<QPushButton *>()) {
            swatch = button;
            QVERIFY(button->text().isEmpty());
            if (!button->icon().isNull()) ++marked;
        }
        QVERIFY(swatch);
        QCOMPARE(marked, 1);
        swatch->click();
        QVERIFY(calls.last().startsWith("theme "));
        // The custom separator understands escapes and closes the menu.
        custom->setText("\\n--\\n");
        emit custom->returnPressed();
        QVERIFY(calls.contains("separator \\n--\\n"));
        QTRY_VERIFY(!menu);
    }

    void themesAndTheirStyles() {
        auto *manager = get_theme_manager();
        QSignalSpy changed(manager, &ThemeManager::theme_changed);
        const QString original = manager->get_current_theme().name;
        QVERIFY(!manager->set_theme("no such theme"));
        QCOMPARE(changed.size(), 0);
        QVERIFY(!preset_theme("no such theme"));
        QCOMPARE(THEME_LIGHT().name, QString("light"));
        QCOMPARE(PRESET_THEME_SWATCHES().size(), PRESET_THEMES().size());
        for (const auto &[name, theme] : PRESET_THEMES()) {
            QVERIFY(manager->set_theme(name));
            QCOMPARE(env_.config->get_clipboard_theme(), name);
            const ThemeStyleGenerator styles(theme);
            for (const QString &css : {styles.generate_window_style(), styles.generate_window_style(50), styles.generate_list_widget_style(30),
                                       styles.generate_search_bar_style(30), styles.generate_time_filter_bar_style(30), styles.generate_quick_edit_style(),
                                       styles.generate_menu_style(), styles.generate_search_box_style(true), styles.generate_search_box_style(false),
                                       styles.generate_clear_search_btn_style(), styles.generate_filter_toggle_btn_style(),
                                       styles.generate_time_filter_date_edit_style(), styles.generate_time_filter_calendar_style(),
                                       styles.generate_time_filter_action_btn_style(true), styles.generate_time_filter_action_btn_style(false),
                                       styles.generate_time_filter_type_btn_style(), styles.generate_menu_btn_style()})
                QVERIFY2(css.contains("{") && css.contains("}"), qPrintable(name));
            // The empty search field keeps a faint base so it reads as an input.
            QVERIFY2(styles.generate_search_box_style(false).contains("rgba(" + ThemeStyleGenerator::hex_to_rgb(theme.colors.text_primary)), qPrintable(name));
        }
        QVERIFY(!changed.isEmpty());
        QCOMPARE(ThemeStyleGenerator::hex_to_rgb("#FF8000"), QString("255, 128, 0"));
        QVERIFY(manager->set_theme(original));
    }

    void foregroundTrackingSkipsThisProcess() {
        QVERIFY(!is_alive(0));
        QVERIFY(!get_window_pid(0));
        QVERIFY(get_window_class(0).isEmpty());
        QCOMPARE(get_current_pid(), static_cast<quint32>(QCoreApplication::applicationPid()));
        if (const Hwnd foreground = get_foreground_hwnd()) {
            QVERIFY(is_alive(foreground));
            QVERIFY(get_window_pid(foreground));
        }
        ForegroundWindowTracker tracker;
        tracker.set_excluded([](Hwnd) { return true; });
        tracker.start();
        // Everything excluded: no target.
        QCOMPARE(tracker.target_hwnd(), Hwnd(0));
        QVERIFY(!tracker.sample());
        tracker.stop();
        tracker.set_excluded({});
        tracker.sample();
        tracker.stop();
    }

    void theSpecialPasteEditorHasATabPerKind() {
        SpecialPasteDialog editor(special_paste::default_entries());
        editor.show();
        QVERIFY(editor.tabs);
        editor.tabs->setCurrentItem("file");
        QVERIFY(!editor.grab().isNull());
        editor.tabs->setCurrentItem("text");
        QCOMPARE(editor.entries(), special_paste::default_entries());
    }

    void theSingleFileSwitchFollowsTheDatabaseAndRevertsWhenItCannotSwitch() {
        auto &cm = ClipboardManager::instance();
        QVERIFY(!cm.single_file());
        SettingsDialog dialog(env_.config, "");
        dialog.show();
        dialog.on_nav_changed(2, "clipboard");
        auto *card = dialog.attr<SwitchSettingCard>("_clipboard_single_file_toggle");
        QVERIFY(card && card->isEnabled());
        QVERIFY(!card->isChecked());

        // Another connection keeps the database in WAL: the switch goes back and says why.
        const QByteArray path = cm.get_db_path().value_or(QString()).toUtf8();
        char error[256] = {};
        JbClipboard *other = jb_clipboard_open(path.constData(), error, sizeof error);
        QVERIFY2(other, error);
        {
            ModalAnswerer ok(true);
            card->setChecked(true);
            QTRY_COMPARE(ok.answered(), 1);
            QVERIFY(ok.texts.first().contains("another program"));
        }
        jb_clipboard_free(other);
        QVERIFY(!card->isChecked());
        QVERIFY(!cm.single_file());

        card->setChecked(true);
        QVERIFY(cm.single_file());
        QVERIFY(cm.add_item("one file"));
        QVERIFY(!QFileInfo::exists(QString::fromUtf8(path) + "-wal"));
        card->setChecked(false);
        QVERIFY(!cm.single_file());
    }

    void theSettingsPageMovesClearsAndOpensTheData() {
        const auto group = ClipboardManager::instance().create_group("Keep");
        const auto ids = add_texts({"loose", "grouped", "pinned"});
        QVERIFY(ClipboardManager::instance().move_to_group(ids[1], *group));
        QVERIFY(ClipboardManager::instance().toggle_pin(ids[2]));
        SettingsDialog dialog(env_.config, "");
        dialog.show();
        dialog.on_nav_changed(2, "clipboard");
        auto button = [&dialog](const char *text) -> QPushButton * {
            for (auto *candidate : dialog.findChildren<QPushButton *>())
                if (candidate->text() == settings_tr(text) && candidate->isVisible()) return candidate;
            return nullptr;
        };
        QVERIFY(button("Change") && button("Open") && button("Clear History"));

        // The size is measured when the page opens, without a button.
        auto *size_label = dialog.attr<QLabel>("_clipboard_size_label");
        QVERIFY(size_label);
        QVERIFY(!dialog.has_attr("_clipboard_refresh_btn"));
        QTRY_VERIFY(size_label->text().contains("B"));

        // Opening shows the data folder.
        button("Open")->click();
        QTRY_COMPARE(urls_.urls.size(), 1);
        QCOMPARE(QDir::cleanPath(urls_.urls.first().toLocalFile()), QDir::cleanPath(env_.db_folder()));

        // The same folder, and a folder inside the images folder, are refused.
        {
            FileDialogAnswerer same(env_.db_folder());
            ModalAnswerer ok(true);
            button("Change")->click();
            QVERIFY(same.answered);
            QCOMPARE(ok.answered(), 1);
            QVERIFY(ok.texts.first().contains("already the current location"));
        }
        const QString images = ClipboardManager::instance().get_images_dir().value_or(QString());
        QDir().mkpath(QDir(images).filePath("inner"));
        {
            FileDialogAnswerer inside(QDir(images).filePath("inner"));
            ModalAnswerer ok(true);
            button("Change")->click();
            QCOMPARE(ok.answered(), 1);
            QVERIFY(ok.texts.first().contains("outside"));
        }

        // Moving copies the database, reopens it there and removes the old files.
        const QString old_db = ClipboardManager::instance().get_db_path().value_or(QString());
        const QString target = QDir::toNativeSeparators(env_.dir.filePath("moved"));
        // A repeated run (check.ps1 -Mode Leak) would find the first run's database there and be asked to replace it.
        QVERIFY(QDir(target).removeRecursively());
        QDir().mkpath(target);
        {
            FileDialogAnswerer move(target);
            button("Change")->click();
            QVERIFY(move.answered);
        }
        const QString new_db = QDir::toNativeSeparators(QDir(target).filePath("clipboard.db"));
        QTRY_COMPARE_WITH_TIMEOUT(env_.config->get_clipboard_db_path(), new_db, 10000);
        QTRY_COMPARE(ClipboardManager::instance().get_db_path().value_or(QString()), new_db);
        QVERIFY(!QFileInfo::exists(old_db));
        QCOMPARE(contents_of(ClipboardManager::instance().get_history()).size(), 3);
        QCOMPARE(dialog.attr<SettingCard>("_clipboard_storage_card")->contentLabel->text(), new_db);

        // A folder holding clipboard data asks first; cancelling keeps everything.
        const QString occupied = QDir::toNativeSeparators(env_.dir.filePath("occupied"));
        QDir().mkpath(occupied);
        QFile stale(QDir(occupied).filePath("clipboard.db"));
        QVERIFY(stale.open(QIODevice::WriteOnly));
        stale.close();
        {
            FileDialogAnswerer pick(occupied);
            ModalAnswerer no(false);
            button("Change")->click();
            QCOMPARE(no.answered(), 1);
        }
        QCOMPARE(ClipboardManager::instance().get_db_path().value_or(QString()), new_db);

        // Using the data already there switches to it and leaves both folders as they are.
        const QString other = QDir::toNativeSeparators(env_.dir.filePath("other"));
        const QString other_db = QDir::toNativeSeparators(QDir(other).filePath("clipboard.db"));
        QVERIFY(QDir(other).removeRecursively());
        QVERIFY(ClipboardManager::instance().reset_storage(other_db));
        add_texts({"elsewhere"});
        QVERIFY(ClipboardManager::instance().reset_storage(new_db));
        {
            FileDialogAnswerer pick(other);
            ModalAnswerer use(settings_tr("Use That Data"));
            button("Change")->click();
            QCOMPARE(use.answered(), 1);
        }
        QCOMPARE(ClipboardManager::instance().get_db_path().value_or(QString()), other_db);
        QCOMPARE(env_.config->get_clipboard_db_path(), other_db);
        QCOMPARE(contents_of(ClipboardManager::instance().get_history()), QStringList{"elsewhere"});
        QVERIFY(QFileInfo::exists(new_db));
        QCOMPARE(dialog.attr<SettingCard>("_clipboard_storage_card")->contentLabel->text(), other_db);

        // Data that cannot be opened keeps the current database.
        const QString broken = QDir::toNativeSeparators(env_.dir.filePath("broken"));
        QDir().mkpath(broken);
        QFile garbage(QDir(broken).filePath("clipboard.db"));
        QVERIFY(garbage.open(QIODevice::WriteOnly));
        garbage.write(QByteArray(4096, 'x'));
        garbage.close();
        {
            FileDialogAnswerer pick(broken);
            ModalAnswerer use(settings_tr("Use That Data"));
            button("Change")->click();
            QCOMPARE(use.answered(), 2);
            QVERIFY(use.texts.last().contains(settings_tr("Cannot open the clipboard data in the selected folder. The current data is still in use.")));
        }
        QCOMPARE(ClipboardManager::instance().get_db_path().value_or(QString()), other_db);
        QCOMPARE(env_.config->get_clipboard_db_path(), other_db);
        QCOMPARE(contents_of(ClipboardManager::instance().get_history()), QStringList{"elsewhere"});

        // Replacing moves the current data over the folder's own and removes it from the old place.
        const QString occupied_db = QDir::toNativeSeparators(QDir(occupied).filePath("clipboard.db"));
        {
            FileDialogAnswerer pick(occupied);
            ModalAnswerer replace(settings_tr("Replace"));
            button("Change")->click();
            QCOMPARE(replace.answered(), 1);
        }
        QTRY_COMPARE_WITH_TIMEOUT(env_.config->get_clipboard_db_path(), occupied_db, 10000);
        QTRY_COMPARE(ClipboardManager::instance().get_db_path().value_or(QString()), occupied_db);
        QCOMPARE(contents_of(ClipboardManager::instance().get_history()), QStringList{"elsewhere"});
        QVERIFY(!QFileInfo::exists(other_db));

        // Back to the moved data for the rest of the test.
        {
            FileDialogAnswerer pick(target);
            ModalAnswerer use(true);
            button("Change")->click();
            QCOMPARE(use.answered(), 1);
        }
        QCOMPARE(ClipboardManager::instance().get_db_path().value_or(QString()), new_db);

        // Clearing asks and keeps pinned and grouped items unless the box is ticked.
        {
            ModalAnswerer yes(true);
            button("Clear History")->click();
            QTRY_COMPARE(yes.answered(), 2);
        }
        QCOMPARE(contents_of(ClipboardManager::instance().get_history()), (QStringList{"pinned", "grouped"}));
        {
            ModalAnswerer yes(true, true);
            button("Clear History")->click();
            QTRY_COMPARE(yes.answered(), 2);
        }
        QVERIFY(ClipboardManager::instance().get_history().isEmpty());
        {
            ModalAnswerer no(false);
            button("Clear History")->click();
            QCOMPARE(no.answered(), 1);
        }
        dialog.hide();
    }
};

QTEST_MAIN(ClipboardWidgetTests)
#include "test_clipboard_widgets.moc"
