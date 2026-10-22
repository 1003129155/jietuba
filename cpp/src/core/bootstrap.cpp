#include "core/bootstrap.h"
#include "clipboard/ui/widgets/item_drag.h"
#include "clipboard/ui/windows/clipboard_window.h"
#include "core/shortcut_manager.h"
#include "ui/dialogs.h"
#include "ui/welcome/wizard.h"
#include "capture/capture_service.h"
#include "capture/msaa_element_finder.h"
#include "core/crash_handler.h"
#include "core/elevation.h"
#include "core/logger.h"
#include "core/platform_utils.h"
#include "main_app.h"
#include "ocr/bundled_runtime.h"
#include "ocr/ocr_manager.h"
#include "ocr/ppocr_models.h"
#include "settings/data_folder.h"
#include "settings/tool_settings.h"
#include "ui/toolbar.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTimer>
#include <QWidget>
#include <iterator>
#include <optional>
#include <thread>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace jietuba {
namespace {
// (pid, create_time); create_time is absent in records from older versions that wrote only the PID.
std::optional<std::pair<qint64, std::optional<quint64>>> read_instance_record(const QString &pid_file) {
    QFile file(pid_file);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return std::nullopt;
    QStringList lines;
    for (const auto &line : QString::fromUtf8(file.readAll()).split('\n'))
        if (!line.trimmed().isEmpty()) lines << line.trimmed();
    if (lines.isEmpty()) return std::nullopt;
    bool ok = false;
    const qint64 pid = lines[0].toLongLong(&ok);
    // A damaged or unknown file counts as no record and is rewritten.
    if (!ok) return std::nullopt;
    std::optional<quint64> create_time;
    if (lines.size() > 1) {
        const quint64 value = lines[1].toULongLong(&ok);
        if (!ok) return std::nullopt;
        create_time = value;
    }
    return std::make_pair(pid, create_time);
}

// Whether the record really is an older instance of this program. The PID file easily
// goes stale (killed or crashed processes never clean it up) and Windows reuses PIDs
// eagerly, so a PID alone could kill an unrelated process.
bool is_our_old_instance(const std::pair<qint64, std::optional<quint64>> &record, qint64 my_pid, const QString &my_image_name) {
    const auto [pid, create_time] = record;
    if (pid == my_pid) return false;
    const auto live = get_process_identity(pid);
    // Gone or inaccessible: a stale record.
    if (!live) return false;
    const auto [live_create_time, live_image_name] = *live;
    // (PID, creation time) identifies one process; the most reliable test.
    if (create_time) return live_create_time == *create_time;
    // Older records lack the creation time: the image name must match this executable.
    if (my_image_name.isEmpty()) return false;
    return live_image_name.toLower() == my_image_name.toLower();
}

// This executable's file name; QCoreApplication does not exist yet.
QString exe_file_name() {
    wchar_t path[MAX_PATH * 4] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return QFileInfo(QString::fromWCharArray(path, static_cast<int>(length))).fileName();
}

QString pid_file_path() {
    // Named after the executable, as the reference's packaged build does.
    return QDir(QDir::tempPath()).filePath(exe_file_name() + ".pid");
}
}

void setup_environment() {
    install_crash_hooks();
    // Qt's automatic high-DPI scaling is off: at desktop scales other than 100% it makes
    // the capture look wrong. Must be set before Qt starts.
    qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");
    qputenv("QT_SCALE_FACTOR", "1");
    // DPI is handled manually, so Qt's DPI warnings are noise.
    qputenv("QT_LOGGING_RULES", "qt.qpa.window=false");
    set_dpi_awareness();
}

