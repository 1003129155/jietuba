#include "ocr/ocr_manager.h"
#include "core/logger.h"
#include "ocr/ocr_bands.h"
#include "ocr/ocr_line_geometry.h"
#include "ocr/ppocr_models.h"
#include "settings/tool_settings.h"
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTransform>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>
#include <vector>
#include <windows.h>
#include "core/native.h"

namespace jietuba {
namespace {
void ocr_log(const LogMsg &message, const QString &level = "INFO") {
    if (level == "ERROR") log_error(message, "OCR");
    else if (level == "WARN") log_warning(message, "OCR");
    else if (level == "DEBUG") log_debug(message, "OCR");
    else log_info(message, "OCR");
}

using SnippingToolPackage = std::optional<std::pair<QString, QString>>;
constexpr std::chrono::milliseconds SNIPPING_TOOL_RESCAN{std::chrono::seconds(30)};

// The Snipping Tool OCR needs a Snipping Tool with OCR files (Windows 11). It can be installed,
// removed or updated while running; an update moves the files to a new versioned directory.
struct SnippingToolLookup {
    std::mutex lock;
    std::function<SnippingToolPackage()> finder = find_snipping_tool;
    std::chrono::milliseconds interval = SNIPPING_TOOL_RESCAN;
    SnippingToolPackage package;
    std::optional<std::chrono::steady_clock::time_point> checked_at;
};

SnippingToolLookup &snipping_tool_lookup() {
    static SnippingToolLookup lookup;
    return lookup;
}

// Looked up again at most once per interval, while missing or once the found files are gone;
// force looks up now.
SnippingToolPackage snipping_tool(bool force = false) {
    auto &lookup = snipping_tool_lookup();
    std::lock_guard lock(lookup.lock);
    const auto now = std::chrono::steady_clock::now();
    const bool first = !lookup.checked_at;
    if (!force && !first && now - *lookup.checked_at < lookup.interval) return lookup.package;
    lookup.checked_at = now;
    if (!force && lookup.package && snipping_tool_files_present(lookup.package->second)) return lookup.package;
    auto found = lookup.finder();
    if (first || found != lookup.package) {
        if (found) ocr_log(T("截图工具 OCR 可用: {package}", {{"package", found->first}}), "DEBUG");
        else ocr_log(T("截图工具 OCR 不可用（未安装截图工具或缺少 OCR 文件）"), "DEBUG");
    }
    lookup.package = std::move(found);
    return lookup.package;
}

std::optional<bool> &oneocr_override() {
    static std::optional<bool> available;
    return available;
}

// A loaded engine works from its own copy of the files, so it is not looked up again.
bool oneocr_available(bool loaded) { return oneocr_override().value_or(loaded || snipping_tool().has_value()); }

// The ort used by the reference's ppocr_rust needs ONNX Runtime 1.24 or later (API version 24).
constexpr int ORT_MIN_MINOR = 24;

// Read from the version resource without loading the library: loading an unsupported build can
// hang instead of failing.
bool runtime_supported(const QString &path) {
    const auto file = QDir::toNativeSeparators(path).toStdWString();
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &ignored);
    if (!size) return false;
    std::vector<char> data(size);
    VS_FIXEDFILEINFO *info = nullptr;
    UINT length = 0;
    if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data()) || !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&info), &length) || !info)
        return false;
    return HIWORD(info->dwFileVersionMS) == 1 && LOWORD(info->dwFileVersionMS) >= ORT_MIN_MINOR;
}

// models/onnxruntime.dll is usable. Checked on each call: a download can add it while running.
bool runtime_ready() {
    const QString path = ppocr_models::runtime_path();
    return QFileInfo::exists(path) && runtime_supported(path);
}

PpocrFiles files_of(const QJsonObject &model) { return {model.value("det").toString(), model.value("rec").toString(), model.value("dict").toString()}; }

QString configured_preference() { return get_tool_settings_manager()->get_ocr_engine(); }
}

