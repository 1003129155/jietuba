#include "ocr/snipping_tool_ocr.h"
#include "core/constants.h"
#include "ocr/ocr_line_geometry.h"
// Generated at configure time from the oneocr package (CMakeLists.txt).
#include "ocr/oneocr_model_key.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <stdexcept>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace jietuba {
namespace {
constexpr UINT32 PACKAGE_FILTER_HEAD_FLAG = 0x00000010;
constexpr LONG ERROR_INSUFFICIENT_BUFFER_CODE = 122;

// The package full name is Name_1.2.3.4_x64__PublisherId; the version compares part by part.
QList<int> package_version(const QString &full_name) {
    const auto parts = full_name.split('_');
    if (parts.size() < 2) return {};
    QList<int> version;
    for (const auto &part : parts[1].split('.')) {
        bool ok = false;
        const int value = part.toInt(&ok);
        if (!ok) return {};
        version.append(value);
    }
    return version;
}

bool has_required_files(const QString &directory) {
    for (const auto &name : SNIPPING_TOOL_REQUIRED_FILES)
        if (!QFileInfo(QDir(directory).filePath(name)).isFile()) return false;
    return true;
}

// The image the engine takes: BGRA rows.
struct OcrImage {
    int32_t type;
    int32_t width;
    int32_t height;
    int32_t reserved;
    int64_t step_size;
    const uint8_t *data;
};
// Corners from top-left clockwise.
struct OcrBoundingBox {
    float x1, y1, x2, y2, x3, y3, x4, y4;
};

QVector<QPointF> corners(const OcrBoundingBox &box) { return {QPointF(box.x1, box.y1), QPointF(box.x2, box.y2), QPointF(box.x3, box.y3), QPointF(box.x4, box.y4)}; }
}

struct SnippingToolOcr::Api {
    using CreateOcrInitOptions = int64_t(__cdecl *)(int64_t *);
    using OcrInitOptionsSetUseModelDelayLoad = int64_t(__cdecl *)(int64_t, char);
    using CreateOcrPipeline = int64_t(__cdecl *)(const char *, const char *, int64_t, int64_t *);
    using CreateOcrProcessOptions = int64_t(__cdecl *)(int64_t *);
    using OcrProcessOptionsSetMaxRecognitionLineCount = int64_t(__cdecl *)(int64_t, int64_t);
    using RunOcrPipeline = int64_t(__cdecl *)(int64_t, const OcrImage *, int64_t, int64_t *);
    using GetOcrLineCount = int64_t(__cdecl *)(int64_t, int64_t *);
    using GetOcrLine = int64_t(__cdecl *)(int64_t, int64_t, int64_t *);
    using GetOcrContent = int64_t(__cdecl *)(int64_t, const char **);
    using GetOcrBoundingBox = int64_t(__cdecl *)(int64_t, const OcrBoundingBox **);
    using GetOcrLineWordCount = int64_t(__cdecl *)(int64_t, int64_t *);
    using GetOcrWord = int64_t(__cdecl *)(int64_t, int64_t, int64_t *);
    using GetOcrWordConfidence = int64_t(__cdecl *)(int64_t, float *);
    using Release = void(__cdecl *)(int64_t);
    CreateOcrInitOptions create_init_options;
    OcrInitOptionsSetUseModelDelayLoad set_use_model_delay_load;
    CreateOcrPipeline create_pipeline;
    CreateOcrProcessOptions create_process_options;
    OcrProcessOptionsSetMaxRecognitionLineCount set_max_line_count;
    RunOcrPipeline run_pipeline;
    GetOcrLineCount get_line_count;
    GetOcrLine get_line;
    GetOcrContent get_line_content;
    GetOcrBoundingBox get_line_bounding_box;
    GetOcrLineWordCount get_line_word_count;
    GetOcrWord get_word;
    GetOcrWordConfidence get_word_confidence;
    // Optional: without them lines carry no character positions.
    GetOcrContent get_word_content;
    GetOcrBoundingBox get_word_bounding_box;
    Release release_result, release_init_options, release_pipeline, release_process_options;
};