void ensure_single_instance() {
    const auto pid_file = pid_file_path();
    const qint64 my_pid = static_cast<qint64>(GetCurrentProcessId());
    const auto my_identity = get_process_identity(my_pid);
    const std::optional<quint64> my_create_time = my_identity ? std::optional<quint64>(my_identity->first) : std::nullopt;
    const auto my_image_name = my_identity ? my_identity->second : exe_file_name();

    if (const auto record = QFile::exists(pid_file) ? read_instance_record(pid_file) : std::nullopt) {
        if (is_our_old_instance(*record, my_pid, my_image_name)) {
            if (terminate_process_by_pid(record->first)) log_info(T("已终止旧实例 (PID {old_pid})", {{"old_pid", record->first}}), "SingleInstance");
            else log_warning(T("旧实例 (PID {old_pid}) 终止失败", {{"old_pid", record->first}}), "SingleInstance");
        } else {
            log_debug(T("实例记录 (PID {old_pid}) 未通过身份校验，不终止", {{"old_pid", record->first}}), "SingleInstance");
        }
    }
    // Records this instance: PID and creation time.
    QFile file(pid_file);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        QByteArray content = QByteArray::number(my_pid) + "\n";
        if (my_create_time) content += QByteArray::number(*my_create_time) + "\n";
        file.write(content);
    } else {
        log_exception(file.errorString(), T("写入实例记录"));
    }
    // Removed on normal exit; a killed or crashed process leaves it to the check above.
    std::atexit([] {
        const auto path = pid_file_path();
        const auto current = read_instance_record(path);
        if (current && current->first == static_cast<qint64>(GetCurrentProcessId())) QFile::remove(path);
    });
}

bool hand_off_to_admin_task(int poll_ms, int timeout_ms, const QString &pid_file) {
    // The task's own instance is elevated, and after the account drops to a standard user
    // the state is STANDARD, so neither hands off again.
    if (elevation::elevation_state() != elevation::CAN_ELEVATE) return false;
    const auto command = elevation::query_task_command();
    // A task whose program is gone starts nothing: this program starts itself elevated instead,
    // and that instance points the task at its own place (PreloadManager).
    const bool stale = elevation::task_is_stale(command);
    if (!stale && !elevation::points_to_self(command)) return false;
    const QString path = pid_file.isEmpty() ? pid_file_path() : pid_file;
    const auto before = QFile::exists(path) ? read_instance_record(path) : std::nullopt;
    if (stale) {
        log_info(T("管理员计划任务指向的程序已不存在，以管理员身份重新启动并修复"), "Elevation");
        if (!elevation::run_self_elevated()) {
            log_warning(T("无法以管理员身份重新启动，以普通权限启动"), "Elevation");
            return false;
        }
    } else if (!elevation::run_task()) {
        log_warning(T("运行管理员计划任务失败，以普通权限启动"), "Elevation");
        return false;
    }
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
    while (GetTickCount64() < deadline) {
        Sleep(static_cast<DWORD>(poll_ms));
        const auto record = QFile::exists(path) ? read_instance_record(path) : std::nullopt;
        if (record && record != before && record->first != static_cast<qint64>(GetCurrentProcessId())) {
            log_info(T("已交给管理员实例 (PID {pid})", {{"pid", record->first}}), "Elevation");
            return true;
        }
    }
    log_warning(T("等待管理员实例超时，以普通权限启动"), "Elevation");
    return false;
}

PreloadManager::PreloadManager(MainApp *app) : app_(app), config_(app->config_manager) {}

