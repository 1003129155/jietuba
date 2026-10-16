#pragma once
#include <QImage>
#include <memory>

namespace jietuba {
// The system pointer at capture time, drawn into the image afterwards. BitBlt and
// DXGI frames do not contain the pointer. DrawIconEx paints straight onto the
// pixels: monochrome pointers (such as the I-beam) rely on inversion, which a
// pre-rendered image with alpha would lose on dark backgrounds.
class SystemCursor {
public:
    // Records the current pointer; nullptr when it is hidden or unavailable.
    static std::shared_ptr<SystemCursor> grab();
    ~SystemCursor();
    SystemCursor(const SystemCursor &) = delete;
    SystemCursor &operator=(const SystemCursor &) = delete;
    // origin is the screen position of the image's top-left corner.
    void draw_onto(QImage &image, int origin_x, int origin_y) const;
    // Top-left of the pointer image in physical screen coordinates, hotspot already subtracted.
    int x, y, width, height;
private:
    SystemCursor(void *handle, int x, int y, int width, int height) : x(x), y(y), width(width), height(height), handle_(handle) {}
    // A copy of the cursor handle: the original belongs to the foreground program,
    // which may destroy it mid-session while background refreshes still draw it.
    void *handle_;
};
}
