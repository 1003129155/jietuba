#pragma once
#include <QPointer>
#include <QThread>
#include <functional>
#include <deque>

// Start-up in two phases. Before Qt: environment, DPI awareness, crash hooks, single
// instance. After Qt: PreloadManager registers hotkeys and the tray, then warms capture,
// toolbar, OCR and settings in a chain.
namespace jietuba {
class MainApp;
class ToolSettingsManager;

// Environment variables and DPI awareness; before QApplication exists.
void setup_environment();
// Terminates a confirmed older instance of this program, then records this one. The
// record holds the PID and creation time, since Windows reuses PIDs.
void ensure_single_instance();
// With admin mode on and this process not elevated: runs the scheduled task so an elevated
// instance takes over, or, when the task's program is gone, starts this program elevated through
// UAC (that instance points the task at itself). True once that instance has rewritten the
// instance record, and this process should exit; a task that cannot run shows as a timeout and
// start-up goes on normally. pid_file defaults to this executable's instance record.
bool hand_off_to_admin_task(int poll_ms = 100, int timeout_ms = 5000, const QString &pid_file = {});

// The chained preloading after start-up. Main-thread steps continue with
// singleShot(0) (one event loop turn in between); worker steps continue on finished.
class PreloadManager {
public:
    explicit PreloadManager(MainApp *app);
    // Builds the steps from the settings and starts them.
    void build_and_start();
    // Waits for the preload threads, at most timeout_ms each.
    void wait_for_threads(unsigned long timeout_ms);
    // Whether a preload thread still runs (blocks updates).
    bool threads_running() const;
    // Every step has run and no preload thread is left.
    bool finished() const { return steps_.empty() && !threads_running(); }
    void preload_settings();
private:
    // Takes the head of the queue and runs it; a step returning true continues itself
    // when its thread finishes.
    void run_next();
    bool preload_toolbar_assets();
    bool preload_hdr_session();
    bool seed_bundled_runtime();
    bool apply_ocr_engine_setting();
    bool preload_ocr_engine();
    // Hotkeys and the tray; after the welcome wizard on the first run.
    bool register_entry_points();
    bool show_main_window_on_start();
    void init_clipboard_manager();
    // Builds the clipboard window hidden when starting in the background.
    void preload_clipboard_window();

    MainApp *app_;
    ToolSettingsManager *config_;
    std::deque<std::function<bool()>> steps_;
    QPointer<QThread> ocr_preload_thread_;
};

// The entry point: start-up preparation, then the application.
int run(int argc, char **argv);
}
