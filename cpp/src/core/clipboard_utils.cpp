#include "core/clipboard_utils.h"
#include "core/background_tasks.h"
#include "core/logger.h"
#include "core/save.h"
#include "settings/tool_settings.h"
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QMetaObject>
#include <QThread>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <stdexcept>
#include <thread>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace jietuba {
namespace {
std::mutex clipboard_write_lock;
constexpr unsigned long ERROR_ACCESS_DENIED_CODE = 5;
constexpr double CLIPBOARD_WRITE_RETRY_DELAYS[] = {0.0, 0.02, 0.05, 0.1, 0.2, 0.35};

struct ClipboardError : std::runtime_error {
    ClipboardError(const char *function, unsigned long code)
        : std::runtime_error(QString("(%1, '%2', ...)").arg(code).arg(QLatin1String(function)).toStdString()), funcname(function), winerror(code) {}
    const char *funcname;
    unsigned long winerror;
};

// Another program holding the clipboard makes OpenClipboard fail with ERROR_ACCESS_DENIED.
bool is_clipboard_busy_error(const ClipboardError &error) {
    return std::strcmp(error.funcname, "OpenClipboard") == 0 && error.winerror == ERROR_ACCESS_DENIED_CODE;
}

QObject *main_thread_relay = nullptr;

// Non-Windows platforms or a failed Win32 write. QClipboard only works on the
// main thread, so background callers are relayed there.
void copy_qt_fallback(const QImage &image) {
    if (QThread::currentThread() != qApp->thread()) {
        if (!main_thread_relay) {
            log_warning(T("剪切板: 后台线程无法回退到 Qt 写入"), "Clipboard");
            return;
        }
        QMetaObject::invokeMethod(main_thread_relay, [image] { copy_qt_fallback(image); }, Qt::QueuedConnection);
        return;
    }
    QApplication::clipboard()->setImage(image);
    log_info(T("已复制到剪切板 (Qt)"), "Clipboard");
}

#ifdef Q_OS_WIN
// One transaction for all formats, so only one change notification fires.
void write_clipboard(const std::vector<std::pair<UINT, QByteArray>> &formats) {
    std::lock_guard guard(clipboard_write_lock);
    if (!OpenClipboard(nullptr)) throw ClipboardError("OpenClipboard", GetLastError());
    struct Closer { ~Closer() { CloseClipboard(); } } closer;
    if (!EmptyClipboard()) throw ClipboardError("EmptyClipboard", GetLastError());
    for (const auto &[format_id, data] : formats) {
        HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(data.size()));
        if (!handle) throw ClipboardError("GlobalAlloc", GetLastError());
        void *target = GlobalLock(handle);
        if (!target) {
            const auto code = GetLastError();
            GlobalFree(handle);
            throw ClipboardError("GlobalLock", code);
        }
        std::memcpy(target, data.constData(), static_cast<size_t>(data.size()));
        GlobalUnlock(handle);
        if (!SetClipboardData(format_id, handle)) {
            const auto code = GetLastError();
            GlobalFree(handle);
            throw ClipboardError("SetClipboardData", code);
        }
    }
}

void run_clipboard_write_with_retry(const std::function<void()> &operation, const QString &path_name) {
    const int total_attempts = static_cast<int>(std::size(CLIPBOARD_WRITE_RETRY_DELAYS));
    for (int attempt_index = 1; attempt_index <= total_attempts; ++attempt_index) {
        const double delay = CLIPBOARD_WRITE_RETRY_DELAYS[attempt_index - 1];
        if (delay > 0) std::this_thread::sleep_for(std::chrono::duration<double>(delay));
        try {
            operation();
            return;
        } catch (const ClipboardError &error) {
            if (!is_clipboard_busy_error(error) || attempt_index == total_attempts) throw;
            log_debug(T("剪贴板: {path_name} 写入时剪贴板被占用，准备重试 {attempt_next}/{total_attempts}",
                        {{"path_name", path_name}, {"attempt_next", attempt_index + 1}, {"total_attempts", total_attempts}}), "Clipboard");
        }
    }
}