void PreloadManager::build_and_start() {
    auto *cfg = config_;
    if (cfg->get_preload_screenshot() && cfg->get_smart_selection_mode() == "element")
        prepare_point_detection();
    // Entry points come first and preloading follows: a capture during preloading costs
    // nothing extra here. The reference registers hotkeys last, because there preloading
    // and a capture compete for the interpreter. Its module-import warm-ups (the GDI grab
    // and the PNG encoder) have nothing to warm in a native build and are left out.
    // An elevated instance points a task left on a program that is gone at itself; schtasks runs
    // on a worker so start-up is not held.
    if (cfg->get_run_as_admin()) steps_.push_back([] {
        std::thread([] { run_thread_guarded("AdminTask", [] { elevation::repair_task(); }); }).detach();
        return false;
    });
    // Before the engine setting, which resolves the engine by what is installed.
    if (!bundled_runtime::data().isEmpty()) steps_.push_back([this] { return seed_bundled_runtime(); });
    // The engine preference is read here and handed to OCR, so recognition threads never read settings.
    steps_.push_back([this] { return apply_ocr_engine_setting(); });
    // Files of drags from the last run that exited before removing them; nothing can be dragged yet.
    steps_.push_back([] {
        clipboard::ItemDragMimeData::remove_stale_folders(clipboard::ItemDragMimeData::default_temp_root(), 0);
        return false;
    });
    if (cfg->get_preload_clipboard()) steps_.push_back([this] {
        init_clipboard_manager();
        return false;
    });
    steps_.push_back([this] { return register_entry_points(); });
    if (cfg->get_preload_screenshot() && cfg->get_capture_engine() != "mss")
        steps_.push_back([this] { return preload_hdr_session(); });
    if (cfg->get_preload_toolbar()) steps_.push_back([this] { return preload_toolbar_assets(); });
    if (cfg->get_preload_ocr()) steps_.push_back([this] { return preload_ocr_engine(); });
    if (cfg->get_preload_settings()) steps_.push_back([this] {
        preload_settings();
        return false;
    });
    // Last: the main UI and a working-set trim, always.
    steps_.push_back([this] { return show_main_window_on_start(); });
    steps_.push_back([] {
        request_trim_working_set(1000);
        return false;
    });
    // Starts after 50 ms so the event loop settles first.
    QTimer::singleShot(50, app_, [this] { run_next(); });
}

void PreloadManager::run_next() {
    if (steps_.empty()) return;
    auto step = std::move(steps_.front());
    steps_.pop_front();
    bool is_async = false;
    try {
        is_async = step();
    } catch (const std::exception &e) {
        log_exception(e, T("预加载步骤执行失败"));
        is_async = false;
    }
    // Synchronous (or failed) steps continue one event loop turn later, so the main
    // thread is never held for long.
    if (!is_async) QTimer::singleShot(0, app_, [this] { run_next(); });
}

bool PreloadManager::preload_toolbar_assets() {
    // Warms the capture toolbar on the main thread so the first capture does not stall building it.
    try {
        auto *dummy_parent = new QWidget();
        dummy_parent->hide();
        auto *toolbar = new Toolbar(dummy_parent);
        toolbar->hide();
        toolbar->deleteLater();
        dummy_parent->deleteLater();
        log_debug(T("工具栏预加载完成"), "Preload");
    } catch (const std::exception &e) {
        log_warning(T("工具栏预加载失败: {e}", {{"e", QString::fromUtf8(e.what())}}), "Preload");
    }
    return false;
}

bool PreloadManager::preload_hdr_session() {
    // Builds the HDR session in the background without blocking the UI; under auto only
    // when a display has HDR on.
    warm_up_hdr_session(config_->get_capture_engine());
    return false;
}

bool PreloadManager::seed_bundled_runtime() {
    // A write of some 16 MB, only when models/ has no usable DLL (first start, or after an update
    // from a package that linked ONNX Runtime in); off the main thread.
    auto *thread = QThread::create([] {
        QString written, error;
        switch (bundled_runtime::seed_embedded(&written, &error)) {
        case bundled_runtime::SeedResult::Written: log_info(T("已把内置的 ONNX Runtime 放到 {path}", {{"path", QDir::toNativeSeparators(written)}}), "OCR"); break;
        case bundled_runtime::SeedResult::Failed: log_warning(T("无法放置内置的 ONNX Runtime: {error}", {{"error", error}}), "OCR"); break;
        case bundled_runtime::SeedResult::Unneeded: break;
        }
    });
    thread->setParent(app_);
    ocr_preload_thread_ = thread;
    QObject::connect(thread, &QThread::finished, app_, [this] { run_next(); });
    thread->start();
    return true;
}

bool PreloadManager::apply_ocr_engine_setting() {
    // No download runs yet at startup, so leftovers of unfinished ones can go.
    ppocr_models::clean_staging();
    set_ppocr_model(config_->get_ppocr_model());
    set_ocr_engine(config_->get_ocr_engine());
    return false;
}

