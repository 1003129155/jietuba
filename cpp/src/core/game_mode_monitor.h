#pragma once
#include <QMetaObject>
#include <QObject>
#include <QTimer>
#include <functional>

namespace jietuba {
// Reports when a game takes or leaves the foreground. Checks follow foreground switches, which the
// input hub reports through a window event hook, not a low-level input hook.
class GameModeMonitor : public QObject {
    Q_OBJECT
public:
    // A game outside Windows' list counts only once in exclusive full screen, which it may enter a
    // while after its window appears, so a switch is checked again twice.
    static constexpr int FOLLOW_UP_MS = 2500;
    static constexpr int FOLLOW_UPS = 2;
    // Catches such a game switching to or from exclusive full screen without a foreground switch.
    static constexpr int FALLBACK_MS = 30000;
    explicit GameModeMonitor(QObject *parent = nullptr);
    // Replaces the check (tests); empty restores is_game_in_foreground.
    void set_probe(std::function<bool()> probe);
    // Checks now, then on every foreground switch and every FALLBACK_MS.
    void start();
    // Stops checking; a game seen until now is reported gone.
    void stop();
    // One check; reports only a change.
    void check();
    bool running() const { return running_; }
    bool active() const { return active_; }
signals:
    void changed(bool active);
private:
    void on_foreground();
    void on_follow_up();
    void set_active(bool active);
    std::function<bool()> probe_;
    QTimer fallback_;
    QTimer follow_up_;
    int follow_ups_left_ = 0;
    QMetaObject::Connection foreground_connection_;
    bool running_ = false;
    bool active_ = false;
};
}
