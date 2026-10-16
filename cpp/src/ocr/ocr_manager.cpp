#include "ocr/ocr_manager.h"
#include "core/logger.h"
#include "core/resource_manager.h"
#include "settings/tool_settings.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <algorithm>
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

// The Snipping Tool OCR needs a Snipping Tool with OCR files (Windows 11).
const std::optional<std::pair<QString, QString>> &snipping_tool() {
    static const auto package = [] {
        const auto found = find_snipping_tool();
        if (found) ocr_log(T("截图工具 OCR 可用: {package}", {{"package", found->first}}), "DEBUG");
        else ocr_log(T("截图工具 OCR 不可用（未安装截图工具或缺少 OCR 文件）"), "DEBUG");
        return found;
    }();
    return package;
}

bool oneocr_available() { return snipping_tool().has_value(); }

// The PP-OCR pack in models/ next to the executable: both models and the ONNX Runtime they run on.
struct PpocrPack {
    QString det, rec, runtime;
};

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

// The pack's status ("available", "missing_models" or "missing_engine"), read once per process.
const std::pair<QString, std::optional<PpocrPack>> &pp_pack() {
    static const auto pack = [] {
        const QDir base(ResourceManager::get_resource_path("models"));
        const PpocrPack found{base.filePath("PP-OCRv6_det_small.onnx"), base.filePath("PP-OCRv6_rec_small.onnx"), base.filePath("onnxruntime.dll")};
        if (!QFileInfo::exists(found.det) || !QFileInfo::exists(found.rec)) {
            ocr_log(T("ppocr_rust 已安装但缺少模型文件"), "DEBUG");
            return std::make_pair(QString("missing_models"), std::optional<PpocrPack>());
        }
        if (!QFileInfo::exists(found.runtime) || !runtime_supported(found.runtime)) {
            ocr_log(T("PP-OCR 缺少可用的 ONNX Runtime（需要 1.{minor} 或更新）: {path}", {{"minor", ORT_MIN_MINOR}, {"path", QDir::toNativeSeparators(found.runtime)}}), "DEBUG");
            return std::make_pair(QString("missing_engine"), std::optional<PpocrPack>());
        }
        ocr_log(T("ppocr_rust 引擎可用 (Rust + ort PP-OCR)"), "DEBUG");
        return std::make_pair(QString("available"), std::optional<PpocrPack>(found));
    }();
    return pack;
}

bool pp_rust_available() { return pp_pack().second.has_value(); }

QString configured_preference() { return get_tool_settings_manager()->get_ocr_engine(); }
}

// One PP-OCR engine holding det and rec models; the first one loads the pack's ONNX Runtime.
class PpocrEngine {
public:
    explicit PpocrEngine(const PpocrPack &pack) {
        char error[512] = {};
        if (!jb_ppocr_init_runtime(QDir::toNativeSeparators(pack.runtime).toUtf8().constData(), error, sizeof error)) throw std::runtime_error(error);
        engine_ = jb_ppocr_open(QDir::toNativeSeparators(pack.det).toUtf8().constData(), QDir::toNativeSeparators(pack.rec).toUtf8().constData(), error, sizeof error);
        if (!engine_) throw std::runtime_error(error);
    }
    ~PpocrEngine() { jb_ppocr_free(engine_); }
    void close() { jb_ppocr_close(engine_); }
    // RGB888 rows of `stride` bytes; (four corners, text, score) per line.
    QVector<OcrLine> recognize(const uchar *rgb, size_t len, int w, int h, int stride) {
        JbOcrLine *lines = nullptr;
        size_t count = 0;
        char error[512] = {};
        if (!jb_ppocr_recognize(engine_, rgb, len, w, h, stride, &lines, &count, error, sizeof error))
            throw std::runtime_error(error);
        QVector<OcrLine> result;
        for (size_t index = 0; index < count; ++index) {
            OcrLine line;
            for (int corner = 0; corner < 4; ++corner) line.box.append(QPointF(lines[index].points[corner * 2], lines[index].points[corner * 2 + 1]));
            line.text = QString::fromUtf8(lines[index].text);
            line.score = lines[index].score;
            result.append(line);
        }
        jb_ppocr_lines_free(lines, count);
        return result;
    }
private:
    JbPpocr *engine_ = nullptr;
};

QString get_ppocr_status() { return pp_pack().first; }

bool ppocr_runtime_supported(const QString &dll) { return runtime_supported(dll); }

OCRManager::OCRManager() = default;

OCRManager &OCRManager::instance() {
    static OCRManager manager;
    return manager;
}

bool OCRManager::is_available() const { return oneocr_available() || pp_rust_available(); }

QStringList OCRManager::get_available_engines() const {
    QStringList engines;
    if (oneocr_available()) engines.append(ENGINE_ONEOCR);
    if (pp_rust_available()) engines.append(ENGINE_PP_RUST);
    return engines;
}

