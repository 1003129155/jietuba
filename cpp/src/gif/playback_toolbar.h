#pragma once
#include <QLabel>
#include <QPushButton>
#include <QRect>
#include <QWidget>
#include <optional>
#include <utility>
#include <vector>

class QHBoxLayout;
class QVBoxLayout;

// The playback toolbar: range slider, play, speed, save, copy and re-record.
namespace jietuba {
class ClickMenuButton;

// A slider with trim_start / trim_end handles and a playhead.
class RangeSlider : public QWidget {
    Q_OBJECT
public:
    // Base sizes (pixels at 100%); hit testing uses the same values, so the visible and
    // clickable areas stay aligned at any scale.
    static constexpr int BASE_TRACK_H = 6;
    static constexpr int BASE_HANDLE_W = 10;
    static constexpr int BASE_HANDLE_H = 20;
    static constexpr int BASE_PLAYHEAD_W = 4;
    static constexpr int BASE_HEIGHT = 32;
    static constexpr int BASE_MIN_WIDTH = 200;
    static constexpr int BASE_GRAB_SLACK = 4;
    explicit RangeSlider(QWidget *parent = nullptr);
    // Recomputes the timeline's own size for the current scale.
    void apply_scale();
    void set_range(int total);
    void set_playhead(int index);
    void set_trim(int start, int end);
    int trim_start() const { return trim_start_; }
    int trim_end() const { return trim_end_; }
    std::pair<int, int> get_trim_range() const { return {trim_start_, trim_end_}; }
signals:
    void trim_changed(int start, int end);
    // A click on empty track jumps there.
    void seek_requested(int index);
protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
private:
    QRect track_rect() const;
    int index_to_x(int index) const;
    int x_to_index(int x) const;
    QRect handle_rect(int index) const;
    int total_ = 1;
    int trim_start_ = 0;
    int trim_end_ = 0;
    int playhead_ = 0;
    // "start", "end" or empty.
    QString dragging_;
};

// Floating toolbar for playback: trimming, playback control and export.
class PlaybackToolbar : public QWidget {
    Q_OBJECT
public:
    static constexpr int BASE_BTN_W = 34;
    static constexpr int BASE_BTN_H = 30;
    static constexpr int BASE_ICON = 22;
    static constexpr int BASE_HEIGHT = 76;
    static constexpr int BASE_MIN_WIDTH = 400;
    explicit PlaybackToolbar(QWidget *parent = nullptr);
    void set_toolbar_side(bool below);
    // Recomputes sizes for the current scale; position and trim stay.
    void apply_scale();
    void set_total_frames(int total, int fps = 16, int total_ms = 0);
    // Updates the progress and time; current_ms >= 0 shows the real timestamp,
    // otherwise index / fps.
    void set_playhead(int index, std::optional<int> total = std::nullopt, std::optional<int> fps = std::nullopt, int current_ms = -1);
    void set_playing(bool playing);
    std::pair<int, int> get_trim_range() const { return slider_->get_trim_range(); }
    RangeSlider *slider() const { return slider_; }
signals:
    void play_pause();
    void speed_changed(double speed);
    void seek_requested(int index);
    void trim_changed(int start, int end);
    void save_requested();
    void copy_requested();
    void rerecord();
    void close_requested();
    // The cursor display switch.
    void cursor_toggled(bool visible);
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    void build_ui();
    // Sets the icon and remembers its SVG: icons are rasterised per size and redrawn on scale changes.
    void set_button_icon(QPushButton *button, const QString &svg);
    static QString fmt(double seconds);
    void on_play_clicked();

    bool playing_ = false;
    int fps_ = 16;
    int total_frames_ = 1;
    int total_ms_ = 0;
    QWidget *container_ = nullptr;
    QVBoxLayout *vbox_ = nullptr;
    QHBoxLayout *row_ = nullptr;
    RangeSlider *slider_ = nullptr;
    QLabel *time_label_ = nullptr;
    ClickMenuButton *speed_label_ = nullptr;
    QPushButton *play_btn_ = nullptr;
    QPushButton *rerecord_btn_ = nullptr;
    QPushButton *cursor_btn_ = nullptr;
    QPushButton *copy_btn_ = nullptr;
    QPushButton *save_btn_ = nullptr;
    QPushButton *close_btn_ = nullptr;
    std::vector<std::pair<QPushButton *, QString>> button_svgs_;
};
}
