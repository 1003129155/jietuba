#include "canvas/items/drawing_items.h"
#include "canvas/items/stroke_item.h"
#include "canvas/undo.h"
#include "canvas/items/arrow_item.h"
#include <QTest>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QPainter>
#include <QStyleOptionGraphicsItem>
#include <QGraphicsSceneHoverEvent>
#include <QFontInfo>

using namespace jietuba;
namespace {
QJsonArray rect(const QRectF &r) { return {r.x(), r.y(), r.width(), r.height()}; }
class ContextView : public QGraphicsView, public DrawingEditContext, public NumberRefreshContext {
public:
    using QGraphicsView::QGraphicsView;
    QGraphicsItem *selected = nullptr;
    bool allow = false;
    std::function<void(int)> on_toolbar;
    QGraphicsItem *selected_drawing_item() const override { return selected; }
    bool can_show_hover_cursor(QGraphicsItem *) const override { return allow; }
    void refresh_number_toolbar(int value) override { if (on_toolbar) on_toolbar(value); }
    void refresh_number_cursor_if_active(bool) override {}
};
class ContextScene : public QGraphicsScene, public NumberRefreshContext {
public:
    QString tool = "number";
    std::function<void(bool)> on_cursor;
    void refresh_number_toolbar(int) override {}
    void refresh_number_cursor_if_active(bool force) override { if (tool == "number" && on_cursor) on_cursor(force); }
};
ItemReference create(const QJsonObject &c) {
    QPen pen(QColor(c.value("color").toString("#d02060")), c.value("width").toDouble(4));
    pen.setStyle(static_cast<Qt::PenStyle>(c.value("pen_style").toInt(1)));
    const auto kind = c["kind"].toString();
    if (kind == "stroke") {
        QPainterPath path(QPointF(20, 30)); if (!c["empty"].toBool()) path.cubicTo(40, 80, 90, 0, 120, 70);
        return ItemReference(new StrokeItem(path, pen, c["highlighter"].toBool()));
    }
    if (kind == "rect") {
        auto *item = new RectItem(QRectF(20, 20, 100, 60), pen, c["radius"].toDouble());
        item->is_highlighter = c["highlighter"].toBool(); item->is_highlighter_rect = c["filled"].toBool();
        if (item->is_highlighter_rect) item->setBrush(QColor("#40a060"));
        return ItemReference(item);
    }
    if (kind == "ellipse") return ItemReference(new EllipseItem(QRectF(20, 20, 100, 60), pen));
    if (kind == "arrow") {
        const auto start = c.value("start").toArray({20, 40}), end = c.value("end").toArray({130, 70});
        return ItemReference(new ArrowItem(QPointF(start[0].toDouble(), start[1].toDouble()), QPointF(end[0].toDouble(), end[1].toDouble()), pen, c.value("style").toString("single")));
    }
    return ItemReference(new NumberItem(c.value("number").toInt(12), QPointF(75, 55), c.value("radius").toDouble(20), pen.color(), c["style"].toString()));
}
QJsonObject snapshot(const ItemReference &reference) {
    auto *item = reference.get(); auto *drawing = reference.as<DrawingItemMixin>();
    const auto shape = item->shape(); QJsonArray elements;
    for (int i = 0; i < shape.elementCount(); ++i) { auto e = shape.elementAt(i); elements.append(QJsonArray{e.x, e.y, static_cast<int>(e.type)}); }
    const auto style = drawing->selection_frame_style();
    QImage image(160, 120, QImage::Format_ARGB32_Premultiplied); image.fill(QColor("#bfd2e8"));
    QPainter painter(&image); painter.translate(item->pos()); painter.setOpacity(item->opacity());
    QStyleOptionGraphicsItem option; item->paint(&painter, &option, nullptr); painter.end();
    QJsonObject out{{"bounds", rect(item->boundingRect())}, {"shape", elements},
        {"width", *drawing->get_stroke_width()}, {"opacity", *drawing->get_visual_opacity()},
        {"item_opacity", item->opacity()}, {"z", item->zValue()},
        {"movable", item->flags().testFlag(QGraphicsItem::ItemIsMovable)}, {"selectable", item->flags().testFlag(QGraphicsItem::ItemIsSelectable)},
        {"frame", style ? QJsonValue(static_cast<int>(*style)) : QJsonValue(QJsonValue::Null)}, {"cursor", static_cast<int>(item->cursor().shape())},
        {"sha256", QString::fromLatin1(QCryptographicHash::hash(QByteArray::fromRawData(reinterpret_cast<const char *>(image.constBits()), static_cast<qsizetype>(image.sizeInBytes())), QCryptographicHash::Sha256).toHex())}};
    if (auto *number = reference.as<NumberItem>()) {
        out.insert("style", number->style); out.insert("hollow", number->is_hollow()); out.insert("ring", number->has_ring());
        out.insert("visual", rect(number->visualRect())); out.insert("scene_visual", rect(number->sceneVisualRect()));
        out.insert("radius", number->radius); out.insert("text_color", number->solid_text_color().name());
        out.insert("font_size", number->number_font().pointSize()); out.insert("ring_width", number->ring_width());
        QPainterPath glyphs; glyphs.addText(QPointF(0, 0), number->number_font(), QString::number(number->number));
        out.insert("font_family", QFontInfo(number->number_font()).family()); out.insert("glyph_elements", glyphs.elementCount());
    }
    if (auto *arrow = reference.as<ArrowItem>()) {
        const auto path = arrow->path(); QJsonArray path_elements;
        for (int i = 0; i < path.elementCount(); ++i) { const auto e = path.elementAt(i); path_elements.append(QJsonArray{e.x, e.y, static_cast<int>(e.type)}); }
        auto point = [](const QPointF &p) { return QJsonArray{p.x(), p.y()}; };
        out.insert("arrow_style", arrow->arrow_style()); out.insert("start", point(arrow->start_pos)); out.insert("end", point(arrow->end_pos));
        out.insert("control", point(arrow->control_pos())); out.insert("stored_control", point(arrow->_control_pos)); out.insert("curved", arrow->is_curved());
        out.insert("color", arrow->color.name(QColor::HexArgb)); out.insert("path", path_elements); out.insert("path_bounds", rect(path.boundingRect()));
    }
    return out;
}
bool equivalent(const QJsonValue &a, const QJsonValue &b) {
    if (a.type() != b.type()) return false;
    if (a.isDouble()) return qAbs(a.toDouble() - b.toDouble()) <= 1e-9;
    if (a.isArray()) { const auto aa = a.toArray(), bb = b.toArray(); if (aa.size() != bb.size()) return false; for (qsizetype i = 0; i < aa.size(); ++i) if (!equivalent(aa[i], bb[i])) return false; return true; }
    if (a.isObject()) { const auto aa = a.toObject(), bb = b.toObject(); if (aa.keys() != bb.keys()) return false; for (auto it = aa.begin(); it != aa.end(); ++it) if (!equivalent(it.value(), bb[it.key()])) return false; return true; }
    return a == b;
}
std::optional<int> optional_int(const QJsonObject &o, const QString &key) { return o.contains(key) && !o[key].isNull() ? std::optional<int>(o[key].toInt()) : std::nullopt; }
QVariantMap decode_state(const QJsonObject &data) {
    auto out = data.toVariantMap();
    for (const auto &key : {"pos", "transformOriginPoint", "start", "end", "control"}) if (data[key].isArray()) { const auto a = data[key].toArray(); out[key] = QPointF(a[0].toDouble(), a[1].toDouble()); }
    if (data["rect"].isArray()) { const auto a = data["rect"].toArray(); out["rect"] = QRectF(a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble()); }
    if (data["transform"].isArray()) { const auto a = data["transform"].toArray(); out["transform"] = QTransform(a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble(), a[4].toDouble(), a[5].toDouble(), a[6].toDouble(), a[7].toDouble(), a[8].toDouble()); }
    // JSON uses one numeric type; recover the Python int/float distinction for
    // isinstance(number,int), without converting deliberate float 2.5.
    for (const auto &key : {"number", "rotation", "opacity"}) if (data[key].isDouble() && data[key].toDouble() == data[key].toInt()) out[key] = data[key].toInt();
    if (data["pen_state"].isObject()) { auto pen = data["pen_state"].toObject().toVariantMap(); if (pen.contains("pen_color")) pen["pen_color"] = QColor(pen["pen_color"].toString()); out["pen_state"] = pen; }
    return out;
}
QJsonObject edit_snapshot(const ItemReference &reference) {
    auto out = snapshot(reference); auto *item = reference.get(); const auto t = item->transform();
    out.insert("pos", QJsonArray{item->pos().x(), item->pos().y()});
    out.insert("transform", QJsonArray{t.m11(), t.m12(), t.m13(), t.m21(), t.m22(), t.m23(), t.m31(), t.m32(), t.m33()});
    out.insert("rotation", item->rotation()); out.insert("origin", QJsonArray{item->transformOriginPoint().x(), item->transformOriginPoint().y()});
    if (auto *shape = dynamic_cast<QAbstractGraphicsShapeItem *>(item)) {
        const auto pen = shape->pen(); QJsonArray dash; for (auto d : pen.dashPattern()) dash.append(d);
        out.insert("pen", QJsonObject{{"color", pen.color().name(QColor::HexArgb)}, {"width", pen.widthF()}, {"style", static_cast<int>(pen.style())}, {"cap", static_cast<int>(pen.capStyle())}, {"join", static_cast<int>(pen.joinStyle())}, {"dash", dash}});
    }
    if (auto *r = reference.as<RectItem>()) { out.insert("rect", rect(r->rect())); out.insert("corner", r->get_corner_radius()); }
    if (auto *e = reference.as<EllipseItem>()) out.insert("rect", rect(e->rect()));
    if (auto *n = reference.as<NumberItem>()) out.insert("number", n->number);
    return out;
}
}
class CanvasParityTests : public QObject {
    Q_OBJECT
    QJsonObject fixtures_;
    QJsonObject expected_snapshot(const QJsonValue &value) const {
        auto out = value.toObject();
        for (const auto &key : {"shape", "path"}) {
            const auto ref_key = QString(key) + "_ref";
            if (out.contains(ref_key)) out.insert(key, fixtures_["geometry"].toArray()[out.take(ref_key).toInt()]);
        }
        return out;
    }
private slots:
    void initTestCase() {
        const auto override_path = qEnvironmentVariable("JIETUBA_CANVAS_FIXTURE");
        QFile file(override_path.isEmpty() ? QString(JIETUBA_CANVAS_FIXTURE_PATH) : override_path); QVERIFY(file.open(QIODevice::ReadOnly));
        fixtures_ = QJsonDocument::fromJson(file.readAll()).object(); QVERIFY(!fixtures_.isEmpty());
        QCOMPARE(QGuiApplication::platformName(), fixtures_["platform"].toString());
    }
    void drawingGeometryPixelsAndHover() {
        int case_index = 0;
        for (const auto &value : fixtures_["drawing"].toArray()) {
            const auto sequence = value.toObject(); QGraphicsScene scene; ContextView view(&scene);
            auto item = create(sequence["config"].toObject()); scene.addItem(item.get()); auto *drawing = item.as<DrawingItemMixin>();
            for (const auto &step_value : sequence["steps"].toArray()) {
                const auto step = step_value.toObject(), action = step["action"].toObject(); const auto op = action["op"].toString();
                if (op == "width") drawing->set_stroke_width(action["value"].toDouble());
                else if (op == "scale") drawing->scale_stroke_width(action["value"].toDouble());
                else if (op == "opacity") drawing->set_visual_opacity(action["value"].toDouble());
                else if (op == "direct_opacity") item.get()->setOpacity(action["value"].toDouble());
                else if (op == "hover") { view.allow = action["allow"].toBool(); drawing->hover_enter(nullptr); }
                else if (op == "move_hover") drawing->hover_move(nullptr);
                else if (op == "select") view.selected = item.get();
                else if (op == "deselect") view.selected = nullptr;
                else if (op == "allow") view.allow = action["value"].toBool();
                else if (op == "leave") drawing->hover_leave(nullptr);
                else if (op == "rect") { if (auto *r = item.as<RectItem>()) r->setRect({25, 15, 90, 70}); else item.as<EllipseItem>()->setRect({25, 15, 90, 70}); }
                else if (op == "corner") item.as<RectItem>()->set_corner_radius(action["value"].toDouble());
                else if (op == "path") { QPainterPath path(QPointF(30, 40)); path.lineTo(100, 90); item.as<StrokeItem>()->setPath(path); }
                else if (op == "control_value") { const auto a = action["value"].toArray(); item.as<ArrowItem>()->set_control_pos(action["value"].isNull() ? std::nullopt : std::optional<QPointF>(QPointF(a[0].toDouble(), a[1].toDouble()))); }
                else if (op == "geometry") item.as<ArrowItem>()->update_geometry();
                else if (op == "control") { const auto a = action["value"].toArray(); item.as<ArrowItem>()->set_control_point({a[0].toDouble(), a[1].toDouble()}); }
                else if (op == "positions") { const auto a = action["start"].toArray(), b = action["end"].toArray(); item.as<ArrowItem>()->set_positions({a[0].toDouble(), a[1].toDouble()}, {b[0].toDouble(), b[1].toDouble()}); }
                else if (op == "reset") item.as<ArrowItem>()->reset_control_point();
                else if (op == "arrow_style") item.as<ArrowItem>()->set_arrow_style(action["value"].toString());
                const auto actual = snapshot(item), expected = expected_snapshot(step["expected"]);
                for (auto it = expected.begin(); it != expected.end(); ++it) {
                    const auto message = QString("case %1 %2 %3: actual=%4 expected=%5").arg(case_index).arg(op, it.key(), QString::fromUtf8(QJsonDocument(QJsonArray{actual[it.key()]}).toJson(QJsonDocument::Compact)), QString::fromUtf8(QJsonDocument(QJsonArray{it.value()}).toJson(QJsonDocument::Compact))).toUtf8();
                    QVERIFY2(equivalent(actual[it.key()], it.value()), message.constData());
                }
            }
            ++case_index;
        }
    }
    void numberUndoStateAndMergeTrace() {
        int sequence_index = 0;
        for (const auto &sequence : fixtures_["undo"].toArray()) {
            ContextScene scene; ContextView view(&scene); std::vector<ItemReference> items; CommandUndoStack stack; double now = 100.; QJsonArray events;
            auto record = [&](const QString &kind, QJsonValue value) {
                QJsonArray numbers; for (const auto &item : items) if (item.get()->scene() == &scene) numbers.append(item.as<NumberItem>()->number);
                events.append(QJsonObject{{"kind", kind}, {"value", value}, {"index", stack.index()}, {"next", NumberSequence::get_next_number(&scene)}, {"numbers", numbers}});
            };
            view.on_toolbar = [&](int value) { record("toolbar", value); }; scene.on_cursor = [&](bool force) { record("cursor", QJsonArray{"number", force}); };
            int step_index = 0;
            for (const auto &step_value : sequence.toArray()) {
                const auto step = step_value.toObject(), action = step["action"].toObject(); const auto op = action["op"].toString();
                events = {}; now += action.value("dt").toDouble(.1);
                const auto index = action["item"].toInt();
                if (op == "add") {
                    auto *number = new NumberItem(action["number"].toInt(), QPointF(items.size() * 10., 0), 20, QColor("#d02060"));
                    if (action.contains("order")) number->number_order = action["order"].toInt();
                    items.emplace_back(number); stack.push_command(new AddNumberCommand(&scene, items.back(), optional_int(action, "before"), optional_int(action, "after")));
                } else if (op == "remove") stack.push_command(new RemoveNumberCommand(&scene, items[index]));
                else if (op == "renumber") stack.push_command(new RemoveNumberAndRenumberCommand(&scene, items[index]));
                else if (op == "batch") { std::vector<ItemReference> batch; for (const auto &i : action["items"].toArray()) batch.push_back(items[i.toInt()]); stack.push_command(new BatchRemoveCommand(&scene, batch, "Remove Items", optional_int(action, "before"))); }
                else if (op == "style") stack.push_command(new NumberStyleCommand(items[index], items[index].as<NumberItem>()->style, action["value"].toString(), "Style"));
                else if (op == "edit") {
                    auto *number = items[index].as<NumberItem>(); const auto before = NumberSequence::get_next_number(&scene);
                    const auto after = NumberSequence::get_next_after_number_edit(&scene, number, number->number, action["value"].toInt());
                    stack.push_command(new NumberEditCommand(items[index], {{"number", number->number}}, {{"number", action["value"].toInt()}}, before, after, "Edit Number", [&now] { return now; }));
                } else if (op == "next") NumberSequence::set_next_number(&scene, action["value"].toInt());
                else if (op == "adjust") NumberSequence::adjust_next_number(&scene, action["value"].toInt());
                else if (op == "tool") scene.tool = action["value"].toString();
                else if (op == "order") NumberSequence::assign_number_order(&scene, items[index].as<NumberItem>());
                else if (op == "undo") stack.undo(); else if (op == "redo") stack.redo(); else if (op == "clear") stack.clear(); else QFAIL("Unknown action");
                QJsonArray values, draw_order;
                for (const auto &item : items) { auto *number = item.as<NumberItem>(); values.append(QJsonObject{{"attached", number->scene() == &scene}, {"number", number->number}, {"style", number->style}, {"order", number->number_order ? QJsonValue(*number->number_order) : QJsonValue(QJsonValue::Null)}}); }
                auto scene_items = scene.items(); std::reverse(scene_items.begin(), scene_items.end());
                for (auto *item : scene_items) { for (qsizetype i = 0; i < static_cast<qsizetype>(items.size()); ++i) if (items[i].get() == item) { draw_order.append(i); break; } }
                const QJsonObject actual{{"count", stack.count()}, {"index", stack.index()}, {"can_undo", stack.canUndo()}, {"can_redo", stack.canRedo()},
                    {"undo_text", stack.undoText()}, {"redo_text", stack.redoText()}, {"next", NumberSequence::get_next_number(&scene)},
                    {"offset", scene.property("_number_tool_offset").toInt()}, {"items", values}, {"draw_order", draw_order}, {"events", events}};
                const auto message = QString("sequence %1 step %2 %3\nactual=%4\nexpected=%5").arg(sequence_index).arg(step_index).arg(op, QString::fromUtf8(QJsonDocument(actual).toJson(QJsonDocument::Compact)), QString::fromUtf8(QJsonDocument(step["expected"].toObject()).toJson(QJsonDocument::Compact))).toUtf8();
                QVERIFY2(equivalent(actual, step["expected"]), message.constData()); ++step_index;
            }
            ++sequence_index;
        }
    }
    void editGeometryAndValueCopies() {
        int case_index = 0;
        for (const auto &v : fixtures_["edit"].toArray()) {
            const auto sequence = v.toObject(); QGraphicsScene scene; auto item = create(sequence["config"].toObject()); scene.addItem(item.get());
            auto old = decode_state(sequence["old"].toObject()), next = decode_state(sequence["new"].toObject());
            auto *command = new EditItemCommand(item, old, next);
            // Mutating caller-owned values/maps must not alter captured Qt values.
            for (auto *state : {&old, &next}) {
                if (state->value("pos").metaType().id() == QMetaType::QPointF) { auto p = state->value("pos").toPointF(); p.setX(999); (*state)["pos"] = p; }
                if (state->value("rect").metaType().id() == QMetaType::QRectF) { auto r = state->value("rect").toRectF(); r.setWidth(999); (*state)["rect"] = r; }
                if (state->value("transform").metaType().id() == QMetaType::QTransform) { auto t = state->value("transform").value<QTransform>(); t.translate(999, 999); (*state)["transform"] = t; }
            }
            CommandUndoStack stack;
            for (const auto &s : sequence["steps"].toArray()) {
                const auto step = s.toObject(); const auto op = step["op"].toString();
                if (op == "push") stack.push_command(command); else if (op == "undo") stack.undo(); else stack.redo();
                const auto actual = edit_snapshot(item), expected = expected_snapshot(step["expected"]);
                for (auto it = expected.begin(); it != expected.end(); ++it) {
                    const auto message = QString("edit case %1 %2 %3 actual=%4 expected=%5").arg(case_index).arg(op, it.key(), QString::fromUtf8(QJsonDocument(QJsonArray{actual[it.key()]}).toJson(QJsonDocument::Compact)), QString::fromUtf8(QJsonDocument(QJsonArray{it.value()}).toJson(QJsonDocument::Compact))).toUtf8();
                    QVERIFY2(equivalent(actual[it.key()], it.value()), message.constData());
                }
            }
            ++case_index;
        }
    }
    void commandAndSceneOwnership() {
        // Stack truncation destroys detached items when no external lease exists.
        CommandUndoStack stack; QGraphicsScene scene;
        auto *item = new RectItem({0, 0, 10, 10}, QPen(Qt::red)); auto lifetime = item->lifetime();
        stack.push_command(new AddItemCommand(&scene, ItemReference(item))); stack.undo(); stack.clear(); QVERIFY(!lifetime->item);
        // Scene clear destroys attached items while commands stay usable.
        item = new RectItem({0, 0, 10, 10}, QPen(Qt::red)); lifetime = item->lifetime();
        stack.push_command(new AddItemCommand(&scene, ItemReference(item))); scene.clear(); QVERIFY(!lifetime->item); stack.undo(); stack.redo(); stack.clear();
        // Scene destruction before command destruction is safe in both states.
        auto *temporary = new QGraphicsScene;
        auto attached = ItemReference(new NumberItem(1, {}, 20, Qt::red)); auto detached = ItemReference(new NumberItem(2, {}, 20, Qt::red));
        stack.push_command(new AddNumberCommand(temporary, attached)); stack.push_command(new AddNumberCommand(temporary, detached)); stack.undo();
        delete temporary; QVERIFY(!attached.get()); QVERIFY(detached.get()); stack.redo(); stack.undo(); stack.clear();
        // A caller lease retains a removed item after the stack releases it.
        QVERIFY(detached.get());
    }
};
QTEST_MAIN(CanvasParityTests)
#include "test_canvas_parity.moc"
