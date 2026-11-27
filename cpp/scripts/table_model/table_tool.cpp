// Data for training and checking the table models (scripts/table_model/train.py), computed by the
// application's own code:
//   ocr   --engine oneocr|ppocr [--ppocr-models DIR] --tag TAG DIR...
//         recognizes every DIR/<name>.png that has a DIR/<name>.json answer, writes DIR/lines_TAG.jsonl
//   build --tag TAG --out OUT [--rules] [--rules-extent] [--given FILE] [--given-extent FILE]
//         [--given-caption FILE] [--pred PRED] DIR...
//         builds each table from DIR/lines_TAG.jsonl and writes OUT, one JSON object per image: every
//         visual row with its extent measurements, the rows just outside the table (title and note
//         candidates), the table's visual rows, each pair's measurements and decisions, the cells, and
//         whether a table is offered. --rules leaves every decision to the rules (and keeps no title or
//         note), --rules-extent only the extent; the --given files ({"dir/name": [1, 0, -1, ...]}) decide
//         pairs, extent rows or caption rows from outside; --pred also writes DIR/pred_PRED/<name>.json.
//   names [extent|caption]  the feature names of the row, extent or caption model
// Linked with the test sandbox, so settings and engine copies go to a temporary folder.
#include "ocr/ocr_manager.h"
#include "ocr/ppocr_models.h"
#include "text_recognition/table.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <cstdio>

using namespace jietuba;
using namespace jietuba::text_table;