// One PP-OCR engine holding det and rec models; the first one loads models/onnxruntime.dll.
class PpocrEngine {
public:
    explicit PpocrEngine(const PpocrFiles &files) {
        char error[512] = {};
        if (!jb_ppocr_init_runtime(QDir::toNativeSeparators(ppocr_models::runtime_path()).toUtf8().constData(), error, sizeof error)) throw std::runtime_error(error);
        const QByteArray dict = QDir::toNativeSeparators(files.dict).toUtf8();
        engine_ = jb_ppocr_open(QDir::toNativeSeparators(files.det).toUtf8().constData(), QDir::toNativeSeparators(files.rec).toUtf8().constData(),
                                files.dict.isEmpty() ? nullptr : dict.constData(), error, sizeof error);
        if (!engine_) throw std::runtime_error(error);
    }
    ~PpocrEngine() { jb_ppocr_free(engine_); }
    void close() { jb_ppocr_close(engine_); }
    // RGB888 rows of `stride` bytes; corners, text, score, direction and characters per line.
    // A set cancelled stops it with no lines.
    QVector<OcrLine> recognize(const uchar *rgb, size_t len, int w, int h, int stride, const std::atomic_bool *cancelled) {
        JbOcrLine *lines = nullptr;
        size_t count = 0;
        char error[512] = {};
        const auto asked = [](const void *flag) { return static_cast<const std::atomic_bool *>(flag)->load(); };
        if (!jb_ppocr_recognize(engine_, rgb, len, w, h, stride, cancelled ? +asked : nullptr, cancelled, &lines, &count, error, sizeof error))
            throw std::runtime_error(error);
        QVector<OcrLine> result;
        for (size_t index = 0; index < count; ++index) {
            OcrLine line;
            for (int corner = 0; corner < 4; ++corner) line.box.append(QPointF(lines[index].points[corner * 2], lines[index].points[corner * 2 + 1]));
            line.text = QString::fromUtf8(lines[index].text);
            line.score = lines[index].score;
            line.vertical = lines[index].vertical;
            line.chars.reserve(static_cast<qsizetype>(lines[index].char_count));
            for (size_t c = 0; c < lines[index].char_count; ++c) {
                const auto &span = lines[index].chars[c];
                line.chars.append(OcrCharSpan{span.start, span.center, span.end});
            }
            result.append(line);
        }
        jb_ppocr_lines_free(lines, count);
        return result;
    }
private:
    JbPpocr *engine_ = nullptr;
};

QString get_ppocr_status() {
    if (!OCRManager::instance().selected_ppocr_model()) return "missing_models";
    return runtime_ready() ? "available" : "missing_engine";
}

bool ppocr_runtime_supported(const QString &dll) { return runtime_supported(dll); }

void set_oneocr_available_for_test(std::optional<bool> available) { oneocr_override() = available; }

void set_snipping_tool_finder_for_test(std::function<std::optional<std::pair<QString, QString>>()> finder, std::chrono::milliseconds rescan) {
    auto &lookup = snipping_tool_lookup();
    std::lock_guard lock(lookup.lock);
    lookup.interval = finder ? rescan : SNIPPING_TOOL_RESCAN;
    lookup.finder = finder ? std::move(finder) : find_snipping_tool;
    lookup.package.reset();
    lookup.checked_at.reset();
}

OCRManager::OCRManager() = default;

OCRManager &OCRManager::instance() {
    static OCRManager manager;
    return manager;
}

bool OCRManager::is_available() const { return oneocr_available(one_loaded_) || ppocr_ready(); }

QStringList OCRManager::get_available_engines() const {
    QStringList engines;
    if (oneocr_available(one_loaded_)) engines.append(ENGINE_ONEOCR);
    if (ppocr_ready()) engines.append(ENGINE_PP_RUST);
    return engines;
}

