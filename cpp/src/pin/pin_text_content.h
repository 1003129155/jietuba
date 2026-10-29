#pragma once
#include "pin/ocr_text_layout.h"
#include <QImage>
#include <QPixmap>
#include <QSize>
#include <QString>
#include <functional>
#include <memory>
#include <utility>

class QObject;
class QTextDocument;

namespace jietuba {
// What a text pin shows: the text laid out on a white page as wide as its longest line, up to a
// limit past which lines wrap. The page is the pin's image, drawn afresh at whatever size the pin
// shows it, so zooming keeps the text sharp; the layout gives each character's place for selecting.
// Laying out and drawing a long text take a while, so the pin has both done on a worker thread.
class PinTextContent {
public:
    // Formatted, the text as formatted in html; otherwise html is kept to switch to. Formatting
    // that leaves no text of its own, such as a lone picture, is never shown.
    PinTextContent(const QString &text, const QString &html, bool formatted);
    ~PinTextContent();
    // The same made on a worker thread; done gets it on context's thread, and not at all once context is
    // gone. With page_pixmap the page is made into a pixmap there too, for take_page_pixmap().
    static void build_async(const QString &text, const QString &html, bool formatted, QObject *context, std::function<void(std::unique_ptr<PinTextContent>)> done,
                            bool page_pixmap = false);
    // The page at 100%, and drawn at that size when laid out, until the pin takes it.
    QSize size() const { return size_; }
    QImage take_page() { return std::exchange(page_, QImage()); }
    // The page made into a pixmap on the worker thread by build_async, once; null otherwise.
    QPixmap take_page_pixmap() { return std::exchange(page_pixmap_, QPixmap()); }
    // The page stretched to size, in pixels of the given ratio.
    QImage render(const QSize &size, double device_pixel_ratio = 1.0) const;
    // The same drawn on a worker thread from its own copy of the text. Only the latest request is
    // drawn: done gets its image on context's thread; earlier ones still waiting are dropped.
    void render_async(const QSize &size, double device_pixel_ratio, QObject *context, std::function<void(QImage)> done) const;
    // Every visible line with its characters' places, in page pixels; copying joins the lines as
    // the text runs, not by where they sit.
    const OcrTextLayout &layout() const { return layout_; }
    // As copied.
    QString text() const { return text_; }
    // The formatting to show or switch to; empty when there is none.
    QString source_html() const { return html_; }
    // As shown; empty unless formatted.
    QString html() const;
    bool formatted() const { return formatted_; }
    bool has_formatting() const { return !html_.isEmpty(); }
    // The text as laid out on the page.
    const QTextDocument *document() const { return document_.get(); }
private:
    struct Source;
    QString text_, html_;
    bool formatted_ = false;
    std::unique_ptr<QTextDocument> document_;
    QSize size_;
    OcrTextLayout layout_;
    QImage page_;
    QPixmap page_pixmap_;
    std::shared_ptr<Source> source_;
};
}