namespace {
QString option(const QStringList &args, const QString &name, const QString &fallback = {}) {
    const auto at = args.indexOf(name);
    return at >= 0 && at + 1 < args.size() ? args[at + 1] : fallback;
}

// Positional arguments: everything that is neither an option nor an option's value.
QStringList folders(const QStringList &args, const QStringList &flags) {
    QStringList out;
    for (int i = 1; i < args.size(); ++i) {
        if (flags.contains(args[i])) continue;
        if (args[i].startsWith("--")) {
            ++i;
            continue;
        }
        out << args[i];
    }
    return out;
}

QJsonArray quad(double x0, double y0, double x1, double y1) { return {x0, y0, x1, y0, x1, y1, x0, y1}; }

std::array<double, 4> extent(const QJsonArray &b) {
    double x0 = 1e18, x1 = -1e18, y0 = 1e18, y1 = -1e18;
    for (int k = 0; k + 1 < b.size(); k += 2) {
        x0 = std::min(x0, b[k].toDouble());
        x1 = std::max(x1, b[k].toDouble());
        y0 = std::min(y0, b[k + 1].toDouble());
        y1 = std::max(y1, b[k + 1].toDouble());
    }
    return {x0, x1, y0, y1};
}

QStringList answered_images(const QDir &dir) {
    QStringList out;
    for (const auto &file : dir.entryList({"*.png"}, QDir::Files, QDir::Name))
        if (QFile::exists(dir.filePath(QFileInfo(file).completeBaseName() + ".json"))) out << file;
    return out;
}

int run_ocr(const QStringList &args) {
    const auto engine = option(args, "--engine", "oneocr");
    const auto tag = option(args, "--tag", engine);
    if (engine == "ppocr") ppocr_models::set_models_root_for_test(option(args, "--ppocr-models"));
    const auto engine_id = engine == "ppocr" ? OCRManager::ENGINE_PP_RUST : OCRManager::ENGINE_ONEOCR;
    if (!set_ocr_engine(engine_id) || !initialize_ocr(engine_id)) {
        std::fprintf(stderr, "the %s engine is not available here: %s\n", qPrintable(engine), qPrintable(OCRManager::instance().get_last_error()));
        return 2;
    }
    for (const auto &name : folders(args, {})) {
        const QDir dir(name);
        QFile out(dir.filePath("lines_" + tag + ".jsonl"));
        if (!out.open(QIODevice::WriteOnly)) return 2;
        int count = 0;
        for (const auto &file : answered_images(dir)) {
            const auto result = recognize_text(QImage(dir.filePath(file)));
            QJsonArray lines;
            for (const auto &l : table_lines(result.data)) {
                QJsonArray words;
                for (const auto &w : l.words) words.append(QJsonObject{{"text", w.text}, {"box", quad(w.x0, w.y0, w.x1, w.y1)}});
                lines.append(QJsonObject{{"text", l.text}, {"box", quad(l.x0, l.y0, l.x1, l.y1)}, {"words", words}});
            }
            out.write(QJsonDocument(QJsonObject{{"image", file}, {"lines", lines}}).toJson(QJsonDocument::Compact) + '\n');
            ++count;
        }
        std::printf("%s: %d images\n", qPrintable(name), count);
        std::fflush(stdout);
    }
    return 0;
}

int run_build(const QStringList &args) {
    const auto tag = option(args, "--tag", "oneocr");
    const auto pred = option(args, "--pred");
    QJsonObject given;
    if (const auto path = option(args, "--given"); !path.isEmpty()) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return 2;
        given = QJsonDocument::fromJson(f.readAll()).object();
    }
    QJsonObject given_extent, given_caption;
    for (auto [flag, target] : {std::pair<const char *, QJsonObject *>{"--given-extent", &given_extent}, {"--given-caption", &given_caption}})
        if (const auto path = option(args, flag); !path.isEmpty()) {
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly)) return 2;
            *target = QJsonDocument::fromJson(f.readAll()).object();
        }
    QFile out(option(args, "--out"));
    if (!out.open(QIODevice::WriteOnly)) {
        std::fprintf(stderr, "build needs --out\n");
        return 2;
    }
    const bool rules = args.contains("--rules"), rules_extent = rules || args.contains("--rules-extent");
    for (const auto &name : folders(args, {"--rules", "--rules-extent"})) {
        const QDir dir(name);
        QFile in(dir.filePath("lines_" + tag + ".jsonl"));
        if (!in.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "%s: no lines_%s.jsonl\n", qPrintable(name), qPrintable(tag));
            return 2;
        }
        if (!pred.isEmpty()) dir.mkpath("pred_" + pred);
        double total_ms = 0, max_ms = 0;
        int count = 0;
        while (!in.atEnd()) {
            const auto rec = QJsonDocument::fromJson(in.readLine()).object();
            const QString file = rec["image"].toString();
            const QString base = QFileInfo(file).completeBaseName();
            QVector<Line> lines;
            for (const auto &lv : rec["lines"].toArray()) {
                const auto lo = lv.toObject();
                const auto le = extent(lo["box"].toArray());
                Line line{lo["text"].toString(), le[0], le[1], le[2], le[3], {}};
                for (const auto &wv : lo["words"].toArray()) {
                    const auto wo = wv.toObject();
                    const auto we = extent(wo["box"].toArray());
                    line.words.append({wo["text"].toString(), we[0], we[1], we[2], we[3]});
                }
                lines.append(line);
            }
            RowTrace trace;
            RowControl control;
            control.use_model = !rules;
            control.use_extent_model = !rules_extent;
            control.use_caption_model = !rules;
            control.trace = &trace;
            const QString key = QFileInfo(name).fileName() + "/" + base;
            for (const auto &v : given.value(key).toArray()) control.given.append(v.toInt(-1));
            for (const auto &v : given_extent.value(key).toArray()) control.given_extent.append(v.toInt(-1));
            for (const auto &v : given_caption.value(key).toArray()) control.given_caption.append(v.toInt(-1));
            const QImage image(dir.filePath(file));
            QElapsedTimer timer;
            timer.start();
            const auto table = build_table(image, lines, control);
            const double ms = timer.nsecsElapsed() / 1e6;
            total_ms += ms;
            max_ms = std::max(max_ms, ms);
            ++count;
            const bool offered = is_table(table);
            QJsonArray all_rows, extent_rows, captions, rows, pairs, cells;
            for (const auto &c : trace.captions) {
                QJsonArray f;
                for (float v : c.features) f.append(static_cast<double>(v));
                captions.append(QJsonObject{{"row", c.row}, {"above", c.above}, {"f", f}, {"kept", c.kept}});
            }
            for (int r = 0; r < trace.all_rows.size(); ++r) {
                QJsonArray segs;
                for (int s = 0; s < trace.all_rows[r].size(); ++s) {
                    const auto &b = trace.all_rows[r][s];
                    segs.append(QJsonArray{b.left(), b.top(), b.right(), b.bottom(), trace.all_texts[r][s]});
                }
                all_rows.append(segs);
            }
            for (const auto &e : trace.extent) {
                QJsonArray f;
                for (float v : e.features) f.append(static_cast<double>(v));
                extent_rows.append(QJsonObject{{"f", f}, {"rules", e.rules_in}, {"in", e.in}});
            }
            for (int r = 0; r < trace.rows.size(); ++r) {
                QJsonArray segs;
                for (int s = 0; s < trace.rows[r].size(); ++s) {
                    const auto &b = trace.rows[r][s];
                    segs.append(QJsonArray{b.left(), b.top(), b.right(), b.bottom(), trace.texts[r][s]});
                }
                rows.append(segs);
            }
            for (const auto &p : trace.pairs) {
                QJsonArray f;
                for (float v : p.features) f.append(static_cast<double>(v));
                pairs.append(QJsonObject{{"f", f}, {"rules", p.rules_join}, {"fixed", p.fixed}, {"joined", p.joined}});
            }
            for (const auto &c : table.cells)
                cells.append(QJsonObject{{"r", c.row}, {"c", c.column}, {"rs", c.row_span}, {"cs", c.column_span}, {"text", c.text}});
            out.write(QJsonDocument(QJsonObject{{"key", key}, {"all_rows", all_rows}, {"extent", extent_rows}, {"captions", captions}, {"rows", rows}, {"pairs", pairs}, {"offered", offered}, {"cells", cells}, {"ms", ms}})
                          .toJson(QJsonDocument::Compact) +
                      '\n');
            if (!pred.isEmpty()) {
                QFile p(dir.filePath("pred_" + pred + "/" + base + ".json"));
                if (p.open(QIODevice::WriteOnly)) p.write(QJsonDocument(QJsonObject{{"offered", offered}, {"cells", cells}}).toJson(QJsonDocument::Compact));
            }
        }
        std::printf("%s: %d tables, %.1f ms average, %.1f ms longest\n", qPrintable(name), count, total_ms / std::max(1, count), max_ms);
        std::fflush(stdout);
    }
    return 0;
}
}

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    const auto args = app.arguments().mid(1);
    if (args.isEmpty()) {
        std::fprintf(stderr, "usage: jietuba_table_tool ocr|build ...\n");
        return 2;
    }
    if (args[0] == "names") {
        if (args.size() > 1 && args[1] == "extent")
            for (const auto *name : EXTENT_FEATURE_NAMES) std::printf("%s\n", name);
        else if (args.size() > 1 && args[1] == "caption")
            for (const auto *name : CAPTION_FEATURE_NAMES) std::printf("%s\n", name);
        else
            for (const auto *name : ROW_FEATURE_NAMES) std::printf("%s\n", name);
        return 0;
    }
    if (args[0] == "ocr") return run_ocr(args);
    if (args[0] == "build") return run_build(args);
    std::fprintf(stderr, "unknown command %s\n", qPrintable(args[0]));
    return 2;
}