std::optional<QJsonObject> OCRManager::selected_ppocr_model() const {
    if (configured() == ENGINE_AUTO) return ppocr_models::auto_choice();
    const auto model = chosen_model();
    if (model && !is_broken(*model)) return model;
    const auto fallback = ppocr_models::auto_choice();
    if (fallback && !is_broken(*fallback)) {
        QString model_id;
        bool first = false;
        {
            std::lock_guard lock(state_lock_);
            model_id = pp_model_id_.value_or(QString());
            first = !fallback_logged_.contains(model_id);
            fallback_logged_.insert(model_id);
        }
        if (first) ocr_log(T("所选 PP-OCR 模型不可用，改用自动选择: {model}", {{"model", model_id.isEmpty() ? QString("default") : model_id}}), "WARN");
        return fallback;
    }
    return model;
}

QString OCRManager::configured() const {
    {
        std::lock_guard lock(state_lock_);
        if (preference_) return *preference_;
    }
    return configured_preference();
}

std::optional<QJsonObject> OCRManager::chosen_model() const {
    QString model_id;
    {
        std::lock_guard lock(state_lock_);
        if (!pp_model_id_) pp_model_id_ = get_tool_settings_manager()->get_ppocr_model();
        model_id = *pp_model_id_;
    }
    return ppocr_models::resolve(model_id);
}

QString OCRManager::signature(const QJsonObject &model) {
    QStringList parts;
    const auto files = files_of(model);
    for (const auto &path : {files.det, files.rec, files.dict}) {
        if (path.isEmpty()) continue;
        const QFileInfo info(path);
        parts << (info.exists() ? QString("%1|%2|%3").arg(path).arg(info.size()).arg(info.lastModified().toMSecsSinceEpoch()) : path + "|-|-");
    }
    return parts.join('\n');
}

bool OCRManager::is_broken(const QJsonObject &model) const {
    const auto key = signature(model);
    std::lock_guard lock(state_lock_);
    return broken_.contains(key);
}

qsizetype OCRManager::broken_count() const {
    std::lock_guard lock(state_lock_);
    return broken_.size();
}

bool OCRManager::falls_back(const QString &preference) const {
    if (preference == ENGINE_ONEOCR) return !oneocr_available(one_loaded_);
    if (preference != ENGINE_PP_RUST) return false;
    const auto model = chosen_model();
    return !model || is_broken(*model) || !runtime_ready();
}

QString OCRManager::missing_reason() { return get_ppocr_status() == "missing_engine" ? QString("no_runtime") : QString("no_model"); }

void OCRManager::set_last_reason(const QString &reason) {
    std::lock_guard lock(state_lock_);
    last_reason_ = reason;
}

void OCRManager::set_ppocr_model(const QString &model_id) {
    std::optional<QString> preference;
    {
        std::lock_guard lock(state_lock_);
        pp_model_id_ = model_id;
        preference = preference_;
    }
    if (preference) set_engine(*preference);
}

bool OCRManager::ppocr_ready() const { return selected_ppocr_model() && runtime_ready(); }

std::optional<QString> OCRManager::resolve_engine(const QString &preference) const {
    if (!ENGINE_PREFERENCES.contains(preference)) return std::nullopt;
    if (preference != ENGINE_AUTO && !falls_back(preference)) return preference;
    const auto available = get_available_engines();
    return available.isEmpty() ? std::nullopt : std::optional<QString>(available.front());
}

std::optional<QString> OCRManager::get_current_engine() const {
    std::lock_guard lock(state_lock_);
    return current_engine_;
}

QString OCRManager::get_last_error() const {
    std::lock_guard lock(state_lock_);
    return last_error_.isEmpty() ? QString::fromUtf8("无错误") : last_error_;
}

void OCRManager::set_last_error(const QString &error) {
    std::lock_guard lock(state_lock_);
    last_error_ = error;
}