// Encodes once; a busy clipboard only retries the write.
void copy_win32(const QImage &image, const QString &file_reference) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const auto dibv5_data = build_dibv5(image);
    const auto t1 = clock::now();
    const auto png_data = build_png(image);
    const auto t2 = clock::now();
    std::vector<std::pair<UINT, QByteArray>> formats = {{RegisterClipboardFormatW(L"PNG"), png_data}, {CF_DIBV5, dibv5_data}};
    if (!file_reference.isEmpty()) formats.emplace_back(CF_HDROP, build_hdrop(file_reference));
    run_clipboard_write_with_retry([&] { write_clipboard(formats); }, "Win32");
    const auto t3 = clock::now();
    auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    log_debug(T("已复制到剪切板 (Win32) dibv5={dibv5_ms:.1f}ms png={png_ms:.1f}ms win32={win32_ms:.1f}ms",
                {{"dibv5_ms", ms(t0, t1)}, {"png_ms", ms(t1, t2)}, {"win32_ms", ms(t2, t3)}}), "Clipboard");
    log_info(T("已复制到剪切板 (Win32 CF_DIBV5 + PNG)"), "Clipboard");
}
#endif

// The clipboard queue; its thread exits when the queue drains so an update never waits on it.
std::mutex clipboard_jobs_lock;
std::condition_variable clipboard_idle_cv;
std::deque<std::function<void()>> clipboard_jobs;
bool clipboard_idle = true;
bool clipboard_worker_running = false;

void run_clipboard_jobs() {
    while (true) {
        std::function<void()> job;
        {
            std::lock_guard guard(clipboard_jobs_lock);
            if (clipboard_jobs.empty()) {
                clipboard_worker_running = false;
                clipboard_idle = true;
                clipboard_idle_cv.notify_all();
                return;
            }
            job = std::move(clipboard_jobs.front());
            clipboard_jobs.pop_front();
        }
        try {
            job();
        } catch (const std::exception &e) {
            log_warning(T("图像投递: 后台任务失败 ({exc})", {{"exc", exception_text(e)}}), "Clipboard");
        }
    }
}

void enqueue_clipboard_job(std::function<void()> job) {
    {
        std::lock_guard guard(clipboard_jobs_lock);
        clipboard_jobs.push_back(std::move(job));
        clipboard_idle = false;
        if (clipboard_worker_running) return;
        clipboard_worker_running = true;
    }
    try {
        background_tasks::start_thread(&run_clipboard_jobs, "ClipboardWrite");
    } catch (const std::runtime_error &) {
        // While preparing an update no new threads start; run inline so the copy still happens.
        run_clipboard_jobs();
    }
}

