#include "pin/pin_file_card.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "core/theme.h"
#include "ui/dialogs.h"
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QMap>
#include <QMimeData>
#include <QPainter>
#include <QTextLayout>
#include <QThread>
#include <QUrl>
#include <cmath>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <shellapi.h>
#include <shlobj.h>

namespace jietuba {
namespace {
QString pin_tr(const char *text) { return i18n_tr(text, "PinWindow"); }

// Opening more files than this at once asks first.
constexpr int MANY_OPENED = 15;

QFont card_font(int pixel_size, bool bold = false) {
    QFont font("Microsoft YaHei UI");
    if (!font.exactMatch()) font = QFont("Segoe UI");
    font.setPixelSize(pixel_size);
    font.setBold(bold);
    return font;
}

// Up to two lines, breaking inside words when a name has no spaces; a longer name ends in an ellipsis.
QStringList name_lines(const QString &name, const QFont &font, int width) {
    QTextLayout layout(name, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.beginLayout();
    QStringList lines;
    int start = 0, end = 0;
    for (int i = 0; i < 2; ++i) {
        QTextLine line = layout.createLine();
        if (!line.isValid()) break;
        line.setLineWidth(width);
        start = line.textStart();
        end = start + line.textLength();
        lines.append(name.mid(start, line.textLength()).trimmed());
    }
    layout.endLayout();
    if (lines.size() == 2 && end < name.size()) lines[1] = QFontMetrics(font).elidedText(name.mid(start).trimmed(), Qt::ElideRight, width);
    return lines;
}

QImage list_icon(int list_kind, int index) {
    IImageList *list = nullptr;
    if (FAILED(SHGetImageList(list_kind, IID_IImageList, reinterpret_cast<void **>(&list))) || !list) return {};
    HICON icon = nullptr;
    // The overlay, such as a shortcut's arrow, rides in the top byte.
    list->GetIcon(index & 0xFFFFFF, ILD_TRANSPARENT | INDEXTOOVERLAYMASK(static_cast<UINT>(index) >> 24), &icon);
    list->Release();
    if (!icon) return {};
    QImage image = QImage::fromHICON(icon);
    DestroyIcon(icon);
    return image;
}

// Whether anything shows outside the top-left square of side: the 256 px list puts a type
// without a large icon there at its small size.
bool fills_beyond(const QImage &image, int side) {
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < argb.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
        for (int x = y < side ? side : 0; x < argb.width(); ++x)
            if (qAlpha(line[x]) != 0) return true;
    }
    return false;
}

// The system's icon for path, as Explorer shows it, at the 256 px size when it has one and else at 48.
QImage system_icon(const QString &path) {
    SHFILEINFOW info{};
    const QString native = QDir::toNativeSeparators(path);
    if (!SHGetFileInfoW(reinterpret_cast<LPCWSTR>(native.utf16()), 0, &info, sizeof info, SHGFI_SYSICONINDEX | SHGFI_OVERLAYINDEX)) return {};
    const QImage jumbo = list_icon(SHIL_JUMBO, info.iIcon);
    if (!jumbo.isNull() && fills_beyond(jumbo, 48)) return jumbo;
    return list_icon(SHIL_EXTRALARGE, info.iIcon);
}
}

PinFileCard::PinFileCard(const QStringList &files, const QSize &max_size) : files_(files) {
    const int count = static_cast<int>(files_.size());
    const int max_columns = std::max(1, max_size.width() / CELL_WIDTH);
    const int max_rows = std::max(1, max_size.height() / CELL_HEIGHT);
    columns_ = std::clamp(count, 1, max_columns);
    rows_ = std::min(std::max(1, (count + columns_ - 1) / columns_), max_rows);
    const int cells = columns_ * rows_;
    shown_ = count > cells ? cells - 1 : count;
}

QRect PinFileCard::cell_rect(int index) const { return QRect((index % columns_) * CELL_WIDTH, (index / columns_) * CELL_HEIGHT, CELL_WIDTH, CELL_HEIGHT); }

int PinFileCard::file_at(const QPointF &pos) const {
    if (pos.x() < 0 || pos.y() < 0) return -1;
    const int column = static_cast<int>(pos.x()) / CELL_WIDTH, row = static_cast<int>(pos.y()) / CELL_HEIGHT;
    if (column >= columns_ || row >= rows_) return -1;
    const int index = row * columns_ + column;
    return index < shown_ ? index : -1;
}

QString PinFileCard::format_size(qint64 bytes) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    if (bytes < 1024) return QString("%1 B").arg(bytes);
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024 && unit < 4) {
        value /= 1024;
        ++unit;
    }
    return QString("%1 %2").arg(value, 0, 'f', 2).arg(units[unit]);
}

void PinFileCard::load_icons() const {
    icons_.resize(shown_);
    for (int i = 0; i < shown_; ++i) {
        const QImage icon = system_icon(files_[i]);
        icons_[i] = icon.width() > ICON_KEPT_PIXELS ? icon.scaled(ICON_KEPT_PIXELS, ICON_KEPT_PIXELS, Qt::KeepAspectRatio, Qt::SmoothTransformation) : icon;
    }
    icons_loaded_ = true;
}

