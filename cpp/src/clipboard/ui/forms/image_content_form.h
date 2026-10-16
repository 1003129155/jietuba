#pragma once
#include "clipboard/core/models.h"
#include <QImage>
#include <QPixmap>
#include <QWidget>
#include <optional>
#include <tuple>

class QMimeData;
class QVBoxLayout;

// Image items in the manage window. Editing shows the original with copy / pin / save
// actions and only the title is editable; adding takes a dropped, pasted or chosen image,
// records it through clipboard history and moves the record into the group.
namespace jietuba::clipboard {
class ManageDialog;

// Shows an image scaled down (never up), transparent areas over a checkerboard.
class ImagePreview : public QWidget {
    Q_OBJECT
public:
    explicit ImagePreview(const QImage &image, QWidget *parent = nullptr, int max_height = 340);
    QSize sizeHint() const override;
    QRect image_rect() const;

signals:
    void double_clicked();

protected:
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    int box_height(int width) const;
    QPixmap scaled_pixmap(const QRect &rect);
    QImage image_;
    int max_height_;
    std::optional<std::tuple<int, int, double>> cache_key_;
    QPixmap scaled_;
};

// A dropped image: the image data itself first, otherwise the first image file.
QImage image_from_mime(const QMimeData *mime);

// Where a new image goes: a dashed prompt until there is an image, then its preview.
class ImageDropZone : public QWidget {
    Q_OBJECT
public:
    explicit ImageDropZone(const QString &prompt, QWidget *parent = nullptr);
    void set_image(const std::optional<QImage> &image);

signals:
    void image_dropped(const QImage &image);

protected:
    void paintEvent(QPaintEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void set_drag_active(bool active);
    QString prompt_;
    ImagePreview *preview_ = nullptr;
    bool drag_active_ = false;
    QVBoxLayout *layout_;
};

// image is null when the original is no longer available.
void build_image_content_form(ManageDialog *dialog, const ClipboardItem &item, const QImage &image, qint64 data_size);
// Adding goes through clipboard history, so the page is disabled while recording is off.
void build_new_image_form(ManageDialog *dialog, bool monitoring);
}