std::optional<QString> OCRManager::resolve_engine(const QString &preference) const {
    const auto available = get_available_engines();
    if (preference == ENGINE_AUTO) return available.isEmpty() ? std::nullopt : std::optional<QString>(available.front());
    return available.contains(preference) ? std::optional<QString>(preference) : std::nullopt;
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
    if (!pp_rust_available()) {
        set_last_error(QString::fromUtf8("ppocr_rust 引擎不可用"));
        return false;
    }
    if (pp_loaded_) return true;
    std::lock_guard lock(engine_lock_);
    if (pp_engine_) return true;
    try {
        ocr_log(T("正在初始化 ppocr_rust 引擎 (Rust + ort)..."), "DEBUG");
        pp_engine_ = std::make_unique<PpocrEngine>(*pp_pack().second);
        pp_loaded_ = true;
        ocr_log(T("ppocr_rust 引擎初始化成功"), "DEBUG");
        return true;
    } catch (const std::exception &e) {
        set_last_error(QString::fromUtf8("ppocr_rust 初始化失败: ") + QString::fromUtf8(e.what()));
        ocr_log(T("ppocr_rust 初始化失败: {e}\n{tb}", {{"e", QString::fromUtf8(e.what())}, {"tb", QString()}}), "ERROR");
        pp_engine_.reset();
        return false;
    }
}

bool OCRManager::initialize_oneocr() {
    if (!oneocr_available()) {
        set_last_error(QString::fromUtf8("截图工具 OCR 不可用"));
        return false;
    }
    if (one_loaded_) return true;
    std::lock_guard lock(engine_lock_);
    if (one_engine_) return true;
    try {
        // The first run copies about 114 MB of files into the app data directory.
        ocr_log(T("正在初始化截图工具 OCR..."), "DEBUG");
        one_engine_ = std::make_unique<SnippingToolOcr>(*snipping_tool());
        one_loaded_ = true;
        ocr_log(T("截图工具 OCR 初始化成功"), "DEBUG");
        return true;
    } catch (const std::exception &e) {
        set_last_error(QString::fromUtf8("截图工具 OCR 初始化失败: ") + QString::fromUtf8(e.what()));
        ocr_log(T("截图工具 OCR 初始化失败: {e}\n{tb}", {{"e", QString::fromUtf8(e.what())}, {"tb", QString()}}), "ERROR");
        one_engine_.reset();
        return false;
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

OcrResult OCRManager::recognize(const QImage &image) {
    ensure_preference();
    std::lock_guard lock(engine_lock_);
    OcrResult result;
    const auto engine = get_current_engine();
    if (engine == ENGINE_PP_RUST) result = recognize_with_ppocr_rust(image);
    else if (engine == ENGINE_ONEOCR) result = recognize_with_oneocr(image);
    else result = format_error();
    close_stale_engines();
    return result;
}

OcrResult OCRManager::recognize_with_ppocr_rust(const QImage &source) {
    // Inference runs on the native side, off the UI thread's work.
    if (!pp_rust_available()) return format_error(QString::fromUtf8("ppocr_rust 不可用"));
    if (!pp_engine_ && !initialize_ppocr_rust()) return format_error();
    try {
        QElapsedTimer clock;
        clock.start();
        if (source.isNull()) return format_empty_result();
        const auto image = source.format() == QImage::Format_RGB888 ? source : source.convertToFormat(QImage::Format_RGB888);
        if (!pp_engine_) return format_error(QString::fromUtf8("ppocr_rust 引擎已释放"));
        // The pixels go in place; the call is synchronous, so the image outlives it.
        const auto lines = pp_engine_->recognize(image.constBits(), static_cast<size_t>(image.sizeInBytes()), image.width(), image.height(),
                                                 static_cast<int>(image.bytesPerLine()));
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

OcrResult OCRManager::recognize_with_oneocr(const QImage &image) {
    if (!one_engine_ && !initialize_oneocr()) return format_error();
    try {
        QElapsedTimer clock;
        clock.start();
        if (image.isNull()) return format_empty_result();
        const auto lines = one_engine_->recognize(image);
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
    if (msg.isEmpty()) {
        std::lock_guard lock(state_lock_);
        msg = last_error_;
    }
    if (msg.isEmpty()) msg = QString::fromUtf8("OCR 不可用");
    return OcrResult{-1, msg, {}, 0.0};
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

bool is_ocr_available() { return OCRManager::instance().is_available(); }
QStringList get_available_engines() { return OCRManager::instance().get_available_engines(); }
bool set_ocr_engine(const QString &engine_type) { return OCRManager::instance().set_engine(engine_type); }
std::optional<QString> get_current_engine() { return OCRManager::instance().get_current_engine(); }
bool initialize_ocr(const QString &engine_type) { return OCRManager::instance().initialize(engine_type); }
OcrResult recognize_text(const QImage &image) { return OCRManager::instance().recognize(image); }
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
}

QVector<OcrTextLine> group_ocr_lines(const OcrResult &result) {
    if (result.code != 100 || result.data.isEmpty()) return {};
    QVector<OcrTextLine> blocks;
    for (const auto &item : result.data) {
        if (item.text.isEmpty()) continue;
        if (item.box.isEmpty()) {
            // A single block without coordinates still gives its text: no layout, but nothing lost.
            if (result.data.size() == 1) blocks.append(OcrTextLine{item.text, 0, 0, 0, 0});
            continue;
        }
        OcrTextLine block{item.text, item.box[0].x(), item.box[0].x(), item.box[0].y(), item.box[0].y()};
        for (const auto &point : item.box) {
            block.left = std::min(block.left, point.x());
            block.right = std::max(block.right, point.x());
            block.top = std::min(block.top, point.y());
            block.bottom = std::max(block.bottom, point.y());
        }
        blocks.append(block);
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