void start_save(const std::shared_ptr<Delivery> &delivery, const QImage &image, const std::shared_ptr<SaveService> &save_service,
                const SaveKwargs &save_kwargs, const QString &target_path, const QImage &original) {
    auto worker = [delivery, image, save_service, save_kwargs, target_path, original] {
        const auto started = std::chrono::steady_clock::now();
        try {
            const auto [save_ok, saved_path] = save_service->save_qimage(image, save_kwargs.directory, save_kwargs.prefix, {},
                                                                         save_kwargs.image_format, SaveService::DEFAULT_PDF_DPI, target_path);
            if (save_ok && !original.isNull() && saved_path) SaveService::save_beside(original, *saved_path, "original");
            log_debug(T("图像投递: 后台保存完成 save={save_ok} {ms:.1f}ms",
                        {{"save_ok", save_ok}, {"ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count()}}), "Clipboard");
        } catch (const std::exception &e) {
            log_warning(T("图像投递: 后台任务失败 ({exc})", {{"exc", exception_text(e)}}), "Clipboard");
        }
        delivery->part_done();
    };
    try {
        background_tasks::start_thread(worker, "ClipboardDeliverSave");
    } catch (const std::runtime_error &e) {
        log_warning(T("图像投递: 后台任务失败 ({exc})", {{"exc", exception_text(e)}}), "Clipboard");
        delivery->part_done();
    }
}

bool auto_save_enabled(ToolSettingsManager *config_manager, const QString &trigger) {
    if (!config_manager) return false;
    if (trigger == "capture") return config_manager->get_screenshot_save_enabled();
    if (trigger == "pin") return config_manager->get_screenshot_pin_save_enabled();
    throw std::invalid_argument(trigger.toStdString());
}
}

void copy_image_to_clipboard(const QImage &image, const QString &file_reference) {
    if (image.isNull()) {
        log_warning(T("剪切板: 图像为空"), "Clipboard");
        return;
    }
#ifdef Q_OS_WIN
    try {
        copy_win32(image, file_reference);
        return;
    } catch (const std::exception &e) {
        log_warning(T("剪切板: Win32 写入失败 ({e})", {{"e", exception_text(e)}}), "Clipboard");
    }
#endif
    copy_qt_fallback(image);
}

void Delivery::part_done() {
    std::lock_guard guard(lock_);
    --pending_;
    if (pending_ <= 0) done_.notify_all();
}

bool Delivery::join(std::optional<double> timeout_seconds) {
    std::unique_lock guard(lock_);
    if (!timeout_seconds) {
        done_.wait(guard, [this] { return pending_ <= 0; });
        return true;
    }
    return done_.wait_for(guard, std::chrono::duration<double>(*timeout_seconds), [this] { return pending_ <= 0; });
}

bool Delivery::is_alive() {
    std::lock_guard guard(lock_);
    return pending_ > 0;
}

void ensure_main_thread_clipboard() {
    // Created on the main thread at the first delivery; background threads only post to it.
    if (!main_thread_relay) main_thread_relay = new QObject;
}

std::shared_ptr<Delivery> deliver_image_async(const QImage &image, bool copy_to_clipboard, std::shared_ptr<SaveService> save_service,
                                              const SaveKwargs &save_kwargs, bool write_file_reference, const QImage &original) {
    if (image.isNull()) {
        log_warning(T("图像投递: 图像为空，跳过"), "Clipboard");
        return nullptr;
    }
    if (!copy_to_clipboard && !save_service) {
        log_warning(T("图像投递: 未请求复制或保存，跳过"), "Clipboard");
        return nullptr;
    }
#ifndef Q_OS_WIN
    const bool windows = false;
#else
    const bool windows = true;
#endif
    if (!copy_to_clipboard || !windows) {
        if (copy_to_clipboard) {
            log_debug(T("图像投递: 非 Windows 平台，剪贴板仍走主线程回退"), "Clipboard");
            copy_image_to_clipboard(image);
        }
        if (!save_service) return nullptr;
        auto delivery = std::make_shared<Delivery>(1);
        start_save(delivery, image, save_service, save_kwargs, QString(), original);
        return delivery;
    }
    auto delivery = std::make_shared<Delivery>(save_service ? 2 : 1);
    const bool reserve = save_service && write_file_reference;
    ensure_main_thread_clipboard();
    enqueue_clipboard_job([delivery, image, save_service, save_kwargs, reserve, original] {
        // This part counts as done however the job ends.
        struct PartDone {
            Delivery &delivery;
            ~PartDone() { delivery.part_done(); }
        } part_done{*delivery};
        QString target_path;
        if (reserve) {
            try {
                target_path = save_service->reserve_save_path(save_kwargs.directory, save_kwargs.prefix, {}, save_kwargs.image_format);
            } catch (const std::exception &e) {
                // An unwritable save folder (unplugged or disconnected drive) degrades to no file reference.
                log_warning(T("图像投递: 预留保存路径失败，剪贴板不写入文件路径 ({exc})", {{"exc", exception_text(e)}}), "Clipboard");
            }
        }
        if (save_service) start_save(delivery, image, save_service, save_kwargs, target_path, original);
        copy_image_to_clipboard(image, target_path);
    });
    return delivery;
}

bool wait_for_clipboard_writes(std::optional<double> timeout_seconds) {
    std::unique_lock guard(clipboard_jobs_lock);
    if (!timeout_seconds) {
        clipboard_idle_cv.wait(guard, [] { return clipboard_idle; });
        return true;
    }
    return clipboard_idle_cv.wait_for(guard, std::chrono::duration<double>(*timeout_seconds), [] { return clipboard_idle; });
}

std::shared_ptr<Delivery> deliver_screenshot(const QImage &image, ToolSettingsManager *config_manager, bool copy_to_clipboard,
                                             const QString &trigger, const QString &file_prefix, const QImage &original_in,
                                             std::shared_ptr<SaveService> save_service) {
    SaveKwargs save_kwargs;
    bool write_file_reference = true;
    QImage original = original_in;
    if (auto_save_enabled(config_manager, trigger)) {
        if (!save_service) save_service = std::make_shared<SaveService>(*config_manager);
        save_kwargs.directory = config_manager->get_screenshot_save_path();
        save_kwargs.prefix = file_prefix;
        save_kwargs.image_format = SaveService::format_for_size(config_manager->get_screenshot_format(), image.width(), image.height());
        // A file path exists only once auto-save writes the file, so this switch is read only then.
        write_file_reference = config_manager->get_clipboard_file_reference_enabled();
        if (!config_manager->get_screenshot_save_original_enabled()) original = QImage();
    } else {
        save_service.reset();
        original = QImage();
    }
    if (!copy_to_clipboard && !save_service) return nullptr;
    return deliver_image_async(image, copy_to_clipboard, save_service, save_kwargs, write_file_reference, original);
}

QByteArray build_dibv5(const QImage &image) {
    // Non-premultiplied ARGB32; in memory that is little-endian BGRA.
    const auto img = image.convertToFormat(QImage::Format_ARGB32);
    const int w = img.width(), h = img.height();
    const quint32 stride = static_cast<quint32>(w) * 4;
    const quint32 pixel_size = stride * static_cast<quint32>(h);
    QByteArray header;
    header.reserve(124);
    auto u32 = [&](quint32 v) { header.append(reinterpret_cast<const char *>(&v), 4); };
    auto i32 = [&](qint32 v) { header.append(reinterpret_cast<const char *>(&v), 4); };
    auto u16 = [&](quint16 v) { header.append(reinterpret_cast<const char *>(&v), 2); };
    u32(124);                // bV5Size
    i32(w);                  // bV5Width
    i32(h);                  // bV5Height (positive = bottom-up)
    u16(1);                  // bV5Planes
    u16(32);                 // bV5BitCount
    u32(3);                  // bV5Compression = BI_BITFIELDS
    u32(pixel_size);         // bV5SizeImage
    i32(0); i32(0);          // pels per metre
    u32(0); u32(0);          // colours used / important
    u32(0x00FF0000); u32(0x0000FF00); u32(0x000000FF); u32(0xFF000000);  // R, G, B, A masks
    u32(0x73524742);         // bV5CSType = LCS_sRGB
    header.append(QByteArray(36, '\0'));  // bV5Endpoints
    u32(0); u32(0); u32(0);  // gamma
    u32(4);                  // bV5Intent = LCS_GM_IMAGES
    u32(0); u32(0); u32(0);  // profile data, profile size, reserved
    Q_ASSERT(header.size() == 124);
    // Flipped vertically for bottom-up rows.
    const auto flipped = img.flipped(Qt::Vertical);
    QByteArray result = header;
    result.reserve(124 + static_cast<qsizetype>(pixel_size));
    for (int y = 0; y < h; ++y) result.append(reinterpret_cast<const char *>(flipped.constScanLine(y)), static_cast<qsizetype>(stride));
    return result;
}

QByteArray build_png(const QImage &image) {
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    // Quality 50 trades ~14 ms of encoding for a ~95% smaller file than 100.
    image.save(&buffer, "PNG", 50);
    buffer.close();
    return buffer.data();
}

QByteArray build_hdrop(const QString &path) {
    // DROPFILES: pFiles = 20 (struct size), fWide = 1, then the path with a double NUL.
    QByteArray result;
    const qint32 header[5] = {20, 0, 0, 0, 1};
    result.append(reinterpret_cast<const char *>(header), sizeof header);
    result.append(reinterpret_cast<const char *>(path.utf16()), path.size() * 2);
    result.append(QByteArray(4, '\0'));
    return result;
}
}