std::optional<std::pair<QString, QString>> find_snipping_tool() {
    using FindPackagesByPackageFamilyFn = LONG(WINAPI *)(PCWSTR, UINT32, UINT32 *, PWSTR *, UINT32 *, WCHAR *, UINT32 *);
    using GetPackagePathByFullNameFn = LONG(WINAPI *)(PCWSTR, UINT32 *, PWSTR);
    auto *kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto find_packages = reinterpret_cast<FindPackagesByPackageFamilyFn>(GetProcAddress(kernel32, "FindPackagesByPackageFamily"));
    auto get_package_path = reinterpret_cast<GetPackagePathByFullNameFn>(GetProcAddress(kernel32, "GetPackagePathByFullName"));
    if (!find_packages || !get_package_path) return std::nullopt;
    const auto family = SNIPPING_TOOL_PACKAGE_FAMILY.toStdWString();
    UINT32 count = 0, buffer_len = 0;
    const LONG rc = find_packages(family.c_str(), PACKAGE_FILTER_HEAD_FLAG, &count, nullptr, &buffer_len, nullptr, nullptr);
    if (count == 0 || (rc != 0 && rc != ERROR_INSUFFICIENT_BUFFER_CODE)) return std::nullopt;
    std::vector<PWSTR> full_names(count);
    std::vector<WCHAR> buffer(buffer_len);
    if (find_packages(family.c_str(), PACKAGE_FILTER_HEAD_FLAG, &count, full_names.data(), &buffer_len, buffer.data(), nullptr) != 0) return std::nullopt;
    std::optional<std::tuple<QList<int>, QString, QString>> best;
    for (UINT32 index = 0; index < count; ++index) {
        UINT32 path_len = 0;
        get_package_path(full_names[index], &path_len, nullptr);
        std::vector<WCHAR> path(path_len);
        if (get_package_path(full_names[index], &path_len, path.data()) != 0) continue;
        const auto full_name = QString::fromWCharArray(full_names[index]);
        const auto source = QDir(QString::fromWCharArray(path.data())).filePath("SnippingTool");
        if (!has_required_files(source)) continue;
        auto candidate = std::make_tuple(package_version(full_name), full_name, source);
        if (!best || candidate > *best) best = candidate;
    }
    if (!best) return std::nullopt;
    return std::make_pair(std::get<1>(*best), std::get<2>(*best));
}

QString snipping_tool_cache_root() {
    // Copies of system files rather than user data. Tests share one cache outside their
    // sandboxes: loaded DLLs cannot be deleted with a sandbox at exit.
    if (!test_sandbox_dir().isEmpty()) return QDir(QDir::tempPath()).filePath("jietuba-oneocr-test-cache");
    return QDir(get_app_data_dir()).filePath("oneocr");
}