QImage PinFileCard::render(const QSize &target, double device_pixel_ratio) const {
    if (!icons_loaded_) load_icons();
    QImage image(std::max(1, static_cast<int>(std::lround(target.width() * device_pixel_ratio))),
                 std::max(1, static_cast<int>(std::lround(target.height() * device_pixel_ratio))), QImage::Format_RGB32);
    image.fill(Qt::white);
    // Enlarged through the pixel ratio, as on a high-DPI screen: text and icons are drawn at the
    // larger size instead of scaled.
    image.setDevicePixelRatio(static_cast<double>(image.width()) / size().width());
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    // Each icon smoothly scaled once to the pixels it fills, rather than sampled while drawing.
    const int icon_pixels = std::max(1, static_cast<int>(std::lround(ICON_SIZE * image.devicePixelRatio())));
    for (int i = 0; i < shown_; ++i) {
        const QImage &icon = icons_[i];
        paint_card(painter, i, icon.isNull() || icon.width() == icon_pixels ? icon : icon.scaled(icon_pixels, icon_pixels, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    if (overflows()) paint_count(painter, cell_rect(shown_));
    painter.end();
    image.setDevicePixelRatio(device_pixel_ratio);
    return image;
}

void PinFileCard::paint_card(QPainter &painter, int index, const QImage &icon) const {
    const QFileInfo info(files_[index]);
    const QRect cell = cell_rect(index);
    const QRect icon_rect(cell.x() + MARGIN, cell.y() + (CELL_HEIGHT - ICON_SIZE) / 2, ICON_SIZE, ICON_SIZE);
    if (!icon.isNull()) painter.drawImage(icon_rect, icon);
    const int text_x = icon_rect.right() + 1 + ICON_TEXT_GAP;
    const int text_width = cell.right() + 1 - MARGIN - text_x;
    int y = cell.y() + MARGIN - 2;
    const auto name_font = card_font(NAME_PIXEL_SIZE);
    const QFontMetrics name_metrics(name_font);
    painter.setFont(name_font);
    painter.setPen(QColor(0x20, 0x20, 0x20));
    // A drive root has no file name.
    const QString name = info.fileName().isEmpty() ? QDir::toNativeSeparators(info.absoluteFilePath()) : info.fileName();
    for (const auto &line : name_lines(name, name_font, text_width)) {
        painter.drawText(QPoint(text_x, y + name_metrics.ascent()), line);
        y += name_metrics.height();
    }
    QString detail;
    if (info.isDir()) detail = pin_tr("Folder");
    else if (info.exists()) detail = format_size(info.size());
    if (detail.isEmpty()) return;
    const auto size_font = card_font(SIZE_PIXEL_SIZE);
    painter.setFont(size_font);
    painter.setPen(QColor(0x99, 0x99, 0x99));
    painter.drawText(QPoint(text_x, y + QFontMetrics(size_font).ascent()), detail);
}

void PinFileCard::paint_count(QPainter &painter, const QRect &cell) const {
    painter.setFont(card_font(NAME_PIXEL_SIZE, true));
    painter.setPen(get_theme().theme_color());
    painter.drawText(cell, Qt::AlignCenter, pin_tr("...%1 items in all").arg(files_.size()));
}

void PinFileCard::copy_files(const QStringList &files) {
    QList<QUrl> urls;
    for (const auto &file : files) urls.append(QUrl::fromLocalFile(file));
    auto *mime = new QMimeData;
    mime->setUrls(urls);
    QApplication::clipboard()->setMimeData(mime);
    log_info(T("已复制 {count} 个文件", {{"count", static_cast<int>(files.size())}}), "PinWindow");
}

void PinFileCard::copy_paths(const QStringList &files) {
    QStringList paths;
    for (const auto &file : files) paths.append(QDir::toNativeSeparators(file));
    QApplication::clipboard()->setText(paths.join('\n'));
}

void PinFileCard::reveal(const QStringList &files) {
    QMap<QString, QStringList> by_folder;
    for (const auto &file : files) by_folder[QFileInfo(file).absolutePath()].append(QDir::toNativeSeparators(QFileInfo(file).absoluteFilePath()));
    // The shell can take a moment to answer, so the pin never waits for it.
    auto *thread = QThread::create([by_folder] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        for (auto it = by_folder.begin(); it != by_folder.end(); ++it) {
            PIDLIST_ABSOLUTE folder = nullptr;
            if (FAILED(SHParseDisplayName(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(it.key()).utf16()), nullptr, &folder, 0, nullptr))) continue;
            std::vector<PIDLIST_ABSOLUTE> items;
            std::vector<PCUITEMID_CHILD> children;
            for (const auto &file : it.value()) {
                PIDLIST_ABSOLUTE item = nullptr;
                if (FAILED(SHParseDisplayName(reinterpret_cast<LPCWSTR>(file.utf16()), nullptr, &item, 0, nullptr))) continue;
                items.push_back(item);
                children.push_back(ILFindLastID(item));
            }
            SHOpenFolderAndSelectItems(folder, static_cast<UINT>(children.size()), children.data(), 0);
            for (auto *item : items) CoTaskMemFree(item);
            CoTaskMemFree(folder);
        }
        if (SUCCEEDED(com)) CoUninitialize();
    });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void PinFileCard::open(const QString &file) { QDesktopServices::openUrl(QUrl::fromLocalFile(file)); }

void PinFileCard::open(const QStringList &files, QWidget *parent) {
    if (files.size() > MANY_OPENED &&
        !show_confirm_dialog(parent, pin_tr("Open Many Files"), pin_tr("Open %1 files at once?").arg(files.size())))
        return;
    for (const auto &file : files) open(file);
}
}
