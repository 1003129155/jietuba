#pragma once
#include <QImage>
#include <QPointF>
#include <QString>
#include <QVector>
#include <optional>
#include <utility>

namespace jietuba {
// The Windows Snipping Tool's own OCR engine (oneocr.dll). The DLL cannot be loaded
// from the Snipping Tool's install directory (not even as administrator), so it is
// copied out; copies are keyed by the package full name, so an updated Snipping Tool
// never mixes old and new files.
inline const QString SNIPPING_TOOL_PACKAGE_FAMILY = "Microsoft.ScreenSketch_8wekyb3d8bbwe";
inline const QStringList SNIPPING_TOOL_REQUIRED_FILES = {"oneocr.dll", "oneocr.onemodel", "onnxruntime.dll"};
// The engine rejects images with a side under 50 px; padded to 50 px they recognize fine.
constexpr int SNIPPING_TOOL_MIN_SIDE = 50;
// Past about this many pixels along its long side the engine shrinks the image and loses small text.
constexpr int SNIPPING_TOOL_MAX_SIDE = 2500;

// (package full name, SnippingTool directory); nullopt without the Snipping Tool or
// without OCR files in its version. Goes through the package manager API: plain users
// cannot list WindowsApps.
std::optional<std::pair<QString, QString>> find_snipping_tool();
QString snipping_tool_cache_root();
// Copies the OCR files into the app data directory and returns that directory. Copies
// to a staging directory first and renames it whole, so a failure never leaves a
// directory with missing files; other versions are removed when not in use.
QString prepare_snipping_tool_files(const std::pair<QString, QString> &package);

// Where one character sits along its line's reading direction, the line's length being 1.
struct OcrCharSpan {
    float start = 0, center = 0, end = 0;
};

// One recognized line: four corners from top-left clockwise, text, confidence. Reading runs
// from the top-right corner down when vertical, else from the top-left corner right. chars
// has one entry per character (code point) of text, or none when the engine gave no positions.
struct OcrLine {
    QVector<QPointF> box;
    QString text;
    double score = 0;
    bool vertical = false;
    QVector<OcrCharSpan> chars;
};

// Holds one oneocr engine. Not locked: callers ensure one thread at a time.
class SnippingToolOcr {
public:
    explicit SnippingToolOcr(const std::pair<QString, QString> &package);
    ~SnippingToolOcr();
    SnippingToolOcr(const SnippingToolOcr &) = delete;
    SnippingToolOcr &operator=(const SnippingToolOcr &) = delete;
    void close();
    // Throws std::runtime_error on engine errors.
    QVector<OcrLine> recognize(const QImage &image);
private:
    struct Api;
    void *dll_ = nullptr;
    Api *api_ = nullptr;
    long long init_options_ = 0, pipeline_ = 0, process_options_ = 0;
};
}