bool OCRManager::set_engine(const QString &engine_type) {
    if (!ENGINE_PREFERENCES.contains(engine_type)) {
        ocr_log(T("不支持的引擎类型: {engine_type}", {{"engine_type", engine_type}}), "ERROR");
        return false;
    }
    const auto target = resolve_engine(engine_type);
    std::optional<QString> previous;
    {
        std::lock_guard lock(state_lock_);
        preference_ = engine_type;
        previous = current_engine_;
        current_engine_ = target;
    }
    if (target != previous) {
        ocr_log(T("切换引擎: {old} -> {new}", {{"old", previous ? QVariant(*previous) : QVariant()}, {"new", target ? QVariant(*target) : QVariant()}}));
        if (engine_lock_.try_lock()) {
            close_stale_engines();
            engine_lock_.unlock();
        }
    }
    if (!target) {
        set_last_reason(missing_reason());
        if (engine_type == ENGINE_PP_RUST) set_last_error(get_ppocr_status() == "missing_models" ? QString::fromUtf8("PP-OCR 缺少模型") : QString::fromUtf8("PP-OCR 缺少引擎"));
        else if (engine_type == ENGINE_ONEOCR) set_last_error(QString::fromUtf8("未找到带 OCR 的 Windows 截图工具"));
        else set_last_error(QString::fromUtf8("没有可用的 OCR 引擎"));
        ocr_log(T("OCR 引擎不可用: {engine}", {{"engine", engine_type}}), "WARN");
        return false;
    }
    return true;
}

void OCRManager::ensure_preference() {
    bool unset = false;
    {
        std::lock_guard lock(state_lock_);
        unset = !preference_;
    }
    if (unset) set_engine(configured_preference());
}

bool OCRManager::initialize(const QString &engine_type) {
    if (!engine_type.isEmpty()) set_engine(engine_type);
    else ensure_preference();
    // Called on the main thread when pinning; an already loaded engine never waits for the lock.
    const auto engine = get_current_engine();
    if (engine == ENGINE_PP_RUST) return initialize_ppocr_rust();
    if (engine == ENGINE_ONEOCR) return initialize_oneocr();
    return false;
}

bool OCRManager::initialize_ppocr_rust() {
    const auto model = selected_ppocr_model();
    if (!model) {
        set_last_error(QString::fromUtf8("PP-OCR 缺少模型"));
        set_last_reason("no_model");
        ocr_log(T("ppocr_rust 已安装但缺少模型文件"), "DEBUG");
        return false;
    }
    if (!runtime_ready()) {
        set_last_error(QString::fromUtf8("ppocr_rust 引擎不可用"));
        set_last_reason("no_runtime");
        ocr_log(T("PP-OCR 缺少可用的 ONNX Runtime（需要 1.{minor} 或更新）: {path}", {{"minor", ORT_MIN_MINOR}, {"path", QDir::toNativeSeparators(ppocr_models::runtime_path())}}),
                "DEBUG");
        return false;
    }
    const PpocrFiles files = files_of(*model);
    {
        std::lock_guard lock(state_lock_);
        if (pp_loaded_ && pp_files_ == files) return true;
    }
    std::lock_guard lock(engine_lock_);
    if (pp_engine_) {
        {
            std::lock_guard state(state_lock_);
            if (pp_files_ == files) return true;
        }
        pp_loaded_ = false;
        pp_engine_->close();
        pp_engine_.reset();
    }
    try {
        ocr_log(T("正在初始化 ppocr_rust 引擎 (Rust + ort)..."), "DEBUG");
        pp_engine_ = std::make_unique<PpocrEngine>(files);
        {
            std::lock_guard state(state_lock_);
            pp_files_ = files;
        }
        pp_loaded_ = true;
        ocr_log(T("ppocr_rust 引擎初始化成功"), "DEBUG");
        return true;
    } catch (const std::exception &e) {
        set_last_error(QString::fromUtf8("ppocr_rust 初始化失败: ") + QString::fromUtf8(e.what()));
        {
            std::lock_guard state(state_lock_);
            last_reason_ = "model_broken";
            broken_.insert(signature(*model));
        }
        ocr_log(T("ppocr_rust 初始化失败: {e}\n{tb}", {{"e", QString::fromUtf8(e.what())}, {"tb", QString()}}), "ERROR");
        pp_engine_.reset();
        return false;
    }
}