QString prepare_snipping_tool_files(const std::pair<QString, QString> &package) {
    const auto &[full_name, source] = package;
    const QDir root(snipping_tool_cache_root());
    const auto target = root.filePath(full_name);
    if (has_required_files(target)) return target;
    const auto staging = root.filePath(QString("%1.tmp%2").arg(full_name).arg(QCoreApplication::applicationPid()));
    QDir(staging).removeRecursively();
    if (!QDir().mkpath(staging)) throw std::runtime_error("cannot create OCR staging directory");
    bool ok = true;
    for (const auto &name : SNIPPING_TOOL_REQUIRED_FILES) ok = ok && QFile::copy(QDir(source).filePath(name), QDir(staging).filePath(name));
    if (ok) {
        QDir(target).removeRecursively();
        ok = QDir().rename(staging, target);
    }
    QDir(staging).removeRecursively();
    if (!ok) throw std::runtime_error("cannot copy the Snipping Tool OCR files");
    for (const auto &stale : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (stale != full_name) QDir(root.filePath(stale)).removeRecursively();
    return target;
}

SnippingToolOcr::SnippingToolOcr(const std::pair<QString, QString> &package) {
    const auto files_dir = QDir::toNativeSeparators(prepare_snipping_tool_files(package));
    const auto check = [](int64_t result, const char *message) {
        if (result != 0) throw std::runtime_error(QString("%1 (Code: %2)").arg(message).arg(result).toStdString());
    };
    // The engine finds its dependencies through the DLL directory, which is process-wide;
    // its dependencies are loaded once the engine exists, so the default order comes back.
    SetDllDirectoryW(files_dir.toStdWString().c_str());
    try {
        auto *module = LoadLibraryW(QDir(files_dir).filePath("oneocr.dll").toStdWString().c_str());
        if (!module) throw std::runtime_error("DLL initialization failed");
        dll_ = module;
        api_ = new Api;
        const auto bind = [module](const char *name) {
            auto *proc = GetProcAddress(module, name);
            if (!proc) throw std::runtime_error(std::string("DLL initialization failed: Missing DLL function: ") + name);
            return proc;
        };
        api_->create_init_options = reinterpret_cast<Api::CreateOcrInitOptions>(bind("CreateOcrInitOptions"));
        api_->set_use_model_delay_load = reinterpret_cast<Api::OcrInitOptionsSetUseModelDelayLoad>(bind("OcrInitOptionsSetUseModelDelayLoad"));
        api_->create_pipeline = reinterpret_cast<Api::CreateOcrPipeline>(bind("CreateOcrPipeline"));
        api_->create_process_options = reinterpret_cast<Api::CreateOcrProcessOptions>(bind("CreateOcrProcessOptions"));
        api_->set_max_line_count = reinterpret_cast<Api::OcrProcessOptionsSetMaxRecognitionLineCount>(bind("OcrProcessOptionsSetMaxRecognitionLineCount"));
        api_->run_pipeline = reinterpret_cast<Api::RunOcrPipeline>(bind("RunOcrPipeline"));
        api_->get_line_count = reinterpret_cast<Api::GetOcrLineCount>(bind("GetOcrLineCount"));
        api_->get_line = reinterpret_cast<Api::GetOcrLine>(bind("GetOcrLine"));
        api_->get_line_content = reinterpret_cast<Api::GetOcrContent>(bind("GetOcrLineContent"));
        api_->get_line_bounding_box = reinterpret_cast<Api::GetOcrBoundingBox>(bind("GetOcrLineBoundingBox"));
        api_->get_line_word_count = reinterpret_cast<Api::GetOcrLineWordCount>(bind("GetOcrLineWordCount"));
        api_->get_word = reinterpret_cast<Api::GetOcrWord>(bind("GetOcrWord"));
        api_->get_word_confidence = reinterpret_cast<Api::GetOcrWordConfidence>(bind("GetOcrWordConfidence"));
        api_->get_word_content = reinterpret_cast<Api::GetOcrContent>(GetProcAddress(module, "GetOcrWordContent"));
        api_->get_word_bounding_box = reinterpret_cast<Api::GetOcrBoundingBox>(GetProcAddress(module, "GetOcrWordBoundingBox"));
        api_->release_result = reinterpret_cast<Api::Release>(bind("ReleaseOcrResult"));
        api_->release_init_options = reinterpret_cast<Api::Release>(bind("ReleaseOcrInitOptions"));
        api_->release_pipeline = reinterpret_cast<Api::Release>(bind("ReleaseOcrPipeline"));
        api_->release_process_options = reinterpret_cast<Api::Release>(bind("ReleaseOcrProcessOptions"));
        check(api_->create_init_options(&init_options_), "Init options creation failed");
        check(api_->set_use_model_delay_load(init_options_, 0), "Model loading config failed");
        const auto model_path = QDir(files_dir).filePath("oneocr.onemodel").toUtf8();
        check(api_->create_pipeline(model_path.constData(), ONEOCR_MODEL_KEY, init_options_, &pipeline_), "Pipeline creation failed");
        check(api_->create_process_options(&process_options_), "Process options creation failed");
        check(api_->set_max_line_count(process_options_, 1000), "Line count config failed");
    } catch (...) {
        SetDllDirectoryW(nullptr);
        close();
        throw;
    }
    SetDllDirectoryW(nullptr);
}

SnippingToolOcr::~SnippingToolOcr() { close(); }

void SnippingToolOcr::close() {
    // The DLL stays loaded; only the engine's resources are released.
    if (api_) {
        if (process_options_) api_->release_process_options(process_options_);
        if (pipeline_) api_->release_pipeline(pipeline_);
        if (init_options_) api_->release_init_options(init_options_);
        delete api_;
        api_ = nullptr;
    }
    process_options_ = pipeline_ = init_options_ = 0;
}

QVector<OcrLine> SnippingToolOcr::recognize(const QImage &source) {
    if (!api_) throw std::runtime_error("engine closed");
    // ARGB32 is BGRA in memory on little-endian machines, the order the engine takes.
    auto image = source.format() == QImage::Format_ARGB32 ? source : source.convertToFormat(QImage::Format_ARGB32);
    if (image.width() < SNIPPING_TOOL_MIN_SIDE || image.height() < SNIPPING_TOOL_MIN_SIDE) {
        QImage padded(std::max(image.width(), SNIPPING_TOOL_MIN_SIDE), std::max(image.height(), SNIPPING_TOOL_MIN_SIDE), QImage::Format_ARGB32);
        padded.fill(image.pixelColor(0, 0));
        QPainter painter(&padded);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.drawImage(0, 0, image);
        painter.end();
        image = padded;
    }
    if (image.width() < 50 || image.height() < 50 || image.width() > 10000 || image.height() > 10000) throw std::runtime_error("Unsupported image size");
    // ARGB32 rows are already width * 4 bytes, the layout the engine takes, so the pixels go in place.
    const OcrImage image_struct{3, image.width(), image.height(), 0, static_cast<int64_t>(image.bytesPerLine()), image.constBits()};
    QVector<OcrLine> lines;
    int64_t result = 0;
    if (api_->run_pipeline(pipeline_, &image_struct, process_options_, &result) != 0) return lines;
    int64_t line_count = 0;
    if (api_->get_line_count(result, &line_count) == 0) {
        for (int64_t index = 0; index < line_count; ++index) {
            int64_t line = 0;
            if (api_->get_line(result, index, &line) != 0) continue;
            const char *content = nullptr;
            if (api_->get_line_content(line, &content) != 0 || !content) continue;
            const auto text = QString::fromUtf8(content);
            const OcrBoundingBox *box = nullptr;
            if (text.isEmpty() || api_->get_line_bounding_box(line, &box) != 0 || !box) continue;
            OcrLine out;
            out.text = text;
            out.box = corners(*box);
            // Average word confidence; 1.0 when no word reports one.
            double total = 0;
            int counted = 0;
            int64_t word_count = 0;
            // Positions need every word's text and box.
            QVector<OcrWord> words;
            bool words_complete = api_->get_word_content && api_->get_word_bounding_box;
            if (api_->get_line_word_count(line, &word_count) == 0) {
                for (int64_t w = 0; w < word_count; ++w) {
                    int64_t word = 0;
                    if (api_->get_word(line, w, &word) != 0) {
                        words_complete = false;
                        continue;
                    }
                    float confidence = 0;
                    if (api_->get_word_confidence(word, &confidence) == 0) {
                        total += confidence;
                        ++counted;
                    }
                    if (!words_complete) continue;
                    const char *word_text = nullptr;
                    const OcrBoundingBox *word_box = nullptr;
                    if (api_->get_word_content(word, &word_text) != 0 || !word_text || api_->get_word_bounding_box(word, &word_box) != 0 || !word_box)
                        words_complete = false;
                    else
                        words.append(OcrWord{QString::fromUtf8(word_text), corners(*word_box)});
                }
            }
            out.score = counted ? total / counted : 1.0;
            if (words_complete && !words.isEmpty()) {
                out.vertical = words_run_vertically(out.box, words);
                out.chars = spans_from_words(out.text, line_frame(out.box, out.vertical), words);
            }
            lines.append(out);
        }
    }
    api_->release_result(result);
    return lines;
}
}