bool PreloadManager::preload_ocr_engine() {
    // Loads the OCR engine on a worker thread so the main thread is never blocked.
    log_info(T("开始在后台线程预加载 OCR 模块和引擎..."), "OCR");
    auto *thread = QThread::create([] {
        try {
            if (!is_ocr_available()) {
                log_debug(T("OCR 模块不可用（无OCR版本）"), "OCR");
                return;
            }
            if (initialize_ocr()) log_info(T("OCR 预加载成功"), "OCR");
            else log_warning(T("OCR 引擎预加载失败"), "OCR");
        } catch (const std::exception &e) {
            log_warning(T("OCR 预加载失败: {e}", {{"e", QString::fromUtf8(e.what())}}), "OCR");
        }
    });
    thread->setParent(app_);
    ocr_preload_thread_ = thread;
    QObject::connect(thread, &QThread::finished, app_, [this] { run_next(); });
    thread->start();
    return true;
}

void PreloadManager::preload_settings() { app_->preload_settings(); }

bool PreloadManager::register_entry_points() {
    // The first run registers them after the welcome wizard, which chooses the hotkeys.
    if (config_->is_first_run()) return false;
    try {
        app_->update_hotkey();
        app_->setup_tray();
        app_->setup_pin_tray_updates();
    } catch (const std::exception &e) {
        log_exception(e, T("启动时显示主界面"));
    }
    return false;
}

bool PreloadManager::show_main_window_on_start() {
    try {
        // The first run shows the welcome wizard.
        if (config_->is_first_run()) {
            app_->hotkey_system->unregister_all();
            WelcomeWizard wizard(config_);
            exec_dialog(&wizard);
            app_->sync_game_mode_monitor();
            app_->update_hotkey();
            app_->setup_tray();
            app_->setup_pin_tray_updates();
            return false;
        }
        if (config_->should_show_main_window_on_start())
            app_->open_settings();
        else
            preload_clipboard_window();
    } catch (const std::exception &e) {
        log_exception(e, T("启动时显示主界面"));
    }
    config_->mark_as_run();
    return false;
}

void PreloadManager::init_clipboard_manager() { app_->set_clipboard_monitoring_enabled(config_->get_clipboard_enabled()); }

void PreloadManager::preload_clipboard_window() {
    if (!config_->get_clipboard_enabled()) {
        log_debug(T("剪贴板功能已禁用，跳过剪贴板窗口预创建"), "Clipboard");
        return;
    }
    try {
        if (app_->clipboard_window) return;
        app_->clipboard_window = new clipboard::ClipboardWindow;
        app_->clipboard_window->hide();
        log_debug(T("剪贴板窗口预创建完成（未显示）"), "Clipboard");
    } catch (const std::exception &e) {
        log_warning(T("剪贴板窗口预创建失败: {e}", {{"e", QString::fromUtf8(e.what())}}), "Clipboard");
    }
}

void PreloadManager::wait_for_threads(unsigned long timeout_ms) {
    if (ocr_preload_thread_ && ocr_preload_thread_->isRunning()) ocr_preload_thread_->wait(timeout_ms);
}

bool PreloadManager::threads_running() const { return ocr_preload_thread_ && ocr_preload_thread_->isRunning(); }

int run(int argc, char **argv) {
    // 1. Environment, before Qt.
    setup_environment();
    // 2./3. Module paths and pinning onefile resources do not apply to a native build.
    // 4. The taskbar identity, before QApplication exists.
    set_app_user_model_id("jietuba.app");
    // 5. On the first start with a data folder, what older versions stored elsewhere, before anything reads it.
    data_folder::migrate(data_location(), data_folder::default_sources(data_location()));
    // 6. In admin mode the elevated instance started by the scheduled task runs instead.
    if (open_local_settings()->value("app/run_as_admin", false).toBool() && hand_off_to_admin_task())
        return 0;
    // 7. Single instance.
    ensure_single_instance();
    // 8. The application.
    MainApp main(argc, argv);
    return main.run();
}
}