bool OCRManager::initialize_oneocr() {
    if (!oneocr_available(one_loaded_)) {
        set_last_error(QString::fromUtf8("截图工具 OCR 不可用"));
        return false;
    }
    if (one_loaded_) return true;
    std::lock_guard lock(engine_lock_);
    if (one_engine_) return true;
    auto package = snipping_tool();
    // A failed load looks the package up again and retries once if it moved (a Store update).
    for (bool retried = false;; retried = true) {
        if (!package) {
            set_last_error(QString::fromUtf8("截图工具 OCR 不可用"));
            return false;
        }
        try {
            // The first run copies about 114 MB of files into the app data directory.
            ocr_log(T("正在初始化截图工具 OCR..."), "DEBUG");
            one_engine_ = std::make_unique<SnippingToolOcr>(*package);
            one_loaded_ = true;
            ocr_log(T("截图工具 OCR 初始化成功"), "DEBUG");
            return true;
        } catch (const std::exception &e) {
            set_last_error(QString::fromUtf8("截图工具 OCR 初始化失败: ") + QString::fromUtf8(e.what()));
            ocr_log(T("截图工具 OCR 初始化失败: {e}\n{tb}", {{"e", QString::fromUtf8(e.what())}, {"tb", QString()}}), "ERROR");
            one_engine_.reset();
        }
        if (retried) return false;
        auto found = snipping_tool(true);
        if (found == package) return false;
        package = std::move(found);
    }
}

void OCRManager::close_stale_engines() {
    const auto current = get_current_engine();
    if (current != ENGINE_PP_RUST && pp_engine_) {
        pp_loaded_ = false;
        pp_engine_->close();
        pp_engine_.reset();
        ocr_log(T("已释放 OCR 引擎: {engine}", {{"engine", ENGINE_PP_RUST}}));
    }
    if (current != ENGINE_ONEOCR && one_engine_) {
        one_loaded_ = false;
        one_engine_->close();
        one_engine_.reset();
        ocr_log(T("已释放 OCR 引擎: {engine}", {{"engine", ENGINE_ONEOCR}}));
    }
}

OcrResult OCRManager::recognize(const QImage &image, const std::atomic_bool *cancelled) {
    ensure_preference();
    std::lock_guard lock(engine_lock_);
    set_last_reason({});
    OcrResult result;
    // Model files can be deleted or replaced between recognitions, so the choice is made again
    // each time; a model that has just failed to load is marked broken, and the next choice
    // takes the automatic one instead.
    for (int attempt = 0; attempt < 2; ++attempt) {
        set_engine(configured());
        const auto engine = get_current_engine();
        if (engine == ENGINE_ONEOCR) {
            result = recognize_with_oneocr(image, cancelled);
            // The Snipping Tool went away during the load: the next choice skips it.
            if (result.code == 100 || oneocr_available(one_loaded_)) break;
            continue;
        }
        if (engine != ENGINE_PP_RUST) {
            result = format_error();
            break;
        }
        const auto broken = broken_count();
        result = recognize_with_ppocr_rust(image, cancelled);
        if (broken_count() == broken) break;
    }
    close_stale_engines();
    return result;
}

OcrResult OCRManager::recognize_with_ppocr_rust(const QImage &source, const std::atomic_bool *cancelled) {
    // Inference runs on the native side, off the UI thread's work.
    if (!initialize_ppocr_rust()) return format_error();
    try {
        QElapsedTimer clock;
        clock.start();
        if (source.isNull()) return format_empty_result();
        const auto image = source.format() == QImage::Format_RGB888 ? source : source.convertToFormat(QImage::Format_RGB888);
        if (!pp_engine_) return format_error(QString::fromUtf8("ppocr_rust 引擎已释放"));
        // The pixels go in place; the call is synchronous, so the image outlives it.
        const auto lines = pp_engine_->recognize(image.constBits(), static_cast<size_t>(image.sizeInBytes()), image.width(), image.height(),
                                                 static_cast<int>(image.bytesPerLine()), cancelled);
        const double elapse = clock.nsecsElapsed() / 1e9;
        QVector<OcrLine> results;
        for (const auto &line : lines)
            if (!line.text.isEmpty()) results.append(line);
        if (results.isEmpty()) return format_empty_result();
        return format_result(results, elapse);
    } catch (const std::exception &e) {
        ocr_log(T("ppocr_rust 识别失败: {e}\n{tb}", {{"e", QString::fromUtf8(e.what())}, {"tb", QString()}}), "ERROR");
        return format_error(QString::fromUtf8("ppocr_rust 识别失败: ") + QString::fromUtf8(e.what()));
    }
}

