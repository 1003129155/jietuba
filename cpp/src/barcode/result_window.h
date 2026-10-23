#pragma once
#include "barcode/reader.h"
#include "ui/fluent_lite/cards.h"
#include "ui/fluent_lite/frameless.h"
#include "ui/fluent_lite/inputs.h"
#include <QImage>
#include <QPushButton>
#include <QScrollArea>
#include <vector>

namespace jietuba {
class BodyLabel;
class CheckBox;

// The screenshot with every code outlined and numbered. The image is only scaled
// down, never up: enlarged small selections blur and hide their real size.
class CodeImageView : public QWidget {
    Q_OBJECT
public:
    CodeImageView(const QImage &image, const std::vector<DecodedCode> &codes, QWidget *parent = nullptr);
    void set_active(int index);
signals:
    // The code under the mouse, -1 when none.
    void hovered(int index);
protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
private:
    // Image pixels to widget coordinates (fit and centred); painting and hit testing share it.
    QTransform image_to_widget() const;
    QImage image_;
    std::vector<DecodedCode> codes_;
    int active_ = -1;
};

class CodeBadge : public QWidget {
    Q_OBJECT
public:
    explicit CodeBadge(int number, QWidget *parent = nullptr);
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    int number_;
};

// The decoded content: read-only, selectable, as tall as its content, no scroll
// bars. Not a QLabel, which only wraps at spaces and so stretches or clips long links.
class CodeText : public TextEdit {
    Q_OBJECT
public:
    explicit CodeText(const QString &text, QWidget *parent = nullptr);
private:
    void apply_theme();
};

// One code: number, format and content, plus copy / open link.
class CodeCard : public SimpleCardWidget {
    Q_OBJECT
public:
    CodeCard(int index, const DecodedCode &code, QWidget *parent = nullptr);
    void set_active(bool active);
    // Card colours stay SimpleCardWidget's; only the active state is added.
    void _apply_theme() override;
    int index;
    QPushButton *copy_button;
    QPushButton *open_button = nullptr;
signals:
    // Its own index on enter, -1 on leave.
    void hovered(int index);
protected:
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
private:
    void copy(const QString &text);
};

// Scan result: the screenshot with outlined, numbered codes on the left and the
// codes listed by number on the right. Hovering a code on either side highlights
// both; only set_active changes which code is current.
//
// Marks tied to a code on the screenshot use the screenshot theme colour, like the
// selection frame; ordinary controls follow the application theme.
class BarcodeResultWindow : public FrostedFramelessDialog {
    Q_OBJECT
public:
    static constexpr int PANEL_WIDTH = 300;
    // codes come from read_codes; outlines are in image pixels.
    BarcodeResultWindow(const QImage &image, const std::vector<DecodedCode> &codes, QWidget *parent = nullptr);
    CodeImageView *image_view;
    BodyLabel *summary_label;
    std::vector<CodeCard *> cards;
    CheckBox *copy_single_check;
private:
    void apply_theme();
    // 1:1 when the image fits, otherwise 80% of the screen; centred on the cursor's screen.
    void place(const QImage &image);
    // Highlights code index, -1 for none.
    void set_active_code(int index);
    void on_image_hovered(int index);
    QScrollArea *scroll_;
    QWidget *panel_;
};

// Decodes image and shows the result window. Decoding runs on the GUI thread with
// no waiting state: a full 2560x1440 screen takes about 65 ms.
//
// With copy_single and exactly one code no window opens: the code is copied and a
// toast shows next to the cursor, returning nullptr.
BarcodeResultWindow *show_barcode_result(const QImage &image, bool copy_single = false);
// 工作线程已完成识别时，GUI 线程直接展示结果，避免再次解码。
BarcodeResultWindow *show_barcode_result(const QImage &image, const std::vector<DecodedCode> &codes, bool copy_single = false);
}
