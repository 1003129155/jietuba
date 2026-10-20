#pragma once
#include "ui/fluent_lite/frameless.h"
#include <QFrame>
#include <QImage>
#include <QPixmap>
#include <QTimer>
#include <QToolButton>
#include <optional>
#include <utility>
#include <vector>

class QComboBox;
class QVBoxLayout;
class QLabel;
class QSlider;
class QSpinBox;

namespace jietuba {
class LineEdit;
class PushButton;
class SegmentedWidget;
class ToolSettingsManager;

// Whether interactive saves open SaveImageDialog rather than the system Save As dialog.
bool use_modern_save_dialog(ToolSettingsManager *config);

// A frequently used save folder, shown as a tag under the folder field.
struct PathTag {
    QString name, path;
};
// Stored as "name|path" strings; an empty list gives back the Desktop tag.
std::vector<PathTag> load_path_tags(ToolSettingsManager *config);
void save_path_tags(ToolSettingsManager *config, const std::vector<PathTag> &tags);
bool same_folder(const QString &a, const QString &b);

// Asks for a tag name, filled with initial and selected.
class PathTagNameDialog : public FrostedFramelessDialog {
    Q_OBJECT
public:
    PathTagNameDialog(const QString &initial, QWidget *parent = nullptr);
    static std::optional<QString> ask(QWidget *parent, const QString &initial);
    LineEdit *name_edit;
    PushButton *save_button;
    PushButton *cancel_button;
};

// One tag: folder mark and name; edit and delete appear on hover, and on right click
// as a menu. Outlined when it is the folder being saved to.
class PathTagChip : public QFrame {
    Q_OBJECT
public:
    PathTagChip(const PathTag &tag, QWidget *parent = nullptr);
    void set_current(bool current);
    PathTag tag;
    QToolButton *edit_button;
    QToolButton *close_button;
signals:
    void clicked();
    void edit_requested();
    void delete_requested();
protected:
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
};

// The tags of load_path_tags plus an add button, wrapped to the bar's fixed width.
// Adding or renaming follows the rules of one name per folder and one folder per name.
class PathTagBar : public QWidget {
    Q_OBJECT
public:
    PathTagBar(ToolSettingsManager *config, int width, QWidget *parent = nullptr);
    void set_current_path(const QString &path);
    // Asks a name for path (its folder name first) and adds it.
    void add_path(const QString &path);
    // RETRY: the name belongs to another folder and the user agreed to choose another.
    enum class AddResult { DONE, CANCELLED, RETRY };
    // Adds name for path, asking when the name or the path is already taken.
    AddResult add_tag(const QString &name, const QString &path);
    void rename_tag(int index);
    void remove_tag(int index);
    const std::vector<PathTag> &tags() const { return tags_; }
    std::vector<PathTagChip *> chips;
    QToolButton *add_button;
signals:
    void path_chosen(const QString &path);
private:
    void rebuild();
    void persist();
    int index_of_name(const QString &name) const;
    int index_of_path(const QString &path) const;
    ToolSettingsManager *config_;
    std::vector<PathTag> tags_;
    QString current_path_;
    int width_;
    QVBoxLayout *rows_;
};

// The image being saved: fitted (never enlarged) on open, the wheel zooms around the
// cursor, dragging pans, double click fits again. The adjusted result (resized and
// encoded) is what shows by default; in split mode the original is left of a draggable
// divider and the result right of it, both filling the result's rectangle.
class SavePreviewView : public QWidget {
    Q_OBJECT
public:
    enum Mode { ORIGINAL, ENCODED, SPLIT };
    static constexpr double MIN_ZOOM = 0.02;
    static constexpr double MAX_ZOOM = 32.0;
    explicit SavePreviewView(QWidget *parent = nullptr);
    // The image at the saved size, before encoding. A new size drops the encoded copy,
    // which no longer lines up.
    void set_image(const QImage &image);
    // The untouched source, shown by Original and left of the divider. Without one the
    // image itself stands in.
    void set_original(const QImage &original);
    // A null image shows the unencoded one as the result.
    void set_encoded(const QImage &encoded);
    void set_mode(Mode mode);
    void set_loading(bool loading);
    void fit();
    void set_zoom(double zoom, const QPointF &anchor);
    Mode mode() const { return mode_; }
    double zoom() const { return zoom_; }
    // The divider as a fraction of the image width.
    double split() const { return split_; }
    const QImage &image() const { return image_; }
    const QImage &encoded() const { return encoded_; }
    QRectF image_rect() const;
    // Room kept free at the top for the controls laid over the view.
    void set_top_inset(int inset);
signals:
    void zoom_changed(double zoom);
protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
private:
    bool splitting() const { return mode_ == SPLIT; }
    const QImage &original_source() const { return original_.isNull() ? image_ : original_; }
    const QImage &result() const { return encoded_.isNull() ? image_ : encoded_; }
    QRectF fit_area() const;
    double divider_x() const;
    bool near_divider(const QPointF &pos) const;
    void clamp_offset();
    void update_cursor(const QPointF &pos);
    enum Drag { NONE, PAN, DIVIDER };
    QImage image_, encoded_, original_;
    Mode mode_ = ORIGINAL;
    double zoom_ = 1.0;
    QPointF offset_;
    bool fitted_ = true;
    double split_ = 0.5;
    bool loading_ = false;
    int top_inset_ = 0;
    Drag drag_ = NONE;
    QPointF drag_origin_, drag_offset_;
    QPixmap checker_;
};

// Save window: preview on the left; folder (with tags), name and format, size, quality
// and the estimated size on the right. The size shown is the length of the bytes
// encoded with the current settings, the same bytes Save writes. Encoding runs off the
// UI thread, debounced.
class SaveImageDialog : public FrostedFramelessDialog {
    Q_OBJECT
public:
    enum class Outcome { SAVED, CANCELLED, CLASSIC };
    static constexpr int PANEL_WIDTH = 330;
    static constexpr int ENCODE_DELAY_MS = 200;
    static constexpr int MAX_SCALE_PERCENT = 400;
    // base_name is the file name without extension.
    SaveImageDialog(const QImage &image, const QString &base_name, ToolSettingsManager *config, QWidget *parent = nullptr);
    // Shows the dialog and writes the file on Save. CLASSIC: the user switched to the system
    // dialog, which the caller opens for this save.
    static std::pair<Outcome, QString> run(QWidget *parent, const QImage &image, const QString &base_name, ToolSettingsManager *config);
    // Why name cannot be a file name, empty when it can.
    static QString file_name_problem(const QString &name);
    QString target_path() const;
    QString current_format() const;
    int current_quality() const;
    QSize target_size() const;
    bool percent_mode() const;
    QString saved_path() const { return saved_path_; }
    bool switched_to_classic() const { return classic_; }
    // -1 while encoding or when the format cannot hold the image.
    qint64 encoded_bytes() const { return encoded_bytes_; }

