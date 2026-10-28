#pragma once
#include <QImage>
#include <QRect>
#include <QSize>
#include <QStringList>
#include <QVector>

class QPainter;

namespace jietuba {
// Copied files shown as a grid of cards: system icon, name and size. Only the cells that fit
// within the size limit are laid out; when the files do not fit, the last cell counts them all.
// The icons are fetched once, large enough to stay sharp when the pin is zoomed, and kept: a
// program's icon is read from the file itself, which takes milliseconds each.
class PinFileCard {
public:
    static constexpr int CELL_WIDTH = 250;
    static constexpr int CELL_HEIGHT = 58;
    static constexpr int ICON_SIZE = 48;
    static constexpr int MARGIN = 6;
    static constexpr int ICON_TEXT_GAP = 8;
    static constexpr int NAME_PIXEL_SIZE = 13;
    static constexpr int SIZE_PIXEL_SIZE = 12;
    // Icons are kept at most this large: sharp at 200% or on a 2x screen, without holding the 256 px originals.
    static constexpr int ICON_KEPT_PIXELS = 96;
    PinFileCard(const QStringList &files, const QSize &max_size);
    const QStringList &files() const { return files_; }
    int columns() const { return columns_; }
    int rows() const { return rows_; }
    // Files drawn as cards; the rest are only counted.
    int shown() const { return shown_; }
    bool overflows() const { return shown_ < files_.size(); }
    QSize size() const { return QSize(columns_ * CELL_WIDTH, rows_ * CELL_HEIGHT); }
    QRect cell_rect(int index) const;
    // The file whose card holds pos (image coordinates); -1 between cards and on the count cell.
    int file_at(const QPointF &pos) const;
    // Fetches the shown files' icons; any thread, which then needs COM. Rendering fetches them
    // itself the first time when this was not done.
    void load_icons() const;
    // The cards stretched to size, in pixels of the given ratio; drawn afresh rather than resampled, so zooming stays sharp.
    QImage render(const QSize &size, double device_pixel_ratio = 1.0) const;
    // "1.25 MB"; bytes as a whole number.
    static QString format_size(qint64 bytes);
    // As files to paste into a folder or a chat.
    static void copy_files(const QStringList &files);
    // One native path per line.
    static void copy_paths(const QStringList &files);
    // Each folder opens once with its files selected; runs off the UI thread.
    static void reveal(const QStringList &files);
    static void open(const QString &file);
private:
    void paint_card(QPainter &painter, int index, const QImage &icon) const;
    void paint_count(QPainter &painter, const QRect &cell) const;
    QStringList files_;
    int columns_ = 1, rows_ = 1, shown_ = 0;
    // One per shown file once loaded; null where the system gives none.
    mutable QVector<QImage> icons_;
    mutable bool icons_loaded_ = false;
};
}
