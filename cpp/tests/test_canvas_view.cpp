#include "canvas/handle_editor.h"
#include "canvas/items/arrow_item.h"
#include "canvas/items/drawing_items.h"
#include <QInputMethodEvent>
#include <QTextCursor>
#include "canvas/items/watermark_item.h"
#include "canvas/items/mosaic_item.h"
#include "canvas/items/spotlight_item.h"
#include "canvas/items/stroke_item.h"
#include "canvas/items/text_item.h"
#include "canvas/scene.h"
#include "canvas/selection_model.h"
#include "canvas/smart_edit_controller.h"
#include "canvas/undo.h"
#include "canvas/view.h"
#include "settings/tool_settings.h"
#include "tools/controller.h"
#include <QApplication>
#include <QDir>
#include <QFontDatabase>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScopeGuard>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <cmath>

using namespace jietuba;

namespace {
constexpr int W = 500;
constexpr int H = 380;
const QColor RED("#FF3B30");

QString font_family;

QFont text_font(int point_size = 16) { return QFont(font_family, point_size); }

TextItem *new_text(const QString &text = "note", const QPointF &pos = QPointF(0, 0), int point_size = 16) {
    return new TextItem(text, pos, text_font(point_size), QColor("black"));
}

StrokeItem *new_stroke(double width = 4.0, bool highlighter = false) {
    QPainterPath path(QPointF(0, 0));
    path.lineTo(QPointF(100, 0));
    return new StrokeItem(path, QPen(QColor("#FF0000"), width), highlighter);
}

RectItem *new_rect(const QRectF &rect = QRectF(0, 0, 100, 60), double width = 4.0) { return new RectItem(rect, QPen(QColor("#FF0000"), width)); }

bool is_dashed(const QPen &pen) { return pen.style() == Qt::DashLine || pen.style() == Qt::CustomDashLine; }

std::optional<EditHandle> handle_of(LayerEditor &editor, HandleType type) {
    for (const auto &handle : editor.handles)
        if (handle.handle_type == type) return handle;
    return std::nullopt;
}

std::vector<EditHandle> handles_of(LayerEditor &editor, HandleType type) {
    std::vector<EditHandle> found;
    for (const auto &handle : editor.handles)
        if (handle.handle_type == type) found.push_back(handle);
    return found;
}

// Every point inside one outline and outside the other lies within tolerance of the first one's edge.
bool same_outline(const QPainterPath &a, const QPainterPath &b, qreal tolerance) {
    const auto bounds = a.boundingRect().united(b.boundingRect()).adjusted(-2, -2, 2, 2);
    for (qreal y = bounds.top(); y <= bounds.bottom(); y += 1)
        for (qreal x = bounds.left(); x <= bounds.right(); x += 1) {
            const QPointF p(x, y);
            const bool in_a = a.contains(p);
            if (in_a == b.contains(p)) continue;
            // Rings from half a pixel out, so a sliver thinner than the tolerance is not stepped over.
            bool near_edge = false;
            for (qreal radius = .5; radius <= tolerance && !near_edge; radius += .5)
                for (int k = 0; k < 16 && !near_edge; ++k) {
                    const auto angle = k * 3.14159265358979323846 / 8;
                    near_edge = a.contains(p + radius * QPointF(std::cos(angle), std::sin(angle))) != in_a;
                }
            if (!near_edge) return false;
        }
    return true;
}

// Where a handle is drawn (its anchor plus a screen offset), in scene coordinates.
QPointF handle_point(CanvasView *view, const EditHandle &handle) {
    return view->mapToScene(view->smart_edit_controller->layer_editor->screen_center(handle).toPoint());
}

// A full press, drag and release in scene coordinates; the step count must not matter.
void send(CanvasView *view, QEvent::Type type, const QPointF &scene_pos, Qt::MouseButtons buttons = Qt::LeftButton,
          Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    const QPointF view_pos = view->mapFromScene(scene_pos);
    const auto button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type, view_pos, QPointF(view->viewport()->mapToGlobal(view_pos.toPoint())), button, buttons, modifiers);
    QApplication::sendEvent(view->viewport(), &event);
    QApplication::processEvents();
}

void gesture(CanvasView *view, const QPointF &start, const QPointF &end, int steps = 5, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    send(view, QEvent::MouseButtonPress, start, Qt::LeftButton, modifiers);
    for (int i = 1; i <= steps; ++i) send(view, QEvent::MouseMove, start + (end - start) * (double(i) / steps), Qt::LeftButton, modifiers);
    send(view, QEvent::MouseButtonRelease, end, Qt::NoButton, modifiers);
}

// The bottom edge's middle: grabs text being edited without placing the caret or hitting a corner handle.
QPointF text_drag_point(TextItem *item) {
    const auto rect = item->mapToScene(item->boundingRect()).boundingRect();
    return QPointF(rect.center().x(), rect.bottom() - 2);
}

template <typename T>
QList<T *> items_of(QGraphicsScene *scene) {
    QList<T *> found;
    for (auto *item : scene->items())
        if (auto *typed = dynamic_cast<T *>(item)) found.append(typed);
    return found;
}

void wheel(CanvasView *view, int delta, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QTest::wheelEvent(view->windowHandle(), QPointF(view->viewport()->rect().center()), QPoint(0, delta), QPoint(), modifiers);
}
}

class CanvasViewTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir dir_;
    QSettings *settings_ = nullptr;
    ToolSettingsManager *config_ = nullptr;
    // Views first: they hold the scenes.
    std::vector<std::unique_ptr<CanvasView>> views_;
    std::vector<std::unique_ptr<CanvasScene>> scenes_;

    std::pair<CanvasScene *, CanvasView *> canvas(const QString &tool = "cursor", bool cross_tool_select = false, const QSize &size = QSize(W, H)) {
        QImage image(size, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        auto *scene = scenes_.emplace_back(std::make_unique<CanvasScene>(image, QRectF(QPointF(0, 0), size), true)).get();
        auto *view = views_.emplace_back(std::make_unique<CanvasView>(scene, nullptr, false, cross_tool_select)).get();
        view->resize(size);
        view->show();
        // Without an active scene hasFocus() is always false and text editing never starts.
        QEvent activate(QEvent::WindowActivate);
        QApplication::sendEvent(view->viewport(), &activate);
        scene->selection_model->initialize_confirmed_rect(QRectF(QPointF(0, 0), size));
        if (tool != "cursor") scene->tool_controller->activate(tool);
        return {scene, view};
    }

    RectItem *put_rect(CanvasScene *scene) {
        auto *item = new RectItem(QRectF(60, 60, 140, 90), QPen(RED, 4));
        scene->addItem(item);
        scene->undo_stack->push(new AddItemCommand(scene, ItemReference(item)));
        return item;
    }

    TextItem *write_text(CanvasScene *scene, CanvasView *view, const QString &text = "hello") {
        gesture(view, QPointF(120, 120), QPointF(120, 120), 1);
        const auto texts = items_of<TextItem>(scene);
        if (texts.isEmpty()) return nullptr;
        texts.first()->setPlainText(text);
        QApplication::processEvents();
        return texts.first();
    }

private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        settings_ = new QSettings(dir_.filePath("settings.ini"), QSettings::IniFormat, this);
        config_ = get_tool_settings_manager(settings_);
        // The offscreen platform has no font directory; text needs a real font for its geometry.
        const int font_id = QFontDatabase::addApplicationFont(QDir(qEnvironmentVariable("WINDIR")).filePath("Fonts/arial.ttf"));
        QVERIFY(font_id >= 0);
        font_family = QFontDatabase::applicationFontFamilies(font_id).value(0);
    }

    void init() { settings_->clear(); }

    void cleanup() {
        for (auto &view : views_) view->cleanup();
        views_.clear();
        scenes_.clear();
    }

    // ---- One user action, one undo step ----

    void oneGesturePushesExactlyOneCommand_data() {
        QTest::addColumn<QString>("name");
        for (const char *name : {"rect", "pen", "arrow", "ellipse", "mosaic", "number", "text", "move rect", "delete rect"}) QTest::newRow(name) << QString(name);
    }

    void oneGesturePushesExactlyOneCommand() {
        QFETCH(QString, name);
        const QString tool = name == "move rect" || name == "delete rect" ? QString("rect") : name;
        auto [scene, view] = canvas(tool);
        RectItem *existing = nullptr;
        if (name == "move rect" || name == "delete rect") existing = put_rect(scene);
        if (name == "delete rect") view->smart_edit_controller->select_item(existing);
        QApplication::processEvents();
        const int before = scene->undo_stack->count();
        if (name == "number") {
            gesture(view, QPointF(120, 120), QPointF(120, 120), 1);
        } else if (name == "text") {
            auto *item = write_text(scene, view);
            QVERIFY(item);
            view->end_text_edit(item);
        } else if (name == "move rect") {
            gesture(view, QPointF(60, 105), QPointF(160, 175), 6);
            QCOMPARE(existing->scenePos(), QPointF(100, 70));
        } else if (name == "delete rect") {
            view->smart_edit_controller->delete_selected();
            QVERIFY(!existing->scene());
        } else {
            gesture(view, QPointF(60, 60), QPointF(220, 170), name == "pen" || name == "mosaic" ? 12 : 5);
        }
        QApplication::processEvents();
        QCOMPARE(scene->undo_stack->count() - before, 1);
        // The step undoes completely.
        scene->undo_stack->undo();
        if (name == "move rect") QCOMPARE(existing->scenePos(), QPointF(0, 0));
        if (name == "delete rect") QCOMPARE(existing->scene(), scene);
    }

    void eachToolDrawsItsOwnItem() {
        const QList<QPair<QString, std::function<bool(QGraphicsItem *)>>> cases = {
            {"rect", [](QGraphicsItem *i) { return dynamic_cast<RectItem *>(i) && !dynamic_cast<SpotlightItem *>(i); }},
            {"ellipse", [](QGraphicsItem *i) { return dynamic_cast<EllipseItem *>(i) != nullptr; }},
            {"arrow", [](QGraphicsItem *i) { return dynamic_cast<ArrowItem *>(i) != nullptr; }},
            {"pen", [](QGraphicsItem *i) { auto *s = dynamic_cast<StrokeItem *>(i); return s && !s->is_highlighter; }},
            {"highlighter", [](QGraphicsItem *i) { auto *s = dynamic_cast<StrokeItem *>(i); return s && s->is_highlighter; }},
            {"mosaic", [](QGraphicsItem *i) { return dynamic_cast<MosaicItem *>(i) != nullptr; }},
            {"spotlight", [](QGraphicsItem *i) { return dynamic_cast<SpotlightItem *>(i) != nullptr; }},
        };
        for (const auto &[tool, matches] : cases) {
            auto [scene, view] = canvas(tool);
            gesture(view, QPointF(60, 60), QPointF(220, 170), 8);
            bool found = false;
            for (auto *item : scene->items()) found = found || matches(item);
            QVERIFY2(found, qPrintable(tool));
        }
    }

    void aTinyDragDrawsNothing() {
        auto [scene, view] = canvas("rect");
        const int before = scene->undo_stack->count();
        gesture(view, QPointF(60, 60), QPointF(61, 61), 1);
        QCOMPARE(scene->undo_stack->count(), before);
        QVERIFY(items_of<RectItem>(scene).isEmpty());
    }

    void pressingWithoutMovingLeavesNoTrace() {
        auto [scene, view] = canvas("rect");
        auto *item = put_rect(scene);
        view->smart_edit_controller->select_item(item);
        QApplication::processEvents();
        const int before = scene->undo_stack->count();
        send(view, QEvent::MouseButtonPress, QPointF(60, 105));
        send(view, QEvent::MouseButtonRelease, QPointF(60, 105), Qt::NoButton);
        QCOMPARE(scene->undo_stack->count(), before);
    }

    void aLongDragIsStillOneStep() {
        auto [scene, view] = canvas("rect");
        put_rect(scene);
        const int before = scene->undo_stack->count();
        gesture(view, QPointF(60, 105), QPointF(260, 275), 40);
        QCOMPARE(scene->undo_stack->count() - before, 1);
    }

    void repeatedNumberEditsCollapseIntoOne() {
        auto [scene, view] = canvas("number");
        auto *item = new NumberItem(1, QPointF(150, 150), 20.0, RED);
        scene->addItem(item);
        scene->undo_stack->push(new AddItemCommand(scene, ItemReference(item)));
        const int before = scene->undo_stack->count();
        for (int n : {2, 3, 4}) {
            const QVariantMap old{{"number", item->number}};
            item->number = n;
            scene->undo_stack->push(new NumberEditCommand(ItemReference(item), old, {{"number", n}}));
        }
        QCOMPARE(scene->undo_stack->count() - before, 1);
        scene->undo_stack->undo();
        QCOMPARE(item->number, 1);
    }

    void textWrittenInOneGoIsOneStep() {
        auto [scene, view] = canvas("text");
        const int before = scene->undo_stack->count();
        auto *item = write_text(scene, view, "some words");
        QVERIFY(item);
        QVERIFY(item->is_editing());
        view->end_text_edit(item);
        QVERIFY(!item->is_editing());
        QCOMPARE(scene->undo_stack->count() - before, 1);
        scene->undo_stack->undo();
        QApplication::processEvents();
        QVERIFY(items_of<TextItem>(scene).isEmpty());
    }

    void textCreatedAndLeftEmptyLeavesNoTrace() {
        auto [scene, view] = canvas("text");
        const int before = scene->undo_stack->count();
        gesture(view, QPointF(120, 120), QPointF(120, 120), 1);
        const auto texts = items_of<TextItem>(scene);
        QCOMPARE(texts.size(), 1);
        view->end_text_edit(texts.first());
        QApplication::processEvents();
        QCOMPARE(scene->undo_stack->count(), before);
        QVERIFY(items_of<TextItem>(scene).isEmpty());
    }

    void draggingTextWhileEditingCanBeUndone() {
        auto [scene, view] = canvas("text");
        auto *item = write_text(scene, view);
        QVERIFY(item && item->is_editing());
        const QPointF start = item->pos();
        const int before = scene->undo_stack->count();
        const auto edge = text_drag_point(item);
        gesture(view, edge, edge + QPointF(60, 45), 4);
        QVERIFY(!view->text_drag->active);
        QVERIFY(item->pos() != start);
        QCOMPARE(scene->undo_stack->count() - before, 1);
        scene->undo_stack->undo();
        QCOMPARE(item->pos(), start);
    }

    void draggingTextAfterEditingCanBeUndone() {
        auto [scene, view] = canvas("text");
        auto *item = write_text(scene, view);
        QVERIFY(item);
        view->end_text_edit(item);
        QApplication::processEvents();
        QVERIFY(!item->is_editing());
        const QPointF start = item->pos();
        const int before = scene->undo_stack->count();
        const auto edge = text_drag_point(item);
        gesture(view, edge, edge + QPointF(60, 45), 4);
        QVERIFY(item->pos() != start);
        QCOMPARE(scene->undo_stack->count() - before, 1);
        scene->undo_stack->undo();
        QCOMPARE(item->pos(), start);
    }

    // ---- Width, opacity and line style of the selection ----

    void sizeChangesScaleTheSelectionOnly() {
        auto [scene, view] = canvas();
        auto *untouched = new_stroke(4.0);
        scene->addItem(untouched);
        view->apply_size_change_to_selection(2.0);
        QCOMPARE(*untouched->get_stroke_width(), 4.0);

        auto *item = new_stroke(4.0);
        scene->addItem(item);
        view->smart_edit_controller->set_selected_item(item);
        view->apply_size_change_to_selection(2.0);
        QVERIFY(std::abs(*item->get_stroke_width() - 8.0) < 1e-6);
        // Zero or negative scales would make the shape vanish.
        for (double bad : {0.0, -1.0, -0.5}) view->apply_size_change_to_selection(bad);
        QVERIFY(std::abs(*item->get_stroke_width() - 8.0) < 1e-6);
        for (int i = 0; i < 20; ++i) view->apply_size_change_to_selection(0.5);
        QVERIFY(*item->get_stroke_width() >= 1.0);
        QCOMPARE(*untouched->get_stroke_width(), 4.0);
    }

    void textIsScaledByFontSizeWithAFloor() {
        auto [scene, view] = canvas();
        auto *item = new_text("abc", QPointF(0, 0), 16);
        scene->addItem(item);
        view->smart_edit_controller->set_selected_item(item);
        view->apply_size_change_to_selection(2.0);
        QVERIFY(std::abs(item->font_point_size() - 32) <= 0.5);
        for (int i = 0; i < 20; ++i) view->apply_size_change_to_selection(0.5);
        QVERIFY(item->font_point_size() >= 6.0);

        // A plain text item without the drawing interface falls back to its font.
        auto *plain = new QGraphicsTextItem("abc");
        auto font = plain->font();
        font.setPointSizeF(10);
        plain->setFont(font);
        scene->addItem(plain);
        view->smart_edit_controller->set_selected_item(plain);
        view->apply_size_change_to_selection(2.0);
        QVERIFY(std::abs(plain->font().pointSizeF() - 20) <= 0.5);
    }

    void opacityLivesOnTheColourNotTheItem() {
        auto [scene, view] = canvas();
        auto *untouched = new_stroke();
        scene->addItem(untouched);
        view->apply_opacity_change_to_selection(0.2);
        QVERIFY(std::abs(*untouched->get_visual_opacity() - 1.0) < 0.01);
        auto *item = new_stroke();
        scene->addItem(item);
        view->smart_edit_controller->set_selected_item(item);
        view->apply_opacity_change_to_selection(0.5);
        QVERIFY(std::abs(*item->get_visual_opacity() - 0.5) < 0.01);
        // Item-level opacity would blend twice on export.
        QCOMPARE(item->opacity(), 1.0);
        view->apply_opacity_change_to_selection(5.0);
        QVERIFY(std::abs(*item->get_visual_opacity() - 1.0) < 0.01);
        view->apply_opacity_change_to_selection(-1.0);
        QVERIFY(std::abs(*item->get_visual_opacity()) < 0.01);
        // Width changes keep the opacity.
        view->apply_opacity_change_to_selection(0.25);
        view->apply_size_change_to_selection(2.0);
        QVERIFY(std::abs(*item->get_visual_opacity() - 0.25) < 0.01);
        QVERIFY(std::abs(*item->get_stroke_width() - 8.0) < 1e-6);
    }

    void lineStyleAppliesOnlyToItsOwnTools() {
        auto [scene, view] = canvas();
        // Nothing selected: no effect, no crash.
        view->apply_line_style_change_to_selection("dashed");
        auto *stroke = new_stroke();
        scene->addItem(stroke);
        view->smart_edit_controller->set_selected_item(stroke);
        view->apply_line_style_change_to_selection("dashed");
        QVERIFY(is_dashed(stroke->pen()));
        QCOMPARE(stroke->pen().dashPattern(), (QList<qreal>{3, 2}));
        const auto loose = stroke->pen().dashPattern();
        view->apply_line_style_change_to_selection("dashed_dense");
        const auto dense = stroke->pen().dashPattern();
        QVERIFY(dense != loose);
        QVERIFY(std::accumulate(dense.begin(), dense.end(), 0.0) < std::accumulate(loose.begin(), loose.end(), 0.0));

        auto *rect = new_rect();
        scene->addItem(rect);
        view->smart_edit_controller->set_selected_item(rect);
        view->apply_line_style_change_to_selection("dashed");
        QVERIFY(is_dashed(rect->pen()));
        view->apply_line_style_change_to_selection("solid");
        QCOMPARE(rect->pen().style(), Qt::SolidLine);

        // Highlighters, highlighter rects, spotlight holes and text have no line style.
        auto *highlighter = new_stroke(4.0, true);
        auto *highlighter_rect = new_rect();
        highlighter_rect->is_highlighter = true;
        highlighter_rect->is_highlighter_rect = true;
        auto *spotlight = new SpotlightItem(QRectF(0, 0, 100, 60));
        for (QAbstractGraphicsShapeItem *item : std::initializer_list<QAbstractGraphicsShapeItem *>{highlighter, highlighter_rect, spotlight}) {
            scene->addItem(item);
            const auto before = item->pen().style();
            view->smart_edit_controller->set_selected_item(item);
            view->apply_line_style_change_to_selection("dashed");
            QCOMPARE(item->pen().style(), before);
        }
        auto *text = new_text("abc");
        scene->addItem(text);
        view->smart_edit_controller->set_selected_item(text);
        view->apply_line_style_change_to_selection("dashed");
        QCOMPARE(text->toPlainText(), QString("abc"));
    }

    void cleanupIsIdempotent() {
        auto [scene, view] = canvas();
        Q_UNUSED(scene);
        view->cleanup();
        view->cleanup();
        // Late callbacks after cleanup are ignored.
        view->request_handles_repaint();
        view->update_edit_handles();
    }

    // ---- Handles: text ----

    void selectedTextHasRotateDeleteAndScaleHandles() {
        std::unique_ptr<TextItem> item(new_text("12323"));
        LayerEditor editor;
        QVERIFY(editor.start_edit(item.get()));
        const auto rect = item->interaction_rect();
        QCOMPARE(editor.handles.size(), size_t(3));
        QCOMPARE(handle_of(editor, HandleType::ROTATE)->position, rect.topLeft());
        QCOMPARE(handle_of(editor, HandleType::ITEM_DELETE)->position, rect.topRight());
        QCOMPARE(handle_of(editor, HandleType::TEXT_SCALE)->position, rect.bottomRight());
        QVERIFY(LayerEditor::is_delete_handle(handle_of(editor, HandleType::ITEM_DELETE)));
        QVERIFY(!LayerEditor::is_delete_handle(handle_of(editor, HandleType::TEXT_SCALE)));
        QVERIFY(!LayerEditor::is_delete_handle(handle_of(editor, HandleType::ROTATE)));
        // The snapshot carries the font size, not a wrapping width.
        const auto state = editor.capture_state(item.get());
        QVERIFY(state);
        QVERIFY(std::abs(state->value("font_point_size").toDouble() - item->font_point_size()) < 1e-6);
        QVERIFY(!state->contains("text_width"));
    }

    void theScaleHandleGrowsShrinksAndClamps() {
        std::unique_ptr<TextItem> item(new_text("12323"));
        LayerEditor editor;
        editor.start_edit(item.get());
        const double base = item->font_point_size();
        auto handle = *handle_of(editor, HandleType::TEXT_SCALE);
        editor.start_drag(handle, handle.position);
        editor.drag_to(handle.position + QPointF(40, 20));
        editor.end_drag();
        const double grown = item->font_point_size();
        QVERIFY(grown > base);
        handle = *handle_of(editor, HandleType::TEXT_SCALE);
        editor.start_drag(handle, handle.position);
        editor.drag_to(handle.position - QPointF(30, 15));
        editor.end_drag();
        QVERIFY(item->font_point_size() < grown);
        handle = *handle_of(editor, HandleType::TEXT_SCALE);
        editor.start_drag(handle, handle.position);
        editor.drag_to(handle.position - QPointF(5000, 5000));
        editor.end_drag();
        QCOMPARE(item->font_point_size(), TextItem::MIN_POINT_SIZE);
    }

    void handlesFollowTypingAndFontChanges() {
        QGraphicsScene scene;
        auto *item = new_text("1");
        scene.addItem(item);
        LayerEditor editor;
        editor.start_edit(item);
        const auto before_delete = handle_of(editor, HandleType::ITEM_DELETE)->position;
        const double narrow = item->interaction_rect().right();
        item->setPlainText("121212121212");
        QVERIFY(item->interaction_rect().right() > narrow);
        editor.refresh_handles();
        QCOMPARE(handle_of(editor, HandleType::ITEM_DELETE)->position, item->interaction_rect().topRight());
        QVERIFY(handle_of(editor, HandleType::ITEM_DELETE)->position != before_delete);
        // render() refreshes stale handles itself.
        item->setPlainText("1212121212121212121212");
        QImage image(600, 200, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        QPainter painter(&image);
        editor.render(&painter);
        painter.end();
        QCOMPARE(handle_of(editor, HandleType::TEXT_SCALE)->position, item->interaction_rect().bottomRight());
        const auto before_scale = handle_of(editor, HandleType::TEXT_SCALE)->position;
        item->set_font_point_size(item->font_point_size() * 2);
        editor.refresh_handles();
        const auto after = handle_of(editor, HandleType::TEXT_SCALE)->position;
        QCOMPARE(after, item->interaction_rect().bottomRight());
        QVERIFY(after.x() > before_scale.x() && after.y() > before_scale.y());
    }

    void activeTextEditStartsScaleDragOnFirstPress() {
        auto [scene, view] = canvas("text", false, QSize(400, 200));
        auto *item = new TextItem("edit me", QPointF(10, 10), text_font(), QColor("black"));
        scene->addItem(item);
        view->enter_text_edit_mode(item);
        auto *editor = view->smart_edit_controller->layer_editor.get();
        const auto handle = *handle_of(*editor, HandleType::TEXT_SCALE);
        const double initial = item->font_point_size();
        send(view, QEvent::MouseButtonPress, handle.position);
        QVERIFY(editor->dragging_handle && editor->dragging_handle->id == handle.id);
        QVERIFY(!view->text_drag->active);
        send(view, QEvent::MouseMove, handle.position + QPointF(30, 15));
        send(view, QEvent::MouseButtonRelease, handle.position + QPointF(30, 15), Qt::NoButton);
        QVERIFY(item->font_point_size() > initial);
        QVERIFY(scene->undo_stack->canUndo());
        scene->undo_stack->undo();
        QVERIFY(std::abs(item->font_point_size() - initial) < 1e-6);
    }

    // ---- Handles: strokes and shapes ----

    void aSecondStrokeResizeCompounds() {
        QGraphicsScene scene(0, 0, 800, 600);
        QPainterPath path(QPointF(0, 0));
        path.lineTo(QPointF(100, 50));
        auto *item = new StrokeItem(path, QPen(QColor("black"), 4));
        scene.addItem(item);
        LayerEditor editor;
        editor.start_edit(item);
        const double original = item->sceneBoundingRect().width();
        auto handle = *handle_of(editor, HandleType::CORNER_BR);
        editor.start_drag(handle, handle.position);
        editor.drag_to(handle.position + QPointF(100, 50));
        editor.end_drag();
        const double first = item->sceneBoundingRect().width();
        QVERIFY(first > original * 1.5);
        // Grabbing the same handle again continues from the new size.
        editor.start_edit(item);
        handle = *handle_of(editor, HandleType::CORNER_BR);
        editor.start_drag(handle, handle.position);
        editor.drag_to(handle.position + QPointF(40, 20));
        editor.end_drag();
        const double second = item->sceneBoundingRect().width();
        QVERIFY(second > first);
        QVERIFY(std::abs(second - (original + 140)) <= 5);
    }

    void rectHandlesResizeRotateAndDeleteThroughTheView() {
        auto [scene, view] = canvas("rect");
        auto *item = put_rect(scene);
        // A plain click of the matching tool selects it.
        send(view, QEvent::MouseButtonPress, QPointF(60, 105));
        send(view, QEvent::MouseButtonRelease, QPointF(60, 105), Qt::NoButton);
        QCOMPARE(view->smart_edit_controller->selected_item(), item);
        auto *editor = view->smart_edit_controller->layer_editor.get();
        QVERIFY(editor->is_editing());

        // The bottom-right corner resizes, as one undo step.
        const int before = scene->undo_stack->count();
        const auto corner = handle_point(view, *handle_of(*editor, HandleType::CORNER_BR));
        gesture(view, corner, corner + QPointF(40, 30), 4);
        QCOMPARE(item->rect(), QRectF(60, 60, 180, 120));
        QCOMPARE(scene->undo_stack->count() - before, 1);
        scene->undo_stack->undo();
        QCOMPARE(item->rect(), QRectF(60, 60, 140, 90));
        // The left edge moves only that side.
        editor->start_edit(item);
        const auto left = handle_point(view, *handle_of(*editor, HandleType::EDGE_L));
        gesture(view, left, left + QPointF(20, 50), 2);
        QCOMPARE(item->mapRectToScene(item->rect()), QRectF(80, 60, 120, 90));
        scene->undo_stack->undo();

        // The rotate handle turns the item around its centre.
        editor->start_edit(item);
        const auto rotate = handle_point(view, *handle_of(*editor, HandleType::ROTATE));
        const auto centre = item->mapRectToScene(item->rect()).center();
        gesture(view, rotate, QPointF(centre.x() + 120, centre.y()), 4);
        QVERIFY(std::abs(item->rotation()) > 1);
        // Handles follow the rotation instead of the scene bounding box.
        const auto turned = *handle_of(*editor, HandleType::CORNER_BR);
        QCOMPARE(turned.position, item->mapToScene(item->rect().bottomRight()));
        scene->undo_stack->undo();
        QCOMPARE(item->rotation(), 0.0);

        // Rects have no delete button; text does.
        QVERIFY(!handle_of(*editor, HandleType::ITEM_DELETE));
    }

    void theTextDeleteButtonRemovesItUndoably() {
        auto [scene, view] = canvas("text");
        auto *item = write_text(scene, view, "remove me");
        QVERIFY(item);
        view->end_text_edit(item);
        view->smart_edit_controller->select_item(item);
        auto *editor = view->smart_edit_controller->layer_editor.get();
        const auto remove = handle_point(view, *handle_of(*editor, HandleType::ITEM_DELETE));
        send(view, QEvent::MouseButtonPress, remove);
        send(view, QEvent::MouseButtonRelease, remove, Qt::NoButton);
        QVERIFY(!item->scene());
        scene->undo_stack->undo();
        QCOMPARE(item->scene(), scene);
        QCOMPARE(item->toPlainText(), QString("remove me"));
    }

    void clearingCommittedTextIsUndoable() {
        auto [scene, view] = canvas("text");
        auto *item = write_text(scene, view, "keep me");
        QVERIFY(item);
        view->end_text_edit(item);
        const int before = scene->undo_stack->count();
        // Editing again and erasing everything removes it, as one undoable step.
        view->enter_text_edit_mode(item);
        QVERIFY(item->is_editing());
        item->setPlainText("   ");
        view->end_text_edit(item);
        QApplication::processEvents();
        QVERIFY(!item->scene() || !item->isVisible());
        QCOMPARE(scene->undo_stack->count() - before, 1);
        scene->undo_stack->undo();
        QCOMPARE(item->scene(), scene);
        QCOMPARE(item->toPlainText(), QString("keep me"));
    }

    void cornerRadiusHandleRoundsTheRect() {
        auto [scene, view] = canvas("rect");
        auto *item = put_rect(scene);
        view->smart_edit_controller->select_item(item);
        auto *editor = view->smart_edit_controller->layer_editor.get();
        const auto radius = handle_of(*editor, HandleType::CORNER_RADIUS);
        QVERIFY(radius);
        QCOMPARE(item->get_corner_radius(), 0.0);
        const auto radius_point = handle_point(view, *radius);
        gesture(view, radius_point, radius_point + QPointF(20, 20), 3);
        QVERIFY(item->get_corner_radius() > 0);
        scene->undo_stack->undo();
        QCOMPARE(item->get_corner_radius(), 0.0);
    }

    // The radius last dragged on a rectangle is where the next one starts.
    void theDraggedCornerRadiusCarriesToTheNextRect() {
        const auto restore = qScopeGuard([this] { config_->reset_tool("rect"); });
        auto [scene, view] = canvas("rect");
        auto *item = put_rect(scene);
        view->smart_edit_controller->select_item(item);
        auto *editor = view->smart_edit_controller->layer_editor.get();
        const auto radius_point = handle_point(view, *handle_of(*editor, HandleType::CORNER_RADIUS));
        gesture(view, radius_point, radius_point + QPointF(20, 20), 3);
        const double dragged = item->get_corner_radius();
        QVERIFY(dragged > 0);
        QCOMPARE(config_->get_setting("rect", "corner_radius").toLongLong(), std::llround(dragged));

        // The next capture's rectangle starts rounded.
        auto [next_scene, next_view] = canvas("rect");
        gesture(next_view, QPointF(60, 60), QPointF(220, 170), 5);
        RectItem *drawn = nullptr;
        for (auto *candidate : next_scene->items())
            if (auto *rect = dynamic_cast<RectItem *>(candidate)) drawn = rect;
        QVERIFY(drawn);
        QCOMPARE(drawn->get_corner_radius(), double(std::llround(dragged)));
    }

    // Highlighter rectangles share the item class but not the rectangle tool's setting.
    void aHighlighterRectKeepsTheRectRadius() {
        const auto restore = qScopeGuard([this] { config_->reset_tool("rect"); });
        auto [scene, view] = canvas("rect");
        auto *item = put_rect(scene);
        item->is_highlighter_rect = true;
        view->smart_edit_controller->select_item(item);
        auto *editor = view->smart_edit_controller->layer_editor.get();
        const auto radius = handle_of(*editor, HandleType::CORNER_RADIUS);
        QVERIFY(radius);
        const auto radius_point = handle_point(view, *radius);
        gesture(view, radius_point, radius_point + QPointF(20, 20), 3);
        QVERIFY(item->get_corner_radius() > 0);
        QCOMPARE(config_->get_setting("rect", "corner_radius").toLongLong(), 0);
    }

    void arrowEndpointsAndControlPointDrag() {
        auto [scene, view] = canvas("arrow");
        auto *arrow = new ArrowItem(QPointF(100, 100), QPointF(300, 100), QPen(RED, 4));
        scene->addItem(arrow);
        view->smart_edit_controller->select_item(arrow);
        auto *editor = view->smart_edit_controller->layer_editor.get();
        const auto end = *handle_of(*editor, HandleType::ARROW_END);
        QCOMPARE(end.position, QPointF(300, 100));
        gesture(view, handle_point(view, end), QPointF(320, 160), 3);
        QCOMPARE(arrow->mapToScene(arrow->end_pos), QPointF(320, 160));
        QVERIFY(!arrow->is_curved());
        const auto control = *handle_of(*editor, HandleType::ARROW_CONTROL);
        const auto control_point = handle_point(view, control);
        gesture(view, control_point, control_point + QPointF(0, -60), 3);
        QVERIFY(arrow->is_curved());
        scene->undo_stack->undo();
        QVERIFY(!arrow->is_curved());
        scene->undo_stack->undo();
        QCOMPARE(arrow->mapToScene(arrow->end_pos), QPointF(300, 100));
    }

    void storingABendsPointsKeepsItsShape_data() {
        QTest::addColumn<QString>("style");
        QTest::addColumn<QPointF>("control");
        QTest::addColumn<double>("tolerance");
        // A lopsided bend is tight near one end, where the single bend's quadratic edges run
        // thin; the stored curve keeps the full width there.
        for (const char *style : {"single", "double", "line_double", "bar_arrow"}) {
            QTest::newRow(qPrintable(QString("%1 even").arg(style))) << QString(style) << QPointF(200, 60) << .5;
            QTest::newRow(qPrintable(QString("%1 lopsided").arg(style))) << QString(style) << QPointF(110, 70) << 2.0;
        }
    }

    void storingABendsPointsKeepsItsShape() {
        QFETCH(QString, style);
        QFETCH(QPointF, control);
        QFETCH(double, tolerance);
        ArrowItem single(QPointF(40, 150), QPointF(360, 150), QPen(RED, 6), style);
        single.set_control_point(control);
        ArrowItem stored(QPointF(40, 150), QPointF(360, 150), QPen(RED, 6), style);
        stored.set_control_point(control);
        const auto bends = stored.bend_points();
        // Offered on the drawn shaft, not on the curve the heads are trimmed from.
        QVERIFY(single.path().contains(bends[0]));
        QVERIFY(single.path().contains(bends[1]));
        stored.set_bend_point(0, bends[0]);
        QVERIFY(stored.has_bend_points());
        QCOMPARE(stored.bend_points(), bends);
        QVERIFY(same_outline(single.path(), stored.path(), tolerance));
        QVERIFY(same_outline(stored.path(), single.path(), tolerance));
    }

    void bendPointsMakeAnS() {
        ArrowItem arrow(QPointF(40, 100), QPointF(360, 100), QPen(RED, 4), "triangle");
        arrow.set_control_point(QPointF(200, 100));
        arrow.set_bend_points(std::array<QPointF, 2>{{QPointF(110, 40), QPointF(290, 160)}});
        const auto path = arrow.path();
        for (const auto &on : {QPointF(110, 40), QPointF(200, 100), QPointF(290, 160)}) QVERIFY(path.contains(on));
        for (const auto &off : {QPointF(110, 160), QPointF(290, 40), QPointF(110, 100), QPointF(290, 100)}) QVERIFY(!path.contains(off));
        // Rising steeply out of the lower bend, the head points up and to the right.
        QVERIFY(path.contains(QPointF(356, 108)));
        QVERIFY(!path.contains(QPointF(350, 100)));
        arrow.reset_control_point();
        QVERIFY(!arrow.has_bend_points());
        QVERIFY(!arrow.is_curved());
        QCOMPARE(arrow.path(), ArrowItem(QPointF(40, 100), QPointF(360, 100), QPen(RED, 4), "triangle").path());
    }

    void bendHandlesAppearOnLongBentArrows() {
        auto [scene, view] = canvas("arrow");
        auto *arrow = new ArrowItem(QPointF(100, 160), QPointF(400, 160), QPen(RED, 4));
        scene->addItem(arrow);
        auto *stub = new ArrowItem(QPointF(100, 320), QPointF(180, 320), QPen(RED, 4));
        stub->set_control_point(QPointF(140, 300));
        scene->addItem(stub);
        auto *editor = view->smart_edit_controller->layer_editor.get();
        view->smart_edit_controller->select_item(stub);
        QVERIFY(handles_of(*editor, HandleType::ARROW_BEND).empty());
        view->smart_edit_controller->select_item(arrow);
        QVERIFY(handles_of(*editor, HandleType::ARROW_BEND).empty());
        const auto control_point = handle_point(view, *handle_of(*editor, HandleType::ARROW_CONTROL));
        gesture(view, control_point, control_point + QPointF(0, -60), 3);
        const auto bends = handles_of(*editor, HandleType::ARROW_BEND);
        QCOMPARE(bends.size(), size_t(2));
        QVERIFY(!arrow->has_bend_points());
        for (const auto &bend : bends) QVERIFY(arrow->path().contains(arrow->mapFromScene(bend.position)));

        const auto grab = handle_point(view, bends[1]);
        gesture(view, grab, grab + QPointF(0, 90), 4);
        QVERIFY(arrow->has_bend_points());
        QVERIFY(QLineF(arrow->mapToScene(arrow->bend_points()[1]), grab + QPointF(0, 90)).length() < 1.5);
        QVERIFY(arrow->path().contains(arrow->mapFromScene(grab + QPointF(0, 90))));
        const auto s_path = arrow->path();
        scene->undo_stack->undo();
        QVERIFY(!arrow->has_bend_points());
        QVERIFY(arrow->is_curved());
        scene->undo_stack->redo();
        QVERIFY(arrow->has_bend_points());
        QCOMPARE(arrow->path(), s_path);

        // Stored bend points stay editable after the arrow is shortened below the spacing.
        const auto end_point = handle_point(view, *handle_of(*editor, HandleType::ARROW_END));
        gesture(view, end_point, QPointF(150, 160), 4);
        QCOMPARE(handles_of(*editor, HandleType::ARROW_BEND).size(), size_t(2));
        // A style change records no bend points and leaves them alone.
        auto old_state = view->smart_edit_controller->layer_editor->capture_state(arrow).value();
        old_state.remove("bend_points");
        old_state["arrow_style"] = arrow->arrow_style();
        auto new_state = old_state;
        new_state["arrow_style"] = QString("double");
        scene->undo_stack->push(new EditItemCommand(ItemReference(arrow), old_state, new_state));
        QCOMPARE(arrow->arrow_style(), QString("double"));
        QVERIFY(arrow->has_bend_points());
        scene->undo_stack->undo();
        QCOMPARE(arrow->arrow_style(), QString("single"));
        QVERIFY(arrow->has_bend_points());
    }

    void crowdedArrowHandlesPickTheNearest() {
        auto [scene, view] = canvas("arrow");
        auto *arrow = new ArrowItem(QPointF(100, 100), QPointF(140, 100), QPen(RED, 4));
        scene->addItem(arrow);
        view->smart_edit_controller->select_item(arrow);
        auto *editor = view->smart_edit_controller->layer_editor.get();
        const auto start = handle_point(view, *handle_of(*editor, HandleType::ARROW_START));
        const auto control = handle_point(view, *handle_of(*editor, HandleType::ARROW_CONTROL));
        // Both hit areas cover the point between them; the closer handle wins, whatever the order.
        QCOMPARE(editor->hit_test(control - QPointF(3, 0))->handle_type, HandleType::ARROW_CONTROL);
        QCOMPARE(editor->hit_test(start + QPointF(3, 0))->handle_type, HandleType::ARROW_START);
        QCOMPARE(handle_type_name(HandleType::ARROW_BEND), QString("arrow_bend"));
    }

    void numberHandlesAdjustAndDelete() {
        auto [scene, view] = canvas("number");
        auto *number = new NumberItem(3, QPointF(200, 200), 20.0, RED);
        scene->addItem(number);
        scene->undo_stack->push(new AddItemCommand(scene, ItemReference(number)));
        view->smart_edit_controller->select_item(number);
        auto *editor = view->smart_edit_controller->layer_editor.get();
        const auto increment = handle_of(*editor, HandleType::NUMBER_INCREMENT);
        const auto decrement = handle_of(*editor, HandleType::NUMBER_DECREMENT);
        QVERIFY(increment && decrement);
        QVERIFY(LayerEditor::is_number_adjust_handle(increment));
        const auto click_handle = [&](HandleType type) {
            const auto point = handle_point(view, *handle_of(*editor, type));
            send(view, QEvent::MouseButtonPress, point);
            send(view, QEvent::MouseButtonRelease, point, Qt::NoButton);
        };
        click_handle(HandleType::NUMBER_INCREMENT);
        QCOMPARE(number->number, 4);
        click_handle(HandleType::NUMBER_DECREMENT);
        click_handle(HandleType::NUMBER_DECREMENT);
        QCOMPARE(number->number, 2);
        // Rapid clicks on one number merge into one step.
        scene->undo_stack->undo();
        QCOMPARE(number->number, 3);
        click_handle(HandleType::NUMBER_DELETE);
        QVERIFY(!number->scene());
        scene->undo_stack->undo();
        QCOMPARE(number->scene(), scene);
    }

    void numberButtonsSpreadOutAroundATinyNumber() {
        NumberItem tiny(1, QPointF(100, 100), 4.0, RED);
        LayerEditor editor;
        // Zoomed out to half, the circle is 4 screen pixels across.
        editor.screen_transform = [] { return QTransform::fromScale(.5, .5); };
        QVERIFY(editor.start_edit(&tiny));
        // The buttons hang on a frame about the centre: 14 + 6 screen pixels (40 units) wide, and tall
        // enough for + and - stacked above the pointer button, two steps of 18 (72 units).
        QCOMPARE(handle_of(editor, HandleType::NUMBER_INCREMENT)->position, QPointF(80, 64));
        QCOMPARE(handle_of(editor, HandleType::NUMBER_DECREMENT)->position, QPointF(80, 64));
        QCOMPARE(handle_of(editor, HandleType::NUMBER_DECREMENT)->offset, QPointF(0, 18));
        QCOMPARE(handle_of(editor, HandleType::NUMBER_DELETE)->position, QPointF(120, 64));
        QCOMPARE(handle_of(editor, HandleType::NUMBER_ARROW)->position, QPointF(80, 136));
        QCOMPARE(handle_of(editor, HandleType::NUMBER_LABEL)->position, QPointF(120, 136));
        for (size_t i = 0; i < editor.handles.size(); ++i)
            for (size_t j = i + 1; j < editor.handles.size(); ++j)
                QVERIFY(!editor.screen_rect(editor.handles[i]).intersects(editor.screen_rect(editor.handles[j])));
        // A number large enough keeps them on its own corners.
        NumberItem large(1, QPointF(100, 100), 30.0, RED);
        editor.screen_transform = {};
        QVERIFY(editor.start_edit(&large));
        QCOMPARE(handle_of(editor, HandleType::NUMBER_INCREMENT)->position, QPointF(70, 70));
        QCOMPARE(handle_of(editor, HandleType::NUMBER_DELETE)->position, QPointF(130, 70));
    }

    void textButtonsKeepApartOnTinyText() {
        std::unique_ptr<TextItem> item(new_text("1", QPointF(0, 0), 6));
        LayerEditor editor;
        editor.screen_transform = [] { return QTransform::fromScale(.25, .25); };
        QVERIFY(editor.start_edit(item.get()));
        for (size_t i = 0; i < editor.handles.size(); ++i)
            for (size_t j = i + 1; j < editor.handles.size(); ++j)
                QVERIFY(!editor.screen_rect(editor.handles[i]).intersects(editor.screen_rect(editor.handles[j])));
        const auto rotate = *handle_of(editor, HandleType::ROTATE);
        const auto remove = *handle_of(editor, HandleType::ITEM_DELETE);
        QCOMPARE(editor.screen_center(remove).x() - editor.screen_center(rotate).x(), 20.);
        // Inside the delete button but also within the rotate button's padding: the button itself wins.
        const auto screen_point = editor.screen_center(remove) - QPointF(6, 0);
        QVERIFY(rotate.hit_rect_at(editor.screen_center(rotate)).contains(screen_point));
        QCOMPARE(editor.hit_test(screen_point * 4)->handle_type, HandleType::ITEM_DELETE);
    }

    void numberPointerIsATailNearTheCircleAndAnArrowFurtherOut() {
        NumberItem number(1, QPointF(0, 0), 20.0, RED);
        QVERIFY(number.pointer_path().isEmpty());
        number.set_arrow_tip(QPointF(15, 0));
        QVERIFY(number.pointer_path().isEmpty());
        // Within two radii: a tail from the circle 30 degrees either side to the tip.
        number.set_arrow_tip(QPointF(36, 0));
        const auto tail = number.pointer_path();
        QCOMPARE(tail.boundingRect().right(), 36.);
        QCOMPARE(tail.boundingRect().top(), -10.);
        QVERIFY(tail.contains(QPointF(30, 0)));
        // Further out: the arrow tool's arrow in the chosen style, 0.2 radii wide, from under the circle.
        number.set_arrow_tip(QPointF(100, 0));
        const auto arrow = number.pointer_path();
        QVERIFY(arrow.contains(QPointF(60, 0)));
        QVERIFY(arrow.contains(QPointF(98, 0)));
        QVERIFY(!arrow.contains(QPointF(101, 0)));
        QCOMPARE(arrow, ArrowItem(QPointF(14, 0), QPointF(100, 0), QPen(RED, 4), "single").path());
        QCOMPARE(number.boundingRect().right(), 100 + NumberItem::CLICK_MARGIN);
        QVERIFY(number.shape().contains(QPointF(60, 0)));
        number.apply_extras_state({{"arrow_style", "line"}});
        QCOMPARE(number.arrow_style(), QString("line"));
        QCOMPARE(number.pointer_path(), ArrowItem(QPointF(14, 0), QPointF(100, 0), QPen(RED, 4), "line").path());
        // Double-headed and unknown styles are not taken.
        number.apply_extras_state({{"arrow_style", "double"}});
        QCOMPARE(number.arrow_style(), QString("line"));
    }

    void numberLabelSizeOutlineAndGuide() {
        NumberItem number(1, QPointF(0, 0), 20.0, RED);
        number.set_label("ab", QPointF(26, 0));
        auto *label = number.label();
        // Five sizes in shares of the radius, the middle by default.
        QCOMPARE(label->font().pixelSize(), 18);
        number.apply_extras_state({{"label_size", 4}});
        QCOMPARE(label->font().pixelSize(), 29);
        number.apply_extras_state({{"label_size", 0}});
        QCOMPARE(label->font().pixelSize(), 10);
        // White around dark text, black around light text.
        QCOMPARE(number.label_outline_color(), QColor(Qt::white));
        number.color = QColor("#FFEE00");
        QCOMPARE(number.label_outline_color(), QColor(Qt::black));
        // The guide line: off by default; on, it leaves the circle towards the text's bottom-left
        // corner and runs under the text.
        QVERIFY(!number.guide() || !number.guide()->isVisible());
        number.apply_extras_state({{"label_guide", true}});
        QVERIFY(number.guide() && number.guide()->isVisible());
        const qreal margin = label->document()->documentMargin();
        const auto text = label->mapRectToParent(label->boundingRect().adjusted(margin, margin, -margin, -margin));
        const auto path = number.guide()->path();
        QCOMPARE(path.elementCount(), 3);
        QCOMPARE(QPointF(path.elementAt(1)), text.bottomLeft());
        QCOMPARE(QPointF(path.elementAt(2)), text.bottomRight());
        QCOMPARE(QLineF(QPointF(), QPointF(path.elementAt(0))).length(), 20.);
        // Over the circle it drops straight from the top of the circle to the text's bottom edge.
        label->set_anchor(QPointF(-30, -60));
        label->setPlainText("a wide label");
        const auto above = number.guide()->path();
        QCOMPARE(QPointF(above.elementAt(0)), QPointF(0, -20));
        QCOMPARE(above.elementAt(1).x, 0.);
        // Gone with the text.
        number.set_label(QString());
        QVERIFY(!number.guide()->isVisible());
        // The style rides in the undo state.
        const auto state = number.extras_state();
        NumberItem copy(1, QPointF(0, 0), 20.0, RED);
        copy.apply_extras_state(state);
        QCOMPARE(copy.label_size_level(), 0);
        QVERIFY(copy.label_guide());
    }

    void numberPointerTogglesDragsAndRemembersItsTip() {
        auto [scene, view] = canvas("number");
        auto *number = new NumberItem(1, QPointF(200, 200), 20.0, RED);
        scene->addItem(number);
        scene->undo_stack->push(new AddItemCommand(scene, ItemReference(number)));
        view->smart_edit_controller->select_item(number);
        auto *editor = view->smart_edit_controller->layer_editor.get();
        QCOMPARE(handle_of(*editor, HandleType::NUMBER_ARROW)->position, QPointF(180, 220));
        QCOMPARE(handle_of(*editor, HandleType::NUMBER_LABEL)->position, QPointF(220, 220));
        QVERIFY(!handle_of(*editor, HandleType::NUMBER_ARROW_TIP));
        const auto click = [&](HandleType type) {
            const auto point = handle_point(view, *handle_of(*editor, type));
            send(view, QEvent::MouseButtonPress, point);
            send(view, QEvent::MouseButtonRelease, point, Qt::NoButton);
        };
        // Turned on for the first time, it points three radii to the right.
        click(HandleType::NUMBER_ARROW);
        QCOMPARE(number->arrow_tip(), std::optional<QPointF>(QPointF(60, 0)));
        QCOMPARE(handle_of(*editor, HandleType::NUMBER_ARROW_TIP)->position, QPointF(260, 200));
        const int before_drag = scene->undo_stack->count();
        gesture(view, QPointF(260, 200), QPointF(200, 300));
        QCOMPARE(number->arrow_tip(), std::optional<QPointF>(QPointF(0, 100)));
        QCOMPARE(number->pos(), QPointF(200, 200));
        QCOMPARE(scene->undo_stack->count(), before_drag + 1);
        // Off and on again, it comes back where it was.
        click(HandleType::NUMBER_ARROW);
        QVERIFY(!number->arrow_tip());
        QVERIFY(!handle_of(*editor, HandleType::NUMBER_ARROW_TIP));
        click(HandleType::NUMBER_ARROW);
        QCOMPARE(number->arrow_tip(), std::optional<QPointF>(QPointF(0, 100)));
        scene->undo_stack->undo();
        QVERIFY(!number->arrow_tip());
        scene->undo_stack->undo();
        QCOMPARE(number->arrow_tip(), std::optional<QPointF>(QPointF(0, 100)));
        scene->undo_stack->undo();
        QCOMPARE(number->arrow_tip(), std::optional<QPointF>(QPointF(60, 0)));
        // Dragged back into the circle, the pointer goes.
        gesture(view, QPointF(260, 200), QPointF(205, 200));
        QVERIFY(!number->arrow_tip());
        scene->undo_stack->undo();
        QCOMPARE(number->arrow_tip(), std::optional<QPointF>(QPointF(60, 0)));
        // A short tail ending under the X button: its tip is still the one grabbed there.
        number->set_arrow_tip(QPointF(20, -20));
        editor->refresh_handles();
        QCOMPARE(editor->hit_test(QPointF(220, 180))->handle_type, HandleType::NUMBER_ARROW_TIP);
        QCOMPARE(editor->hit_test(QPointF(226, 186))->handle_type, HandleType::NUMBER_DELETE);
    }

    void numberLabelIsTypedBesideItAndMovesWithIt() {
        auto [scene, view] = canvas("number");
        auto *number = new NumberItem(1, QPointF(200, 200), 20.0, RED);
        scene->addItem(number);
        scene->undo_stack->push(new AddItemCommand(scene, ItemReference(number)));
        view->smart_edit_controller->select_item(number);
        auto *editor = view->smart_edit_controller->layer_editor.get();
        const auto button = handle_point(view, *handle_of(*editor, HandleType::NUMBER_LABEL));
        send(view, QEvent::MouseButtonPress, button);
        send(view, QEvent::MouseButtonRelease, button, Qt::NoButton);
        auto *label = number->label();
        QVERIFY(label && label->is_editing());
        QCOMPARE(view->get_active_text_item(), label);
        // Just right of the circle, 0.3 radii away, centred on it.
        QCOMPARE(label->anchor(), QPointF(26, 0));
        for (const QChar ch : QString("note")) {
            QKeyEvent typed(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QString(ch));
            QApplication::sendEvent(view->viewport(), &typed);
        }
        QCOMPARE(label->toPlainText(), QString("note"));
        QCOMPARE(label->pos().y(), -label->boundingRect().height() / 2);
        // A click elsewhere ends typing in one undo step; the number stays selected.
        const int typed_at = scene->undo_stack->count();
        send(view, QEvent::MouseButtonPress, QPointF(60, 60));
        send(view, QEvent::MouseButtonRelease, QPointF(60, 60), Qt::NoButton);
        QVERIFY(!label->is_editing());
        QCOMPARE(scene->undo_stack->count(), typed_at + 1);
        QCOMPARE(view->smart_edit_controller->selected_item(), number);
        // Moving the number takes the label along.
        const auto label_at = label->scenePos();
        gesture(view, QPointF(200, 200), QPointF(230, 200));
        QCOMPARE(number->pos(), QPointF(230, 200));
        QCOMPARE(label->scenePos(), label_at + QPointF(30, 0));
        // Dragging the label moves it alone.
        const auto grab = label->sceneBoundingRect().center().toPoint();
        gesture(view, grab, grab + QPoint(0, 40));
        QCOMPARE(number->pos(), QPointF(230, 200));
        QCOMPARE(label->anchor(), QPointF(26, 40));
        QVERIFY(!label->is_editing());
        scene->undo_stack->undo();
        QCOMPARE(label->anchor(), QPointF(26, 0));
        scene->undo_stack->undo();
        QCOMPARE(number->pos(), QPointF(200, 200));
        scene->undo_stack->undo();
        QVERIFY(!label->isVisible());
        QCOMPARE(number->label_text(), QString());
        scene->undo_stack->redo();
        QCOMPARE(number->label_text(), QString("note"));
        // A click on the label without a drag types in it again.
        const auto again = label->sceneBoundingRect().center().toPoint();
        send(view, QEvent::MouseButtonPress, again);
        send(view, QEvent::MouseButtonRelease, again, Qt::NoButton);
        QVERIFY(label->is_editing());
    }

    void wheelWhileTypingTheLabelScalesNumberAndLabelTogether() {
        auto [scene, view] = canvas("number");
        auto *number = new NumberItem(1, QPointF(200, 200), 20.0, RED);
        scene->addItem(number);
        scene->undo_stack->push(new AddItemCommand(scene, ItemReference(number)));
        view->smart_edit_controller->select_item(number);
        auto *label = number->begin_label_edit();
        QCOMPARE(view->get_active_text_item(), label);
        // The wheel steps the tool width by one and scales the selection by the same ratio.
        const double width = scene->tool_controller->ctx.stroke_width;
        wheel(view, 120);
        const double factor = (width + 1) / width;
        QCOMPARE(number->radius, 20 * factor);
        QCOMPARE(label->font().pixelSize(), qRound(number->radius * NumberItem::LABEL_SIZE_LEVELS[NumberItem::DEFAULT_LABEL_SIZE_LEVEL]));
        QCOMPARE(label->anchor(), QPointF(26, 0) * factor);
        QVERIFY(label->is_editing());
    }

    void deletingANumberWhileTypingInItsLabelKeepsTheText() {
        auto [scene, view] = canvas("number");
        auto *number = new NumberItem(1, QPointF(200, 200), 20.0, RED);
        scene->addItem(number);
        scene->undo_stack->push(new AddItemCommand(scene, ItemReference(number)));
        view->smart_edit_controller->select_item(number);
        auto *editor = view->smart_edit_controller->layer_editor.get();
        number->begin_label_edit();
        QKeyEvent typed(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, "x");
        QApplication::sendEvent(view->viewport(), &typed);
        const auto remove = handle_point(view, *handle_of(*editor, HandleType::NUMBER_DELETE));
        send(view, QEvent::MouseButtonPress, remove);
        send(view, QEvent::MouseButtonRelease, remove, Qt::NoButton);
        QVERIFY(!number->scene());
        scene->undo_stack->undo();
        QCOMPARE(number->scene(), scene);
        QCOMPARE(number->label_text(), QString("x"));
        scene->undo_stack->undo();
        QCOMPARE(number->label_text(), QString());
    }

    void labelAndPointerStartOutOfEachOthersWay() {
        NumberItem plain(1, QPointF(0, 0), 20.0, RED);
        plain.set_label("a");
        QCOMPARE(plain.label()->anchor(), QPointF(26, 0));
        // A pointer turned on next to a label on the right goes left.
        plain.toggle_arrow();
        QCOMPARE(plain.arrow_tip(), std::optional<QPointF>(QPointF(-60, 0)));
        // A label added beside a pointer that goes right sits below or above its line.
        NumberItem right(1, QPointF(0, 0), 20.0, RED);
        right.toggle_arrow();
        QCOMPARE(right.arrow_tip(), std::optional<QPointF>(QPointF(60, 0)));
        right.set_label("b");
        QCOMPARE(right.label()->anchor(), QPointF(26, 30));
        NumberItem down(1, QPointF(0, 0), 20.0, RED);
        down.set_arrow_tip(QPointF(50, 50));
        down.set_label("c");
        QCOMPARE(down.label()->anchor(), QPointF(26, -30));
    }

    // A number with a label and guide line is one annotation wherever the scene is searched by point or area.
    NumberItem *put_labelled_number(CanvasScene *scene, const QPointF &at, const QPointF &label_anchor) {
        auto *number = new NumberItem(1, at, 20.0, RED);
        scene->addItem(number);
        scene->undo_stack->push(new AddItemCommand(scene, ItemReference(number)));
        number->apply_extras_state({{"label_guide", true}});
        number->set_label("note", label_anchor);
        return number;
    }

    void partsOfANumberBelongToIt() {
        auto [scene, view] = canvas("number");
        Q_UNUSED(view);
        auto *number = put_labelled_number(scene, QPointF(200, 200), QPointF(26, 0));
        QVERIFY(number->guide() && number->guide()->isVisible());
        QCOMPARE(annotation_owner(number), number);
        QCOMPARE(annotation_owner(number->label()), number);
        QCOMPARE(annotation_owner(number->guide()), number);
        QCOMPARE(annotation_owner(nullptr), nullptr);
    }

    void areaLookupsTakeANumberOnceWithItsParts() {
        auto [scene, view] = canvas("number");
        Q_UNUSED(view);
        // Circle, label and guide all inside: the number once, none of its parts on their own.
        auto *inside = put_labelled_number(scene, QPointF(200, 200), QPointF(26, 0));
        auto found = scene->get_drawing_items_in_rect(QRectF(100, 100, 300, 200));
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.first(), inside);
        // Only the label reaches into the area: the number still comes along.
        auto *outside = put_labelled_number(scene, QPointF(200, 520), QPointF(-20, -200));
        const auto label_area = outside->label()->sceneBoundingRect();
        QVERIFY(!outside->sceneBoundingRect().intersects(label_area));
        found = scene->get_drawing_items_in_rect(label_area);
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.first(), outside);
    }

    void clickingANumbersGuideLineSelectsTheNumber() {
        auto [scene, view] = canvas("number");
        // The label well away, so the line runs clear of the circle's click margin.
        auto *number = put_labelled_number(scene, QPointF(200, 200), QPointF(60, 0));
        auto *controller = view->smart_edit_controller.get();
        controller->clear_selection(true);
        // Halfway along the stretch from the circle to the label, clear of both.
        const auto path = number->guide()->path();
        const auto on_line = number->mapToScene((QPointF(path.elementAt(0)) + QPointF(path.elementAt(1))) / 2);
        QVERIFY(!number->contains(number->mapFromScene(on_line)));
        QVERIFY(!number->label()->contains(number->label()->mapFromScene(on_line)));
        QVERIFY(scene->items(on_line).contains(number->guide()));
        QVERIFY(controller->handle_press(QPointF(), on_line, Qt::LeftButton, Qt::NoModifier));
        QCOMPARE(controller->selected_item(), number);
    }

    void erasingANumbersLabelErasesTheNumber() {
        auto [scene, view] = canvas("eraser");
        auto *number = put_labelled_number(scene, QPointF(200, 200), QPointF(26, 0));
        const int before = scene->undo_stack->count();
        const auto label_middle = number->label()->sceneBoundingRect().center();
        gesture(view, label_middle, label_middle + QPointF(4, 0));
        QVERIFY(!number->scene());
        QCOMPARE(scene->undo_stack->count(), before + 1);
        scene->undo_stack->undo();
        QCOMPARE(number->scene(), scene);
        QCOMPARE(number->label_text(), QString("note"));
    }

    void aNumberCopiesItsPointerAndLabel() {
        NumberItem source(2, QPointF(0, 0), 20.0, RED);
        source.set_arrow_tip(QPointF(0, 70));
        source.set_label("here", QPointF(30, -5));
        NumberItem copy(2, QPointF(50, 50), 20.0, RED);
        copy.copy_extras_from(source);
        QCOMPARE(copy.arrow_tip(), std::optional<QPointF>(QPointF(0, 70)));
        QCOMPARE(copy.label_text(), QString("here"));
        QCOMPARE(copy.label()->anchor(), QPointF(30, -5));
        QCOMPARE(copy.label()->parentItem(), &copy);
    }

    void hoveringAHandleChangesTheCursor() {
        auto [scene, view] = canvas("rect");
        auto *item = put_rect(scene);
        view->smart_edit_controller->select_item(item);
        // Selecting queues a cursor refresh; real input arrives after the event loop ran it.
        QApplication::processEvents();
        auto *editor = view->smart_edit_controller->layer_editor.get();
        const auto corner = *handle_of(*editor, HandleType::CORNER_BR);
        send(view, QEvent::MouseMove, handle_point(view, corner), Qt::NoButton);
        QVERIFY(editor->hovered_handle() && editor->hovered_handle()->id == corner.id);
        QCOMPARE(view->viewport()->cursor().shape(), corner.cursor.shape());
        // Over the outline between handles the item shows that it can be moved.
        send(view, QEvent::MouseMove, QPointF(95, 60), Qt::NoButton);
        QVERIFY(!editor->hovered_handle());
        QCOMPARE(view->viewport()->cursor().shape(), Qt::SizeAllCursor);
        // Rendering draws the handles in viewport coordinates.
        QImage image(W, H, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        QPainter painter(&image);
        editor->render(&painter);
        painter.end();
        // A white handle with the theme outline: its square holds outline pixels.
        const auto square = editor->screen_rect(corner).toAlignedRect();
        int outline = 0;
        for (int x = square.left(); x <= square.right(); ++x)
            for (int y = square.top(); y <= square.bottom(); ++y)
                if (image.pixelColor(x, y) == LayerEditor::HANDLE_COLOR) ++outline;
        QVERIFY(outline > 10);
        QVERIFY(editor->screen_bounds().contains(square));
    }

    // ---- Which items a click selects ----

    void ctrlSelectsAnyItemOnlyWhenTheHostAllowsIt() {
        QGraphicsScene scene;
        SmartEditController controller(&scene);
        controller.current_tool_id = "text";
        auto *ellipse = new EllipseItem(QRectF(0, 0, 20, 20), QPen(RED, 4));
        auto *text = new_text();
        auto *stroke = new_stroke();
        for (auto *item : std::initializer_list<QGraphicsItem *>{ellipse, text, stroke}) scene.addItem(item);
        QVERIFY(!controller.can_select_item(ellipse, Qt::NoModifier));
        QVERIFY(!controller.can_select_item(ellipse, Qt::ControlModifier));
        // Without the capability Ctrl still picks strokes, as it always did.
        QVERIFY(controller.can_select_item(stroke, Qt::ControlModifier));
        controller.cross_tool_select_enabled = true;
        for (const char *tool : {"text", "cursor", "eraser", "mosaic"}) {
            controller.current_tool_id = tool;
            QVERIFY2(controller.can_select_item(ellipse, Qt::ControlModifier), tool);
            QVERIFY2(controller.can_select_item(text, Qt::ControlModifier), tool);
            QVERIFY2(controller.can_select_item(stroke, Qt::ControlModifier), tool);
        }
        controller.current_tool_id = "rect";
        controller.select_item(ellipse);
        QCOMPARE(controller.get_item_tool_id(ellipse), QString("ellipse"));
        QVERIFY(controller.is_cross_tool_selection());
        controller.clear_selection();
        QVERIFY(!controller.selected_item());
    }

    void aPlainClickSkipsTopTextButCtrlTakesIt() {
        QGraphicsScene scene;
        auto *rect = new RectItem(QRectF(0, 0, 80, 40), QPen(RED, 6));
        auto *text = new_text("overlap");
        text->setZValue(30);
        scene.addItem(rect);
        scene.addItem(text);
        SmartEditController controller(&scene);
        controller.current_tool_id = "rect";
        QVERIFY(controller.handle_press(QPointF(2, 2), QPointF(2, 2), Qt::LeftButton, Qt::NoModifier));
        QCOMPARE(controller.selected_item(), rect);
        // The view must drag the lower item itself: Qt would give the gesture to the text.
        QVERIFY(controller.press_requires_manual_dispatch);
        controller.clear_selection();
        controller.cross_tool_select_enabled = true;
        QVERIFY(controller.handle_press(QPointF(2, 2), QPointF(2, 2), Qt::LeftButton, Qt::ControlModifier));
        QCOMPARE(controller.selected_item(), text);
    }

    void theViewDragsTheItemBelowTheTopText() {
        auto [scene, view] = canvas("rect");
        auto *rect = put_rect(scene);
        auto *text = new TextItem("cover", QPointF(55, 95), text_font(), QColor("black"));
        scene->addItem(text);
        QVERIFY(text->sceneBoundingRect().contains(QPointF(60, 105)));
        const int before = scene->undo_stack->count();
        gesture(view, QPointF(60, 105), QPointF(100, 135), 4);
        QCOMPARE(view->smart_edit_controller->selected_item(), rect);
        QCOMPARE(rect->scenePos(), QPointF(40, 30));
        QCOMPARE(text->pos(), QPointF(55, 95));
        QCOMPARE(scene->undo_stack->count() - before, 1);
    }

    void crossToolStyleIsTristateAndKeepsAlpha() {
        auto [scene, view] = canvas("rect", true);
        auto *highlighter = new_stroke(15, true);
        scene->addItem(highlighter);
        view->smart_edit_controller->select_item(highlighter);
        // A highlighter's logical width is a third of its pen.
        QCOMPARE(view->apply_cross_tool_selection_style(std::nullopt, 7.0), std::optional<bool>(true));
        QVERIFY(std::abs(*highlighter->get_stroke_width() - 7) < 1e-6);
        QVERIFY(std::abs(highlighter->pen().widthF() - 21) < 1e-6);
        view->smart_edit_controller->clear_selection();
        QCOMPARE(view->apply_cross_tool_selection_style(std::nullopt, 9.0), std::optional<bool>());
        auto *text = new_text();
        scene->addItem(text);
        view->smart_edit_controller->select_item(text);
        QCOMPARE(view->apply_cross_tool_selection_style(std::nullopt, 9.0), std::optional<bool>(false));

        scene->tool_controller->activate("text");
        QColor faint("red");
        faint.setAlpha(91);
        auto *rect = new RectItem(QRectF(0, 0, 40, 20), QPen(faint, 4));
        scene->addItem(rect);
        view->smart_edit_controller->select_item(rect);
        QCOMPARE(view->apply_cross_tool_selection_style(QColor("blue")), std::optional<bool>(true));
        QCOMPARE(rect->pen().color().name(), QColor("blue").name());
        QCOMPARE(rect->pen().color().alpha(), 91);
        QCOMPARE(view->apply_cross_tool_selection_style(std::nullopt, std::nullopt, 0.35), std::optional<bool>(true));
        QVERIFY(std::abs(*rect->get_visual_opacity() - 0.35) < 0.01);
        // Every other kind takes the new colour and keeps its own alpha.
        QColor half("green");
        half.setAlpha(128);
        auto *stroke = new StrokeItem(QPainterPath(QPointF(0, 0)), QPen(half, 3));
        auto *ellipse = new EllipseItem(QRectF(0, 0, 30, 20), QPen(half, 3));
        auto *arrow = new ArrowItem(QPointF(0, 0), QPointF(40, 30), QPen(half, 3));
        auto *number = new NumberItem(1, QPointF(50, 50), 12, half);
        for (QGraphicsItem *item : std::initializer_list<QGraphicsItem *>{stroke, ellipse, arrow, number}) {
            scene->addItem(item);
            view->smart_edit_controller->select_item(item);
            QCOMPARE(view->apply_cross_tool_selection_style(QColor("purple")), std::optional<bool>(true));
        }
        QCOMPARE(stroke->pen().color().name(), QColor("purple").name());
        QCOMPARE(stroke->pen().color().alpha(), 128);
        QCOMPARE(ellipse->pen().color().name(), QColor("purple").name());
        QCOMPARE(arrow->color.name(), QColor("purple").name());
        QCOMPARE(arrow->color.alpha(), 128);
        QCOMPARE(number->color.name(), QColor("purple").name());
        // A highlighter rectangle's fill follows too.
        rect->setBrush(QColor(255, 0, 0, 60));
        rect->is_highlighter = true;
        rect->is_highlighter_rect = true;
        view->smart_edit_controller->select_item(rect);
        view->apply_cross_tool_selection_style(QColor("orange"));
        QCOMPARE(rect->brush().color().name(), QColor("orange").name());
        QCOMPARE(rect->brush().color().alpha(), 60);
        // A watermark has no colour to take.
        auto *watermark = new WatermarkItem("w", WatermarkStyle{});
        scene->addItem(watermark);
        view->smart_edit_controller->select_item(watermark);
        Q_UNUSED(view->apply_cross_tool_selection_style(QColor("orange")));
    }

    void crossToolSelectionIsHostScoped() {
        auto [plain_scene, plain] = canvas();
        auto [cross_scene, cross] = canvas("cursor", true);
        Q_UNUSED(plain_scene);
        Q_UNUSED(cross_scene);
        QVERIFY(!plain->smart_edit_controller->cross_tool_select_enabled);
        QVERIFY(cross->smart_edit_controller->cross_tool_select_enabled);
    }

    // ---- The wheel ----

    void wheelChangesWidthOpacityNumberAndTextSize() {
        auto [scene, view] = canvas("rect");
        auto &ctx = scene->tool_controller->ctx;
        const double width = ctx.stroke_width;
        wheel(view, 120);
        QCOMPARE(ctx.stroke_width, width + 1);
        wheel(view, -120);
        QCOMPARE(ctx.stroke_width, width);
        // A selected item follows the width.
        auto *item = put_rect(scene);
        view->smart_edit_controller->select_item(item);
        const double item_width = *item->get_stroke_width();
        wheel(view, 120);
        QVERIFY(*item->get_stroke_width() > item_width);
        // Ctrl adjusts opacity in 5% steps.
        const double opacity = ctx.opacity;
        wheel(view, -120, Qt::ControlModifier);
        QVERIFY(std::abs(ctx.opacity - std::max(0.0, opacity - 0.05)) < 1e-6);
        QVERIFY(std::abs(*item->get_visual_opacity() - ctx.opacity) < 0.01);

        scene->tool_controller->activate("number");
        view->smart_edit_controller->clear_selection();
        gesture(view, QPointF(300, 300), QPointF(300, 300), 1);
        const auto numbers = items_of<NumberItem>(scene);
        QCOMPARE(numbers.size(), 1);
        QCOMPARE(numbers.first()->number, 1);
        // Shift moves the next number.
        wheel(view, 120, Qt::ShiftModifier);
        wheel(view, 120, Qt::ShiftModifier);
        gesture(view, QPointF(350, 300), QPointF(350, 300), 1);
        int highest = 0;
        for (auto *number : items_of<NumberItem>(scene)) highest = std::max(highest, number->number);
        QCOMPARE(highest, 4);

        scene->tool_controller->activate("text");
        auto *text = write_text(scene, view, "size");
        QVERIFY(text);
        const double size = text->font_point_size();
        wheel(view, 120);
        QCOMPARE(text->font_point_size(), size + 2);
    }

    // While text is being edited the view hands typing and composition to it; otherwise keys go up to the window.
    void typingAndCompositionGoToTheEditedText() {
        auto [scene, view] = canvas("text");
        view->show();
        view->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(view));
        auto *item = write_text(scene, view, "ab");
        QVERIFY(item && item->is_editing());
        auto cursor = item->textCursor();
        cursor.movePosition(QTextCursor::End);
        item->setTextCursor(cursor);
        QKeyEvent typed(QEvent::KeyPress, Qt::Key_C, Qt::NoModifier, "c");
        QApplication::sendEvent(view->viewport(), &typed);
        QCOMPARE(item->toPlainText(), QString("abc"));
        QInputMethodEvent composed;
        composed.setCommitString("d");
        QApplication::sendEvent(view, &composed);
        QCOMPARE(item->toPlainText(), QString("abcd"));
        view->end_text_edit(item);
        QKeyEvent loose(QEvent::KeyPress, Qt::Key_E, Qt::NoModifier, "e");
        QApplication::sendEvent(view->viewport(), &loose);
        QVERIFY(!loose.isAccepted());
        QCOMPARE(item->toPlainText(), QString("abcd"));
    }

    // A click on another text while editing ends this edit and takes the other one in the same click.
    void clickingAnotherTextSwitchesTheEdit() {
        auto [scene, view] = canvas("text");
        auto *first = write_text(scene, view, "first");
        QVERIFY(first);
        view->end_text_edit(first);
        gesture(view, QPointF(300, 220), QPointF(300, 220), 1);
        TextItem *second = nullptr;
        for (auto *text : items_of<TextItem>(scene))
            if (text != first) second = text;
        QVERIFY(second && second->is_editing());
        second->setPlainText("second");
        QApplication::processEvents();
        const auto target = first->mapToScene(first->boundingRect().center());
        gesture(view, target, target, 1);
        QApplication::processEvents();
        QVERIFY(!second->is_editing());
        QCOMPARE(view->smart_edit_controller->selected_item(), static_cast<QGraphicsItem *>(first));
        // A click on empty canvas only ends the edit; it creates no new text.
        const int texts = items_of<TextItem>(scene).size();
        gesture(view, QPointF(450, 340), QPointF(450, 340), 1);
        QCOMPARE(items_of<TextItem>(scene).size(), texts);
    }

    // A text the tool started and that was left empty is destroyed, not kept outside the scene.
    void anAbandonedEmptyTextIsDestroyed() {
        auto [scene, view] = canvas("text");
        const int steps = scene->undo_stack->count();
        gesture(view, QPointF(120, 120), QPointF(120, 120), 1);
        const auto texts = items_of<TextItem>(scene);
        QCOMPARE(texts.size(), 1);
        QPointer<TextItem> item = texts.first();
        view->end_text_edit(item);
        QVERIFY(items_of<TextItem>(scene).isEmpty());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!item);
        QCOMPARE(scene->undo_stack->count(), steps);
    }

    // In rectangle mode the highlighter fills a translucent box; a tiny one is dropped.
    void theHighlighterDrawsBoxesInRectangleMode() {
        config_->update_settings("highlighter", {{"draw_mode", "rect"}});
        auto [scene, view] = canvas("highlighter");
        gesture(view, QPointF(40, 40), QPointF(140, 90));
        const auto boxes = [scene] {
            QList<RectItem *> found;
            for (auto *rect : items_of<RectItem>(scene))
                if (rect->is_highlighter_rect) found << rect;
            return found;
        };
        QCOMPARE(boxes().size(), 1);
        QCOMPARE(boxes().first()->rect(), QRectF(40, 40, 100, 50));
        QVERIFY(boxes().first()->brush().style() != Qt::NoBrush);
        gesture(view, QPointF(200, 200), QPointF(201, 201));
        QCOMPARE(boxes().size(), 1);
        scene->undo_stack->undo();
        QCOMPARE(boxes().size(), 0);
        config_->update_settings("highlighter", {{"draw_mode", "freehand"}});
    }
};

QTEST_MAIN(CanvasViewTests)
#include "test_canvas_view.moc"
