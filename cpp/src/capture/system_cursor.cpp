#include "capture/system_cursor.h"
#include <QPainter>
#include <cstring>
#include <optional>
#include <tuple>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace jietuba {
namespace {
// (width, height, hotspot x, hotspot y) of the pointer image.
std::optional<std::tuple<int, int, int, int>> icon_geometry(HICON handle) {
    ICONINFO info{};
    if (!GetIconInfo(handle, &info)) return std::nullopt;
    std::optional<std::tuple<int, int, int, int>> result;
    BITMAP bitmap{};
    HBITMAP source = info.hbmColor ? info.hbmColor : info.hbmMask;
    if (GetObjectW(source, sizeof(BITMAP), &bitmap)) {
        int height = bitmap.bmHeight;
        // A monochrome pointer's mask stacks the AND and XOR halves.
        if (!info.hbmColor) height /= 2;
        result = std::make_tuple(static_cast<int>(bitmap.bmWidth), height, static_cast<int>(info.xHotspot), static_cast<int>(info.yHotspot));
    }
    DeleteObject(info.hbmMask);
    if (info.hbmColor) DeleteObject(info.hbmColor);
    return result;
}
}

std::shared_ptr<SystemCursor> SystemCursor::grab() {
    CURSORINFO info{};
    info.cbSize = sizeof(CURSORINFO);
    if (!GetCursorInfo(&info)) return nullptr;
    if (!(info.flags & CURSOR_SHOWING) || !info.hCursor) return nullptr;
    HICON handle = CopyIcon(info.hCursor);
    if (!handle) return nullptr;
    const auto geometry = icon_geometry(handle);
    if (!geometry) {
        DestroyIcon(handle);
        return nullptr;
    }
    const auto [width, height, hotspot_x, hotspot_y] = *geometry;
    return std::shared_ptr<SystemCursor>(new SystemCursor(handle, info.ptScreenPos.x - hotspot_x, info.ptScreenPos.y - hotspot_y, width, height));
}

SystemCursor::~SystemCursor() { DestroyIcon(static_cast<HICON>(handle_)); }

void SystemCursor::draw_onto(QImage &image, int origin_x, int origin_y) const {
    const int left = x - origin_x;
    const int top = y - origin_y;
    const int x0 = std::max(left, 0), y0 = std::max(top, 0);
    const int x1 = std::min(left + width, image.width());
    const int y1 = std::min(top + height, image.height());
    if (x0 >= x1 || y0 >= y1) return;
    const int w = x1 - x0, h = y1 - y0;
    const int stride = w * 4;
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(BITMAPINFOHEADER);
    header.biWidth = w;
    header.biHeight = -h;  // negative height: top-down, matching QImage row order
    header.biPlanes = 1;
    header.biBitCount = 32;
    void *bits = nullptr;
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return;
    HBITMAP dib = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO *>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) {
        DeleteDC(hdc);
        return;
    }
    HGDIOBJ previous = SelectObject(hdc, dib);
    QImage drawn;
    const auto patch = image.copy(x0, y0, w, h).convertToFormat(QImage::Format_RGB32);
    for (int row = 0; row < h; ++row) std::memcpy(static_cast<uchar *>(bits) + row * stride, patch.constScanLine(row), stride);
    if (DrawIconEx(hdc, left - x0, top - y0, static_cast<HICON>(handle_), width, height, 0, nullptr, DI_NORMAL)) {
        GdiFlush();
        drawn = QImage(w, h, QImage::Format_RGB32);
        for (int row = 0; row < h; ++row) {
            auto *line = drawn.scanLine(row);
            std::memcpy(line, static_cast<const uchar *>(bits) + row * stride, stride);
            // Monochrome mask operations clear the alpha bytes; RGB32 requires 0xFF.
            for (int col = 0; col < w; ++col) line[col * 4 + 3] = 0xFF;
        }
    }
    SelectObject(hdc, previous);
    DeleteObject(dib);
    DeleteDC(hdc);
    if (drawn.isNull()) return;
    QPainter painter(&image);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(x0, y0, drawn);
    painter.end();
}
}