    SavePreviewView *preview;
    SegmentedWidget *view_mode;
    LineEdit *directory_edit;
    PushButton *change_button;
    PathTagBar *tag_bar;
    LineEdit *name_edit;
    QComboBox *format_combo;
    QLabel *name_hint;
    SegmentedWidget *size_mode;
    QSpinBox *width_spin;
    QSpinBox *height_spin;
    QLabel *width_suffix;
    QLabel *height_suffix;
    QToolButton *lock_button;
    QToolButton *reset_size_button;
    QLabel *lossless_hint;
    QWidget *quality_block;
    QSlider *quality_slider;
    QLabel *quality_value;
    QLabel *file_size_label;
    QLabel *zoom_label;
    QLabel *original_label;
    QLabel *classic_link;
    PushButton *save_button;
signals:
    void encode_finished(qint64 bytes);
protected:
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    struct Encoded {
        QImage scaled, decoded;
        qint64 bytes = -1;
    };
    static Encoded encode(const QImage &image, const QSize &size, const QString &format, int quality);
    void schedule_encode();
    void start_encode();
    void on_encoded(int generation, const Encoded &result);
    void on_format_changed();
    // Sets the size from the width or height just typed, following the lock.
    void on_size_typed(bool width_changed);
    void set_target_size(const QSize &size);
    void show_size();
    void update_quality_controls();
    void update_view_mode();
    void update_status();
    void update_save_enabled();
    void choose_directory();
    void switch_to_classic();
    void accept_save();
    void apply_theme();
    void place(QWidget *parent);
    QImage image_;
    ToolSettingsManager *config_;
    QSize size_;
    QTimer encode_timer_;
    int generation_ = 0;
    bool encoding_ = false;
    bool encode_pending_ = false;
    bool updating_size_ = false;
    qint64 encoded_bytes_ = -1;
    QString saved_path_;
    bool classic_ = false;
    QWidget *panel_;
};
}