OcrResult OCRManager::recognize_with_oneocr(const QImage &image, const std::atomic_bool *cancelled) {
    if (!one_engine_ && !initialize_oneocr()) return format_error();
    try {
        QElapsedTimer clock;
        clock.start();
        if (image.isNull()) return format_empty_result();
        // Bands overlap by 200 px, so lines up to that tall stay whole where a band ends.
        const auto lines = recognize_in_bands(image, SNIPPING_TOOL_MAX_SIDE, 100, [this](const QImage &band) { return one_engine_->recognize(band); }, cancelled);
        const double elapse = clock.nsecsElapsed() / 1e9;
        if (lines.isEmpty()) return format_empty_result();
        return format_result(lines, elapse);
    } catch (const std::exception &e) {
        ocr_log(T("截图工具 OCR 识别失败: {e}\n{tb}", {{"e", QString::fromUtf8(e.what())}, {"tb", QString()}}), "ERROR");
        return format_error(QString::fromUtf8("截图工具 OCR 识别失败: ") + QString::fromUtf8(e.what()));
    }
}

OcrResult OCRManager::format_result(const QVector<OcrLine> &lines, double elapse) { return OcrResult{100, QString::fromUtf8("成功"), lines, elapse}; }

OcrResult OCRManager::format_empty_result() { return OcrResult{100, QString::fromUtf8("未识别到文字"), {}, 0.0}; }

OcrResult OCRManager::format_error(const QString &error_msg) const {
    QString msg = error_msg;
    QString reason;
    {
        std::lock_guard lock(state_lock_);
        if (msg.isEmpty()) {
            msg = last_error_;
            reason = last_reason_;
        }
    }
    if (msg.isEmpty()) msg = QString::fromUtf8("OCR 不可用");
    return OcrResult{-1, msg, {}, 0.0, reason.isEmpty() ? QString("failed") : reason};
}

void OCRManager::release_engine() {
    std::lock_guard lock(engine_lock_);
    if (pp_engine_) {
        pp_loaded_ = false;
        pp_engine_->close();
        pp_engine_.reset();
    }
    if (one_engine_) {
        one_loaded_ = false;
        one_engine_->close();
        one_engine_.reset();
    }
    ocr_log(T("OCR 管理器状态已重置"));
}

bool OCRManager::is_engine_loaded() const {
    const auto current = get_current_engine();
    if (current == ENGINE_PP_RUST) return pp_loaded_;
    if (current == ENGINE_ONEOCR) return one_loaded_;
    return false;
}

QString OCRManager::get_memory_status() const {
    if (!is_engine_loaded()) return QString::fromUtf8("未初始化");
    if (get_current_engine() == ENGINE_PP_RUST) return QString::fromUtf8("已初始化 (ppocr_rust Rust+ort 引擎)");
    return QString::fromUtf8("已初始化 (截图工具 OCR)");
}

namespace {
std::mutex &recognizer_lock() {
    static std::mutex lock;
    return lock;
}

std::function<OcrResult(const QImage &)> &recognizer_override() {
    static std::function<OcrResult(const QImage &)> recognizer;
    return recognizer;
}

std::function<OcrResult(const QImage &)> current_recognizer() {
    std::lock_guard lock(recognizer_lock());
    return recognizer_override();
}
}

void set_recognizer_for_test(std::function<OcrResult(const QImage &)> recognizer) {
    std::lock_guard lock(recognizer_lock());
    recognizer_override() = std::move(recognizer);
}

