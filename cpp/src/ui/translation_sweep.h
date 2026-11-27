#pragma once
#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>
#include <optional>

class QPainter;

namespace jietuba {
// The wait for a translation, drawn over the picture it is for: a soft band of light sweeps
// across the area, rests a moment and sweeps again until the wait ends. It needs nothing but the
// area, so a capture and a long screenshot look the same. Clicks go through it.
class TranslationSweep : public QWidget {
public:
    // A wait shorter than this shows nothing.
    static constexpr int SHOW_DELAY_MS = 200;
    static constexpr int FRAME_MS = 33;
    // One crossing, then a rest until the period ends.
    static constexpr int CROSS_MS = 1300;
    static constexpr int PERIOD_MS = 1700;
    // Drawing: the delay passed and the wait goes on.
    bool active() const { return active_; }
    // Holds the animation at ms after it started, for tests and stills.
    void hold_at_for_test(int ms);
    // The part of area the band covers at ms; empty while it rests.
    static QRect band_bounds(const QRect &area, qint64 ms);
    // Over a light picture the band is the theme colour; over a dark one it is light.
    static void paint_band(QPainter &painter, const QRect &area, bool dark, qint64 ms);

protected:
    explicit TranslationSweep(QWidget *parent = nullptr);
    // Starts the wait, drawn once SHOW_DELAY_MS have passed; nothing when it is already on.
    void begin();
    void end();
    // In this widget's coordinates.
    virtual QRect area() const = 0;
    virtual bool dark() const = 0;
    void paintEvent(QPaintEvent *event) override;

private:
    void start();
    void tick();
    qint64 now() const;

    QTimer delay_, frames_;
    QElapsedTimer clock_;
    std::optional<qint64> held_ms_;
    qint64 drawn_ms_ = 0;
    bool waiting_ = false;
    bool active_ = false;
};

// The sweep in a window of its own over a screen area, for the long screenshot. Screen capture
// skips the window, so it never ends up in the long image.
class TranslationSweepWindow : public TranslationSweep {
public:
    TranslationSweepWindow(const QRect &screen_area, bool dark);
    using TranslationSweep::begin;
    using TranslationSweep::end;

protected:
    QRect area() const override { return rect(); }
    bool dark() const override { return dark_; }

private:
    bool dark_;
};
}
