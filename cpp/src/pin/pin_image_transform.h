#pragma once
#include <QImage>
#include <QSize>
#include <QTransform>

namespace jietuba {
class PinWindow;

// Rotation (0/90/180/270) and flips of a pin: view transform, window size and image
// export all live here; the window only calls in. Everything drawn over the scene in
// image pixels, recognized text included, follows through the view transform.
class PinImageTransform {
public:
    int rotation() const { return rotation_; }
    bool flip_h() const { return flip_h_; }
    bool flip_v() const { return flip_v_; }
    bool has_transform() const { return rotation_ != 0 || flip_h_ || flip_v_; }
    // Width and height swap.
    bool is_rotated_90_or_270() const { return rotation_ == 90 || rotation_ == 270; }
    void rotate_cw() { rotation_ = (rotation_ + 90) % 360; }
    void rotate_ccw() { rotation_ = (rotation_ + 270) % 360; }
    void flip_horizontal() { flip_h_ = !flip_h_; }
    void flip_vertical() { flip_v_ = !flip_v_; }
    void reset() {
        rotation_ = 0;
        flip_h_ = flip_v_ = false;
    }
    QSize display_size(const QSize &orig_size) const;
    // Scene (original image) to viewport (rotated/flipped display): rotate and flip
    // around the scene centre, align the bounding box at the origin, then scale.
    QTransform build_view_transform(double content_w, double content_h, double scene_w, double scene_h) const;
    // Resizes the window around its centre at the exact logical scale (never derived
    // back from rounded window sizes, so transforms cause no size drift), then updates
    // view, background, buttons, toolbar and border.
    void apply_to_window(PinWindow *pin_window) const;
    // Creates or refreshes the border overlay; a locked pin always shows the locked colour.
    void refresh_border(PinWindow *pin_window) const;
    QImage transform_image(const QImage &image) const;
private:
    int rotation_ = 0;
    bool flip_h_ = false;
    bool flip_v_ = false;
};
}
