#include "core/game_mode_monitor.h"
#include "core/input_hub.h"
#include "core/logger.h"
#include "core/platform_utils.h"

namespace jietuba {
namespace {
const QString WATCHER = "game_mode";
}

GameModeMonitor::GameModeMonitor(QObject *parent) : QObject(parent) {
    fallback_.setInterval(FALLBACK_MS);
    fallback_.setTimerType(Qt::VeryCoarseTimer);
    connect(&fallback_, &QTimer::timeout, this, &GameModeMonitor::check);
    follow_up_.setSingleShot(true);
    follow_up_.setInterval(FOLLOW_UP_MS);
    connect(&follow_up_, &QTimer::timeout, this, &GameModeMonitor::on_follow_up);
}

void GameModeMonitor::set_probe(std::function<bool()> probe) { probe_ = std::move(probe); }

void GameModeMonitor::start() {
    if (!running_) {
        running_ = true;
        try {
            auto *hub = input_hub();
            foreground_connection_ = connect(hub, &InputHub::foreground, this, &GameModeMonitor::on_foreground, Qt::QueuedConnection);
            hub->native->watch_foreground(WATCHER);
        } catch (const std::exception &e) {
            // The fallback still checks, only later.
            log_error(T("前台窗口监听启动失败: {e}", {{"e", QString::fromUtf8(e.what())}}), "Hotkey");
        }
        fallback_.start();
    }
    check();
}

void GameModeMonitor::stop() {
    if (running_) {
        running_ = false;
        fallback_.stop();
        follow_up_.stop();
        disconnect(foreground_connection_);
        if (auto *hub = existing_input_hub()) hub->native->unwatch_foreground(WATCHER);
    }
    set_active(false);
}

void GameModeMonitor::check() { set_active(probe_ ? probe_() : is_game_in_foreground()); }

void GameModeMonitor::on_foreground() {
    // Switches queued before stop no longer count.
    if (!running_) return;
    check();
    follow_ups_left_ = FOLLOW_UPS;
    follow_up_.start();
}

void GameModeMonitor::on_follow_up() {
    if (!running_) return;
    check();
    if (--follow_ups_left_ > 0) follow_up_.start();
}

void GameModeMonitor::set_active(bool active) {
    if (active == active_) return;
    active_ = active;
    emit changed(active);
}
}
