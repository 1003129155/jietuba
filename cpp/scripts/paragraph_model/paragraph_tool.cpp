// Data for training and checking the paragraph model (scripts/paragraph_model/pipeline.py), computed by the
// application's own code so the trees learn from exactly what the application measures:
//   ocr   --engine oneocr|ppocr [--ppocr-models DIR] --cases IN --out OUT   lines from the application's OCR engines
//   pairs --cases IN --out OUT                                              line pairs and their measurements
//   group --cases IN --out OUT                                              paragraphs from the compiled model
// A cases file is {"cases": [{"id", "width", "height", "image", "lines": [{"text", "quad": [[x, y] x4]}]}]}.
// Linked with the test sandbox, so settings and engine copies go to a temporary folder.
#include "ocr/ocr_manager.h"
#include "ocr/ppocr_models.h"
#include "translation/line_grouping.h"

#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPolygonF>
#include <QStringList>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

using namespace jietuba;

namespace {
QString option(const QStringList &args, const QString &name, const QString &fallback = {}) {
    const auto at = args.indexOf(name);
    return at >= 0 && at + 1 < args.size() ? args[at + 1] : fallback;
}

QJsonArray read_cases(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object().value("cases").toArray();
}

bool write_json(const QString &path, const QJsonObject &object) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    return true;
}

// The lines group_overlay_blocks hands to the grouping: trimmed text, the quad's bounding box, at
// least 2 px each way. source receives the case's line index of each.
QVector<GroupingLine> grouping_lines(const QJsonArray &json_lines, QVector<int> &source) {
    QVector<GroupingLine> lines;
    for (int index = 0; index < json_lines.size(); ++index) {
        const auto line = json_lines[index].toObject();
        const auto text = line.value("text").toString().trimmed();
        QPolygonF quad;
        for (const auto &point : line.value("quad").toArray()) quad << QPointF(point.toArray()[0].toDouble(), point.toArray()[1].toDouble());
        const auto rect = quad.boundingRect();
        if (text.isEmpty() || rect.width() < 2 || rect.height() < 2) continue;
        lines.append({text, rect});
        source.append(index);
    }
    return lines;
}

int run_ocr(const QStringList &args, const QJsonArray &cases, QJsonObject &out) {
    const auto engine = option(args, "--engine");
    if (engine == "ppocr") {
        const auto models = option(args, "--ppocr-models");
        if (models.isEmpty()) {
            std::fprintf(stderr, "ppocr needs --ppocr-models: a folder with the det/rec models and onnxruntime.dll\n");
            return 2;
        }
        ppocr_models::set_models_root_for_test(models);
    }
    const auto engine_id = engine == "ppocr" ? OCRManager::ENGINE_PP_RUST : OCRManager::ENGINE_ONEOCR;
    if (!set_ocr_engine(engine_id) || !initialize_ocr(engine_id)) {
        std::fprintf(stderr, "the %s engine is not available here: %s\n", qPrintable(engine), qPrintable(OCRManager::instance().get_last_error()));
        return 2;
    }
    std::mt19937 shuffle(static_cast<unsigned>(option(args, "--seed", "20261122").toUInt()));
    QJsonArray result;
    for (const auto &value : cases) {
        auto object = value.toObject();
        const QImage image(object.value("image").toString());
        if (image.isNull()) {
            std::fprintf(stderr, "cannot read %s\n", qPrintable(object.value("image").toString()));
            return 2;
        }
        const auto recognized = recognize_text(image);
        if (recognized.code != 100) {
            std::fprintf(stderr, "%s: recognition failed: %s\n", qPrintable(object.value("id").toString()), qPrintable(recognized.msg));
            return 2;
        }
        QJsonArray lines;
        for (const auto &line : recognized.data) {
            QJsonArray quad;
            for (const auto &point : line.box) quad.append(QJsonArray{point.x(), point.y()});
            QJsonObject entry{{"text", line.text}, {"quad", quad}};
            if (line.vertical) entry["vertical"] = true;
            lines.append(entry);
        }
        // The grouping must not depend on recognition order; shuffled, a test would notice if it did.
        std::vector<QJsonValue> order(lines.begin(), lines.end());
        std::shuffle(order.begin(), order.end(), shuffle);
        QJsonArray shuffled;
        for (const auto &line : order) shuffled.append(line);
        object["lines"] = shuffled;
        result.append(object);
    }
    out["cases"] = result;
    return 0;
}