bool is_ocr_available() { return current_recognizer() || OCRManager::instance().is_available(); }
QStringList get_available_engines() { return OCRManager::instance().get_available_engines(); }
bool set_ocr_engine(const QString &engine_type) { return OCRManager::instance().set_engine(engine_type); }
void set_ppocr_model(const QString &model_id) { OCRManager::instance().set_ppocr_model(model_id); }
std::optional<QString> get_current_engine() { return OCRManager::instance().get_current_engine(); }
bool initialize_ocr(const QString &engine_type) { return current_recognizer() || OCRManager::instance().initialize(engine_type); }
OcrResult recognize_text(const QImage &image, const std::atomic_bool *cancelled) {
    if (auto recognizer = current_recognizer()) return recognizer(image);
    return OCRManager::instance().recognize(image, cancelled);
}
void release_ocr_engine() { OCRManager::instance().release_engine(); }
QString get_ocr_memory_status() { return OCRManager::instance().get_memory_status(); }

namespace {
// Blocks of one line sorted by x, joined with spaces; the box is their union.
OcrTextLine join_line(QVector<OcrTextLine> blocks) {
    std::stable_sort(blocks.begin(), blocks.end(), [](const OcrTextLine &a, const OcrTextLine &b) { return a.left < b.left; });
    OcrTextLine line = blocks.front();
    QStringList texts;
    for (const auto &block : blocks) {
        texts.append(block.text);
        line.left = std::min(line.left, block.left);
        line.right = std::max(line.right, block.right);
        line.top = std::min(line.top, block.top);
        line.bottom = std::max(line.bottom, block.bottom);
    }
    line.text = texts.join(' ');
    return line;
}

// Turns the page so its lines run level: the median direction of the lines read across.
QTransform level_page(const OcrResult &result) {
    std::vector<double> angles;
    for (const auto &item : result.data) {
        if (item.vertical || item.text.isEmpty()) continue;
        const auto frame = line_frame(item.box, false);
        if (frame.valid()) angles.push_back(std::atan2(frame.along.y(), frame.along.x()));
    }
    if (angles.empty()) return {};
    std::nth_element(angles.begin(), angles.begin() + angles.size() / 2, angles.end());
    return QTransform().rotateRadians(-angles[angles.size() / 2]);
}
}

QVector<OcrTextLine> group_ocr_lines(const OcrResult &result) {
    if (result.code != 100 || result.data.isEmpty()) return {};
    const QTransform level = level_page(result);
    QVector<OcrTextLine> blocks;
    for (const auto &item : result.data) {
        if (item.text.isEmpty()) continue;
        if (item.box.isEmpty()) {
            // A single block without coordinates still gives its text: no layout, but nothing lost.
            if (result.data.size() == 1) blocks.append(OcrTextLine{item.text, 0, 0, 0, 0});
            continue;
        }
        const QRectF bounds = level.map(QPolygonF(item.box)).boundingRect();
        blocks.append(OcrTextLine{item.text, bounds.left(), bounds.right(), bounds.top(), bounds.bottom()});
    }
    if (blocks.isEmpty()) return {};
    // Blocks whose vertical centres differ by less than this share a line.
    double total_height = 0;
    for (const auto &block : blocks) total_height += block.bottom - block.top;
    const double tolerance = total_height / blocks.size() * 0.8;
    const auto center = [](const OcrTextLine &block) { return (block.top + block.bottom) / 2; };
    std::stable_sort(blocks.begin(), blocks.end(), [&](const OcrTextLine &a, const OcrTextLine &b) { return center(a) < center(b); });
    QVector<OcrTextLine> lines, current;
    std::optional<double> current_y;
    for (const auto &block : blocks) {
        const double center_y = center(block);
        if (!current_y) {
            current = {block};
            current_y = center_y;
        } else if (std::abs(center_y - *current_y) <= tolerance) {
            current.append(block);
        } else {
            lines.append(join_line(current));
            current = {block};
            current_y = center_y;
        }
    }
    if (!current.isEmpty()) lines.append(join_line(current));
    return lines;
}

QString format_ocr_result_text(const OcrResult &result, const QString &separator) {
    QStringList texts;
    for (const auto &line : group_ocr_lines(result)) texts.append(line.text);
    return texts.join(separator);
}
}
