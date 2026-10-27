#pragma once
#include <QImage>
#include <QSize>
#include <QString>
#include <memory>

class QTextDocument;

namespace jietuba {
class OcrTextLayout;

// What a text pin shows: the text laid out on a white page as wide as its longest line, up to a
// limit past which lines wrap. The page is the pin's image, drawn afresh at whatever size the pin
// shows it, so zooming keeps the text sharp; the layout gives each character's place for selecting.
class PinTextContent {
public:
    // Formatted, the text as formatted in html; otherwise html is kept to switch to. Formatting
    // that leaves no text of its own, such as a lone picture, is never shown.
    PinTextContent(const QString &text, const QString &html, bool formatted);
    ~PinTextContent();
    // The page at 100%.
    QSize size() const { return size_; }
    // The page stretched to size, in pixels of the given ratio.
    QImage render(const QSize &size, double device_pixel_ratio = 1.0) const;
    // Every visible line with its characters' places, in page pixels; copying joins the lines as
    // the text runs, not by where they sit.
    OcrTextLayout layout() const;
    // As shown.
    QString text() const;
    // Empty unless formatted.
    QString html() const;
    bool formatted() const { return formatted_; }
    bool has_formatting() const { return !html_.isEmpty(); }
    // The text as laid out on the page.
    const QTextDocument *document() const { return document_.get(); }
    // Lays the text out again, plain or formatted, at the width a new pin of it would have.
    void set_formatted(bool formatted);
private:
    void lay_out();
    QString text_, html_;
    bool formatted_ = false;
    std::unique_ptr<QTextDocument> document_;
    QSize size_;
};
}
