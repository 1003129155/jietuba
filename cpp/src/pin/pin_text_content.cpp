#include "pin/pin_text_content.h"
#include "pin/ocr_text_layout.h"
#include <QAbstractTextDocumentLayout>
#include <QCoreApplication>
#include <QPainter>
#include <QPointer>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextLayout>
#include <QTextTable>
#include <QThread>
#include <QThreadPool>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <optional>
#include <vector>

namespace jietuba {
namespace {
const QColor PAGE_COLOR(255, 255, 255);
const QColor TEXT_COLOR(31, 31, 31);
// Body-text size, lines wrapping past the width limit; the margin is the page's own, outside the
// document's 4 pixels, so text sits 10 pixels in and the blank around it drags the pin.
constexpr int PIXEL_SIZE = 16, MAX_TEXT_WIDTH = 800, PAGE_MARGIN = 6;

// Code keeps a fixed-pitch face; Qt's own is Courier New.
const char RICH_STYLE[] = "code, pre, tt, kbd, samp { font-family: Consolas, 'Courier New', monospace; }";
// A copied background stays when the page's text reads on it, a copied text color when it reads on
// its background: text from a dark page turns dark on the white page, colored text keeps its color.
constexpr double KEPT_BACKGROUND_CONTRAST = 4.5, KEPT_TEXT_CONTRAST = 2.0;

// WCAG relative luminance and contrast ratio.
double luminance(const QColor &color) {
    const auto linear = [](double c) { return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
    return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) + 0.0722 * linear(color.blueF());
}

double contrast(const QColor &a, const QColor &b) {
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// A solid brush's color as it shows over under; nullopt for anything else.
std::optional<QColor> shown_color(const QBrush &brush, const QColor &under) {
    if (brush.style() != Qt::SolidPattern) return std::nullopt;
    const QColor color = brush.color();
    const float alpha = color.alphaF();
    return QColor::fromRgbF(color.redF() * alpha + under.redF() * (1 - alpha), color.greenF() * alpha + under.greenF() * (1 - alpha),
                            color.blueF() * alpha + under.blueF() * (1 - alpha));
}

// Clears a background the page's text would not read on; returns what shows behind the text.
QColor keep_readable_background(QTextFormat &format, const QColor &under) {
    if (!format.hasProperty(QTextFormat::BackgroundBrush)) return under;
    const auto color = shown_color(format.background(), under);
    if (color && contrast(*color, TEXT_COLOR) >= KEPT_BACKGROUND_CONTRAST) return *color;
    format.clearBackground();
    return under;
}

// Clears a background or text color that would not read; returns whether anything changed.
bool keep_readable_colors(QTextCharFormat &format, const QColor &under) {
    const auto before = format;
    const QColor background = keep_readable_background(format, under);
    if (format.hasProperty(QTextFormat::ForegroundBrush)) {
        const auto color = shown_color(format.foreground(), background);
        if (!color || contrast(*color, background) < KEPT_TEXT_CONTRAST) format.clearForeground();
    }
    return format != before;
}

// What shows behind a block's text: its table cell's background over the page.
QColor cell_background(const QTextCursor &cursor) {
    auto *table = cursor.currentTable();
    if (!table) return PAGE_COLOR;
    const auto format = table->cellAt(cursor).format();
    if (!format.hasProperty(QTextFormat::BackgroundBrush)) return PAGE_COLOR;
    return shown_color(format.background(), PAGE_COLOR).value_or(PAGE_COLOR);
}

// Copied formatting as it can show on the white page: images go, since the pin loads none;
// unreadable colors go; lines kept from wrapping wrap with the page.
void tidy_rich_text(QTextDocument &document) {
    auto root = document.rootFrame()->frameFormat();
    root.clearBackground();
    document.rootFrame()->setFrameFormat(root);
    const std::function<void(QTextFrame *)> tidy_frames = [&tidy_frames](QTextFrame *frame) {
        for (auto *child : frame->childFrames()) {
            auto format = child->frameFormat();
            if (format.hasProperty(QTextFormat::BackgroundBrush)) {
                keep_readable_background(format, PAGE_COLOR);
                child->setFrameFormat(format);
            }
            if (auto *table = qobject_cast<QTextTable *>(child)) {
                for (int row = 0; row < table->rows(); ++row)
                    for (int column = 0; column < table->columns(); ++column) {
                        auto cell = table->cellAt(row, column);
                        // A spanned position reports the cell it belongs to.
                        if (cell.row() != row || cell.column() != column) continue;
                        auto cell_format = cell.format();
                        if (!cell_format.hasProperty(QTextFormat::BackgroundBrush)) continue;
                        keep_readable_background(cell_format, PAGE_COLOR);
                        cell.setFormat(cell_format);
                    }
            }
            tidy_frames(child);
        }
    };
    tidy_frames(document.rootFrame());

    struct Change {
        int position, length;
        QTextCharFormat format;
    };
    std::vector<Change> changes;
    std::vector<std::pair<int, int>> images;
    for (auto block = document.begin(); block.isValid(); block = block.next()) {
        QTextCursor cursor(block);
        auto block_format = block.blockFormat();
        const auto before = block_format;
        block_format.clearProperty(QTextFormat::BlockNonBreakableLines);
        const QColor under = keep_readable_background(block_format, cell_background(cursor));
        if (block_format != before) cursor.setBlockFormat(block_format);
        // List bullets take the block's character format.
        if (auto format = block.charFormat(); keep_readable_colors(format, under)) cursor.setBlockCharFormat(format);
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            auto format = fragment.charFormat();
            if (format.isImageFormat()) images.emplace_back(fragment.position(), fragment.length());
            else if (keep_readable_colors(format, under)) changes.push_back({fragment.position(), fragment.length(), format});
        }
    }
    QTextCursor cursor(&document);
    for (const auto &change : changes) {
        cursor.setPosition(change.position);
        cursor.setPosition(change.position + change.length, QTextCursor::KeepAnchor);
        cursor.setCharFormat(change.format);
    }
    for (auto it = images.rbegin(); it != images.rend(); ++it) {
        cursor.setPosition(it->first);
        cursor.setPosition(it->first + it->second, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
    }
}

// The text laid out unwrapped; true when it keeps html's formatting. Formatting that leaves no
// text falls back to the plain text.
bool load_text(QTextDocument &document, const QString &text, const QString &html) {
    QFont font("Microsoft YaHei UI");
    font.setPixelSize(PIXEL_SIZE);
    document.setDefaultFont(font);
    auto option = document.defaultTextOption();
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    document.setDefaultTextOption(option);
    if (!html.isEmpty()) {
        document.setDefaultStyleSheet(RICH_STYLE);
        document.setHtml(html);
        tidy_rich_text(document);
        if (!document.toPlainText().trimmed().isEmpty()) return true;
    }
    document.setPlainText(text);
    return false;
}

// One visible line: its characters' document positions and edges in page pixels.
struct LaidLine {
    QTextBlock block;
    QString text;
    // Block positions of the first character and just past the last.
    int start = 0, end = 0;
    std::vector<std::pair<double, double>> edges;
    double top = 0, bottom = 0;
};

// What joins two lines as the text runs: nothing within a wrapped paragraph, a tab between cells
// of a table row, else a newline for each paragraph ended.
QString line_break(const LaidLine &from, const LaidLine &to) {
    if (from.block == to.block) {
        auto between = from.block.text().mid(from.end, to.start - from.end);
        between.replace(QChar::LineSeparator, '\n');
        between.remove(QChar::ObjectReplacementCharacter);
        return between;
    }
    auto *table = QTextCursor(from.block).currentTable();
    if (table && table == QTextCursor(to.block).currentTable()) {
        const auto a = table->cellAt(from.block.position()), b = table->cellAt(to.block.position());
        if (a.row() == b.row() && a.column() != b.column()) return "\t";
    }
    int paragraphs = 1;
    for (auto block = from.block.next(); block.isValid() && block != to.block; block = block.next()) ++paragraphs;
    return QString("\n").repeated(paragraphs);
}

// The document wrapped as on the page, and the width it wraps at.
int wrap_page(QTextDocument &document) {
    const int width = std::min(static_cast<int>(std::ceil(document.size().width())), MAX_TEXT_WIDTH);
    document.setTextWidth(width);
    return width;
}

// The page of document stretched to size, in pixels of the given ratio.
QImage draw_page(const QTextDocument &document, const QSize &page, const QSize &size, double device_pixel_ratio) {
    QImage image(std::max(1, static_cast<int>(std::lround(size.width() * device_pixel_ratio))),
                 std::max(1, static_cast<int>(std::lround(size.height() * device_pixel_ratio))), QImage::Format_ARGB32_Premultiplied);
    image.fill(PAGE_COLOR);
    {
        // Enlarged through the pixel ratio, not a painter scale: fonts are then drawn at the larger size
        // as they are on a high-DPI screen, while a scaled painter draws glyphs as paths and loses
        // synthetic bold. The page's rounding across, under a pixel, is left blank.
        image.setDevicePixelRatio(static_cast<double>(image.width()) / page.width());
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.translate(PAGE_MARGIN, PAGE_MARGIN);
        QAbstractTextDocumentLayout::PaintContext context;
        context.palette.setColor(QPalette::Text, TEXT_COLOR);
        document.documentLayout()->draw(&painter, context);
    }
    image.setDevicePixelRatio(device_pixel_ratio);
    return image;
}

OcrTextLayout layout_of(const QTextDocument &document) {
    auto *document_layout = document.documentLayout();
    std::vector<LaidLine> lines;
    for (auto block = document.begin(); block.isValid(); block = block.next()) {
        const auto *block_layout = block.layout();
        if (!block_layout) continue;
        const QPointF origin = document_layout->blockBoundingRect(block).topLeft() + QPointF(PAGE_MARGIN, PAGE_MARGIN);
        const QString block_text = block.text();
        for (int index = 0; index < block_layout->lineCount(); ++index) {
            const auto line = block_layout->lineAt(index);
            LaidLine laid{block};
            laid.top = origin.y() + line.y();
            laid.bottom = laid.top + line.height();
            const int last = line.textStart() + line.textLength();
            for (int position = line.textStart(); position < last;) {
                const int units = block_text.at(position).isHighSurrogate() && position + 1 < last ? 2 : 1;
                const QChar c = block_text.at(position);
                if (c != QChar::LineSeparator && c != QChar::ObjectReplacementCharacter) {
                    if (laid.edges.empty()) laid.start = position;
                    laid.end = position + units;
                    laid.text += block_text.mid(position, units);
                    laid.edges.emplace_back(origin.x() + line.cursorToX(position), origin.x() + line.cursorToX(position + units));
                }
                position += units;
            }
            if (!laid.edges.empty()) lines.push_back(std::move(laid));
        }
    }
    OcrResult result;
    for (const auto &line : lines) {
        double left = line.edges.front().first, right = line.edges.front().second;
        for (const auto &[a, b] : line.edges) {
            left = std::min({left, a, b});
            right = std::max({right, a, b});
        }
        const double width = std::max(1.0, right - left);
        OcrLine source{{QPointF(left, line.top), QPointF(right, line.top), QPointF(right, line.bottom), QPointF(left, line.bottom)}, line.text, 1.0};
        for (const auto &[a, b] : line.edges) {
            const auto start = static_cast<float>((std::min(a, b) - left) / width), end = static_cast<float>((std::max(a, b) - left) / width);
            source.chars.append({start, (start + end) / 2, end});
        }
        result.data.append(source);
    }
    OcrTextLayout layout(result);
    if (layout.lines().size() == lines.size() && lines.size() > 1) {
        std::vector<QString> breaks;
        for (size_t index = 0; index + 1 < lines.size(); ++index) breaks.push_back(line_break(lines[index], lines[index + 1]));
        layout.set_flow(std::move(breaks));
    }
    return layout;
}

// Below the UI thread, so neither the pins nor other programs wait for it.
QThreadPool &workers() {
    static QThreadPool pool;
    static const bool ready = [] {
        pool.setMaxThreadCount(2);
        pool.setThreadPriority(QThread::LowPriority);
        // Drawing text ends before the application takes the fonts down.
        if (qApp) QObject::connect(qApp, &QCoreApplication::aboutToQuit, qApp, [] {
            pool.clear();
            pool.waitForDone();
        });
        return true;
    }();
    Q_UNUSED(ready);
    return pool;
}

// Runs done on the application's thread while context lives.
template <typename Value> void deliver(const QPointer<QObject> &context, std::function<void(Value)> done, std::shared_ptr<Value> value) {
    QMetaObject::invokeMethod(qApp, [context, done = std::move(done), value] {
        if (context) done(std::move(*value));
    }, Qt::QueuedConnection);
}
}

// What the worker lays out again to draw the page, so it never shares a document with the UI thread.
struct PinTextContent::Source {
    QString text, html;
    int text_width = 0;
    QSize page;
    // Requests older than this one are dropped.
    std::atomic<quint64> latest{0};
};

PinTextContent::PinTextContent(const QString &text, const QString &html, bool formatted) : text_(text), html_(html), formatted_(formatted) {
    if (!html_.isEmpty() && !formatted_) {
        QTextDocument probe;
        if (!load_text(probe, text_, html_)) html_.clear();
    }
    document_ = std::make_unique<QTextDocument>();
    const bool rich = load_text(*document_, text_, formatted_ ? html_ : QString());
    if (formatted_ && !rich) html_.clear();
    formatted_ = rich;
    const int width = wrap_page(*document_);
    size_ = QSize(width + 2 * PAGE_MARGIN, static_cast<int>(std::ceil(document_->size().height())) + 2 * PAGE_MARGIN);
    layout_ = layout_of(*document_);
    page_ = draw_page(*document_, size_, size_, 1.0);
    source_ = std::make_shared<Source>();
    source_->text = text_;
    source_->html = formatted_ ? html_ : QString();
    source_->text_width = width;
    source_->page = size_;
}

PinTextContent::~PinTextContent() {
    // Drawing still waiting for this text is no longer wanted.
    ++source_->latest;
}

void PinTextContent::build_async(const QString &text, const QString &html, bool formatted, QObject *context, std::function<void(std::unique_ptr<PinTextContent>)> done,
                                 bool page_pixmap) {
    QThread *const target = context->thread();
    workers().start([text, html, formatted, page_pixmap, target, context = QPointer<QObject>(context), done = std::move(done)] {
        auto content = std::make_shared<std::unique_ptr<PinTextContent>>(std::make_unique<PinTextContent>(text, html, formatted));
        // Converting a long page takes the UI thread a frame or more.
        if (page_pixmap) (*content)->page_pixmap_ = QPixmap::fromImage((*content)->page_);
        (*content)->document_->moveToThread(target);
        deliver<std::unique_ptr<PinTextContent>>(context, done, content);
    });
}

QString PinTextContent::html() const { return formatted_ ? document_->toHtml() : QString(); }

QImage PinTextContent::render(const QSize &size, double device_pixel_ratio) const { return draw_page(*document_, size_, size, device_pixel_ratio); }

void PinTextContent::render_async(const QSize &size, double device_pixel_ratio, QObject *context, std::function<void(QImage)> done) const {
    const auto source = source_;
    const quint64 ticket = ++source->latest;
    workers().start([source, ticket, size, device_pixel_ratio, context = QPointer<QObject>(context), done = std::move(done)] {
        if (source->latest != ticket) return;
        QTextDocument document;
        load_text(document, source->text, source->html);
        document.setTextWidth(source->text_width);
        if (source->latest != ticket) return;
        deliver<QImage>(context, done, std::make_shared<QImage>(draw_page(document, source->page, size, device_pixel_ratio)));
    });
}
}