void run_pairs(const QJsonArray &cases, QJsonObject &out) {
    QJsonArray names;
    for (const auto *name : PARAGRAPH_FEATURE_NAMES) names.append(QString::fromLatin1(name));
    QJsonObject results;
    for (const auto &value : cases) {
        const auto object = value.toObject();
        QVector<int> source;
        const auto lines = grouping_lines(object.value("lines").toArray(), source);
        QJsonArray pairs, features, used;
        for (const auto &pair : paragraph_line_pairs(lines)) {
            pairs.append(QJsonArray{source[pair.above], source[pair.below]});
            QJsonArray row;
            for (const float feature : pair.features) row.append(static_cast<double>(feature));
            features.append(row);
        }
        // What the grouping needs besides the scores: each line it saw, with its top and left edge (the order a
        // paragraph's lines are read in), whether it is a short label, whether it opens a list item, and for a
        // detached marker the line it belongs to (-1 otherwise).
        const auto owner = detached_list_markers(lines);
        for (int index = 0; index < lines.size(); ++index)
            used.append(QJsonArray{source[index], lines[index].rect.top(), lines[index].rect.left(), is_short_label(lines[index]),
                                   opens_list_item(lines[index].text) || owner.contains(index), owner[index] >= 0 ? source[owner[index]] : -1});
        results[object.value("id").toString()] = QJsonObject{{"pairs", pairs}, {"features", features}, {"lines", used}};
    }
    out["features"] = names;
    out["results"] = results;
}

void run_group(const QJsonArray &cases, QJsonObject &out) {
    QJsonObject results;
    for (const auto &value : cases) {
        const auto object = value.toObject();
        QVector<int> source;
        const auto json_lines = object.value("lines").toArray();
        const auto lines = grouping_lines(json_lines, source);
        auto groups = group_lines_into_paragraphs(lines);
        std::vector<double> seconds;
        for (int repeat = 0; repeat < 5; ++repeat) {
            const auto start = std::chrono::steady_clock::now();
            groups = group_lines_into_paragraphs(lines);
            seconds.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        }
        std::sort(seconds.begin(), seconds.end());
        QJsonArray paragraphs;
        std::vector<bool> placed(static_cast<size_t>(json_lines.size()), false);
        for (const auto &group : groups) {
            QJsonArray members;
            for (const int index : group) {
                members.append(source[index]);
                placed[static_cast<size_t>(source[index])] = true;
            }
            paragraphs.append(members);
        }
        for (int index = 0; index < json_lines.size(); ++index)
            if (!placed[static_cast<size_t>(index)]) paragraphs.append(QJsonArray{index});
        results[object.value("id").toString()] = QJsonObject{{"paragraphs", paragraphs}, {"seconds", seconds[seconds.size() / 2]}};
    }
    out["algo"] = "jietuba";
    out["results"] = results;
}
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    const auto args = app.arguments();
    const auto command = args.size() > 1 ? args[1] : QString();
    const auto cases = read_cases(option(args, "--cases"));
    const auto out_path = option(args, "--out");
    if (out_path.isEmpty() || cases.isEmpty() || (command != "ocr" && command != "pairs" && command != "group")) {
        std::fprintf(stderr, "usage: jietuba_paragraph_tool ocr|pairs|group --cases IN --out OUT [--engine oneocr|ppocr --ppocr-models DIR]\n");
        return 2;
    }
    QJsonObject out;
    if (command == "ocr") {
        const int code = run_ocr(args, cases, out);
        if (code) return code;
    } else if (command == "pairs") {
        run_pairs(cases, out);
    } else {
        run_group(cases, out);
    }
    return write_json(out_path, out) ? 0 : 1;
}
