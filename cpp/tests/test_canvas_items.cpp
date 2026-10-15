#include "canvas/items/drawing_items.h"
#include "canvas/items/mosaic_item.h"
#include "canvas/items/selection_item.h"
#include "canvas/items/spotlight_item.h"
#include "canvas/items/watermark_item.h"
#include "canvas/scene.h"
#include "canvas/selection_model.h"
#include "canvas/smart_edit_controller.h"
#include "canvas/undo.h"
#include "canvas/view.h"
#include "core/export.h"
#include "core/theme.h"
#include "settings/tool_settings.h"
#include "tools/controller.h"
#include "tools/mosaic.h"
#include "tools/watermark.h"
#include "ui/watermark_input_dialog.h"
#include <QApplication>
#include <QDir>
#include <QFontDatabase>
#include <QGraphicsSceneHoverEvent>
#include <QGraphicsSceneMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <cmath>
#include <set>
#include <typeinfo>

using namespace jietuba;

namespace {
const QColor WHITE(255, 255, 255);
const QColor GRAY(128, 128, 128);
const QRectF SEL(120, 90, 160, 120);
const QDateTime NOW(QDate(2026, 10, 11), QTime(9, 5, 7));

WatermarkStyle ink_style() {
    WatermarkStyle style;
    style.color = "#000000";
    style.font_size = 16;
    style.opacity = 1.0;
    return style;
}

QImage blank(int w = 320, int h = 200, const QColor &color = WHITE) {
    QImage image(w, h, QImage::Format_ARGB32);
    image.fill(color);
    return image;
}

int inked(const QImage &image, const QRect &rect = QRect()) {
    const auto area = rect.isNull() ? image.rect() : rect;
    int count = 0;
    for (int x = area.left(); x <= area.right(); ++x)
        for (int y = area.top(); y <= area.bottom(); ++y)
            if (image.pixelColor(x, y) != WHITE) ++count;
    return count;
}

QList<int> inked_rows(const QImage &image) {
    QList<int> rows;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (image.pixelColor(x, y) != WHITE) {
                rows.append(y);
                break;
            }
    return rows;
}

QImage paint(const QRectF &rect, const WatermarkStyle &style, const QString &text = "WATERMARK", const QRectF &exposed = QRectF(),
             const std::optional<QRectF> &clip = std::nullopt, QImage image = blank()) {
    QPainter painter(&image);
    if (clip) painter.setClipRect(*clip);
    paint_watermark(&painter, rect, text, style, exposed);
    painter.end();
    return image;
}

bool color_near(const QColor &actual, const QColor &expected, int tolerance = 2) {
    return std::abs(actual.red() - expected.red()) <= tolerance && std::abs(actual.green() - expected.green()) <= tolerance
        && std::abs(actual.blue() - expected.blue()) <= tolerance;
}

QColor dimmed(const QColor &color, double darkness = 0.5) {
    return QColor(static_cast<int>(std::lround(color.red() * (1 - darkness))), static_cast<int>(std::lround(color.green() * (1 - darkness))),
                  static_cast<int>(std::lround(color.blue() * (1 - darkness))));
}

QImage gradient(int w = 64, int h = 64) {
    QImage image(w, h, QImage::Format_ARGB32);
    for (int x = 0; x < w; ++x)
        for (int y = 0; y < h; ++y) image.setPixelColor(x, y, QColor(x * 255 / std::max(1, w - 1), y * 255 / std::max(1, h - 1), 128));
    return image;
}

template <typename T>
QList<T *> items_of(QGraphicsScene *scene) {
    QList<T *> found;
    for (auto *item : scene->items())
        if (auto *typed = dynamic_cast<T *>(item)) found.append(typed);
    return found;
}

void draw(CanvasScene *scene, const QString &tool, const QPointF &start, const QPointF &end) {
    scene->activate_tool(tool);
    auto *controller = scene->tool_controller.get();
    controller->on_press(start, Qt::LeftButton);
    controller->on_move((start + end) / 2);
    controller->on_move(end);
    controller->on_release(end);
}

// Handle centres sit on the selection border: corners and edge midpoints.
QMap<int, QPointF> handle_positions(const QRectF &rect) {
    return {{SelectionItem::HANDLE_TOP_LEFT, rect.topLeft()}, {SelectionItem::HANDLE_TOP, QPointF(rect.center().x(), rect.top())},
            {SelectionItem::HANDLE_TOP_RIGHT, rect.topRight()}, {SelectionItem::HANDLE_LEFT, QPointF(rect.left(), rect.center().y())},
            {SelectionItem::HANDLE_RIGHT, QPointF(rect.right(), rect.center().y())}, {SelectionItem::HANDLE_BOTTOM_LEFT, rect.bottomLeft()},
            {SelectionItem::HANDLE_BOTTOM, QPointF(rect.center().x(), rect.bottom())}, {SelectionItem::HANDLE_BOTTOM_RIGHT, rect.bottomRight()}};
}

const QMap<int, QPoint> OUTWARD = {{SelectionItem::HANDLE_TOP_LEFT, {-1, -1}}, {SelectionItem::HANDLE_TOP, {0, -1}}, {SelectionItem::HANDLE_TOP_RIGHT, {1, -1}},
                                   {SelectionItem::HANDLE_LEFT, {-1, 0}}, {SelectionItem::HANDLE_RIGHT, {1, 0}}, {SelectionItem::HANDLE_BOTTOM_LEFT, {-1, 1}},
                                   {SelectionItem::HANDLE_BOTTOM, {0, 1}}, {SelectionItem::HANDLE_BOTTOM_RIGHT, {1, 1}}};

Qt::CursorShape cursor_for(int handle) {
    switch (handle) {
    case SelectionItem::HANDLE_TOP_LEFT: case SelectionItem::HANDLE_BOTTOM_RIGHT: return Qt::SizeFDiagCursor;
    case SelectionItem::HANDLE_TOP_RIGHT: case SelectionItem::HANDLE_BOTTOM_LEFT: return Qt::SizeBDiagCursor;
    case SelectionItem::HANDLE_TOP: case SelectionItem::HANDLE_BOTTOM: return Qt::SizeVerCursor;
    case SelectionItem::HANDLE_LEFT: case SelectionItem::HANDLE_RIGHT: return Qt::SizeHorCursor;
    default: return Qt::SizeAllCursor;
    }
}

bool is_theme_color(const QImage &image, double x, double y) { return image.pixelColor(int(x), int(y)).name() == get_theme().theme_color().name(); }

int white_pixels_around(const QImage &image, const QPointF &pos, int box = 10) {
    int count = 0;
    for (int dx = -box; dx <= box; ++dx)
        for (int dy = -box; dy <= box; ++dy) {
            const int x = int(pos.x()) + dx, y = int(pos.y()) + dy;
            if (image.rect().contains(x, y) && image.pixelColor(x, y) == WHITE) ++count;
        }
    return count;
}
}

class CanvasItemTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir dir_;
    QSettings *settings_ = nullptr;
    ToolSettingsManager *config_ = nullptr;
    std::vector<std::unique_ptr<CanvasScene>> scenes_;
    std::unique_ptr<SelectionModel> model_;
    std::unique_ptr<QGraphicsScene> selection_scene_;
    SelectionItem *selection_ = nullptr;

    CanvasScene *scene(const QRectF &selection = QRectF(40, 20, 240, 160), const QImage &image = blank()) {
        auto *created = scenes_.emplace_back(std::make_unique<CanvasScene>(image, QRectF(QPointF(0, 0), image.size()), true)).get();
        created->selection_model->initialize_confirmed_rect(selection);
        return created;
    }

    QImage render_selection() {
        QImage image(400, 300, QImage::Format_ARGB32);
        image.fill(0xFF000000);
        QPainter painter(&image);
        selection_->render(&painter);
        painter.end();
        return image;
    }

    std::set<int> drawn_handles() {
        get_theme().set_selection_border_width(1);
        get_theme().set_selection_handle_size(ThemeManager::HANDLE_SIZE_MEDIUM);
        const auto image = render_selection();
        std::set<int> drawn;
        const auto positions = handle_positions(SEL);
        for (auto it = positions.begin(); it != positions.end(); ++it)
            if (is_theme_color(image, it.value().x() + OUTWARD[it.key()].x() * 4, it.value().y() + OUTWARD[it.key()].y() * 4)) drawn.insert(it.key());
        return drawn;
    }

    void set_selection(QRectF rect) { model_->set_rect(rect); }

    void hover(const QPointF &pos) {
        QGraphicsSceneHoverEvent event(QEvent::GraphicsSceneHoverMove);
        event.setScenePos(pos);
        event.setPos(selection_->mapFromScene(pos));
        selection_scene_->sendEvent(selection_, &event);
    }

    void mouse(QEvent::Type type, const QPointF &pos) {
        QGraphicsSceneMouseEvent event(type);
        event.setButton(type == QEvent::GraphicsSceneMouseMove ? Qt::NoButton : Qt::LeftButton);
        event.setButtons(type == QEvent::GraphicsSceneMouseRelease ? Qt::NoButton : Qt::LeftButton);
        event.setScenePos(pos);
        event.setPos(selection_->mapFromScene(pos));
        event.setAccepted(false);
        selection_scene_->sendEvent(selection_, &event);
    }

private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        settings_ = new QSettings(dir_.filePath("settings.ini"), QSettings::IniFormat, this);
        config_ = get_tool_settings_manager(settings_);
        // The offscreen platform has no font directory; watermarks need a real font.
        QVERIFY(QFontDatabase::addApplicationFont(QDir(qEnvironmentVariable("WINDIR")).filePath("Fonts/arial.ttf")) >= 0);
    }

    void init() {
        settings_->clear();
        get_theme().reset_to_defaults();
        config_->update_settings("watermark", ink_style().to_values());
        config_->set_screenshot_watermark_text("WATERMARK");
        model_ = std::make_unique<SelectionModel>();
        model_->activate();
        set_selection(SEL);
        model_->confirm();
        selection_scene_ = std::make_unique<QGraphicsScene>(0, 0, 400, 300);
        selection_ = new SelectionItem(model_.get());
        selection_scene_->addItem(selection_);
    }

    void cleanup() {
        selection_scene_.reset();
        model_.reset();
        scenes_.clear();
        get_theme().reset_to_defaults();
    }

    // ---- Selection appearance ----

    void borderWidthFollowsTheSetting_data() {
        QTest::addColumn<int>("width");
        for (int width : {1, 4, 10}) QTest::addRow("%d", width) << width;
    }

    void borderWidthFollowsTheSetting() {
        QFETCH(int, width);
        get_theme().set_selection_border_width(width);
        QCOMPARE(selection_->border_width(), width);
        const auto image = render_selection();
        const int y = int(SEL.top()) + 20;
        const int left = int(SEL.left());
        QList<int> run;
        for (int x = left - width; x < left + width; ++x)
            if (is_theme_color(image, x, y)) run.append(x);
        QCOMPARE(run.size(), width);
        QCOMPARE(run.first(), left - width / 2);
    }

    void appearanceSettingsAreClampedOrFallBack() {
        auto &theme = get_theme();
        theme.set_selection_border_width(999);
        QCOMPARE(theme.selection_border_width(), ThemeManager::BORDER_WIDTH_OPTIONS.back());
        theme.set_selection_border_width(0);
        QCOMPARE(theme.selection_border_width(), ThemeManager::BORDER_WIDTH_OPTIONS.front());
        theme.set_selection_border_width(QString("not a number"));
        QCOMPARE(theme.selection_border_width(), ThemeManager::DEFAULT_BORDER_WIDTH);
        theme.set_selection_handle_size("huge");
        QCOMPARE(theme.selection_handle_size(), ThemeManager::DEFAULT_HANDLE_SIZE);
        theme.set_selection_handle_style("round");
        QCOMPARE(theme.selection_handle_style(), ThemeManager::HANDLES_ALL);
        // Diameter and ring grow together.
        int previous_diameter = 0, previous_ring = 0;
        for (const auto &size : ThemeManager::HANDLE_SIZE_OPTIONS) {
            theme.set_selection_handle_size(size);
            QVERIFY(theme.selection_handle_diameter() > previous_diameter);
            QVERIFY(theme.selection_handle_ring_width() >= previous_ring);
            QCOMPARE(selection_->handle_size(), theme.selection_handle_diameter());
            previous_diameter = theme.selection_handle_diameter();
            previous_ring = theme.selection_handle_ring_width();
        }
    }

    void largerHandlesDrawMoreWhite() {
        auto &theme = get_theme();
        theme.set_selection_border_width(1);
        theme.set_selection_handle_size(ThemeManager::HANDLE_SIZE_SMALL);
        const int small = white_pixels_around(render_selection(), SEL.topLeft());
        theme.set_selection_handle_size(ThemeManager::HANDLE_SIZE_LARGE);
        const int large = white_pixels_around(render_selection(), SEL.topLeft());
        QVERIFY(large > small);
    }

    void handleStylesDrawEightFourOrNone() {
        get_theme().set_selection_handle_style(ThemeManager::HANDLES_ALL);
        QCOMPARE(drawn_handles().size(), size_t(8));
        get_theme().set_selection_handle_style(ThemeManager::HANDLES_CORNERS);
        QCOMPARE(drawn_handles(), (std::set<int>{SelectionItem::HANDLE_TOP_LEFT, SelectionItem::HANDLE_TOP_RIGHT, SelectionItem::HANDLE_BOTTOM_LEFT,
                                                 SelectionItem::HANDLE_BOTTOM_RIGHT}));
        get_theme().set_selection_handle_style(ThemeManager::HANDLES_NONE);
        QVERIFY(drawn_handles().empty());
        // The rounded preview skips corners; with corners only nothing is left.
        get_theme().set_selection_handle_style(ThemeManager::HANDLES_CORNERS);
        QImage image(400, 300, QImage::Format_ARGB32);
        image.fill(0xFF000000);
        QPainter painter(&image);
        selection_->draw_handles(&painter, SEL, true);
        painter.end();
        const auto positions = handle_positions(SEL);
        for (auto it = positions.begin(); it != positions.end(); ++it)
            QVERIFY(!is_theme_color(image, it.value().x() + OUTWARD[it.key()].x() * 4, it.value().y() + OUTWARD[it.key()].y() * 4));
    }

    void crowdedSelectionsHideTheirHandles() {
        auto &theme = get_theme();
        theme.set_selection_handle_style(ThemeManager::HANDLES_ALL);
        theme.set_selection_handle_size(ThemeManager::HANDLE_SIZE_SMALL);
        theme.set_selection_border_width(1);
        const QRectF flat(SEL.left(), SEL.top(), 160, 24);
        set_selection(flat);
        QVERIFY(!selection_->handles_visible());
        const auto image = render_selection();
        for (const auto &pos : handle_positions(flat)) QCOMPARE(white_pixels_around(image, pos), 0);
        // The border is still drawn.
        QVERIFY(is_theme_color(image, flat.center().x(), flat.top()));
        // The threshold follows the handle size.
        set_selection(QRectF(SEL.left(), SEL.top(), 160, 36));
        QVERIFY(selection_->handles_visible());
        theme.set_selection_handle_size(ThemeManager::HANDLE_SIZE_LARGE);
        QVERIFY(!selection_->handles_visible());
        // Corners only leave one handle fewer per side, so they fit a smaller selection.
        theme.set_selection_handle_size(ThemeManager::HANDLE_SIZE_SMALL);
        set_selection(QRectF(SEL.left(), SEL.top(), 160, 20));
        QVERIFY(!selection_->handles_visible());
        theme.set_selection_handle_style(ThemeManager::HANDLES_CORNERS);
        QVERIFY(selection_->handles_visible());
        // Handles are hidden while dragging and before the selection is confirmed.
        set_selection(SEL);
        model_->start_dragging();
        QVERIFY(!selection_->handles_visible());
        model_->stop_dragging();
        QVERIFY(selection_->handles_visible());
    }

    void everyHandleStaysDraggableWhateverIsDrawn() {
        for (const auto &style : ThemeManager::HANDLE_STYLE_OPTIONS) {
            get_theme().set_selection_handle_style(style);
            for (const QRectF &rect : {SEL, QRectF(SEL.left(), SEL.top(), 160, 24)}) {
                set_selection(rect);
                const auto positions = handle_positions(rect);
                for (auto it = positions.begin(); it != positions.end(); ++it) {
                    hover(it.value());
                    QVERIFY2(selection_->cursor().shape() == cursor_for(it.key()), qPrintable(QString("%1 %2").arg(style).arg(it.key())));
                }
            }
        }
        set_selection(SEL);
        hover(SEL.center());
        QCOMPARE(selection_->cursor().shape(), Qt::SizeAllCursor);
        hover(QPointF(10, 10));
        QCOMPARE(selection_->cursor().shape(), Qt::ArrowCursor);
    }

    void draggingResizesOrMovesTheSelection() {
        QSignalSpy dragging(model_.get(), &SelectionModel::draggingChanged);
        mouse(QEvent::GraphicsSceneMousePress, SEL.bottomRight());
        QVERIFY(model_->is_dragging());
        mouse(QEvent::GraphicsSceneMouseMove, SEL.bottomRight() + QPointF(30, 20));
        mouse(QEvent::GraphicsSceneMouseRelease, SEL.bottomRight() + QPointF(30, 20));
        QVERIFY(!model_->is_dragging());
        QCOMPARE(model_->rect(), QRectF(SEL.topLeft(), QSizeF(190, 140)));
        QCOMPARE(dragging.count(), 2);
        // Dragging past the opposite edge flips into a normalized rect.
        mouse(QEvent::GraphicsSceneMousePress, QPointF(SEL.left(), SEL.top() + 70));
        mouse(QEvent::GraphicsSceneMouseMove, QPointF(SEL.left() + 250, SEL.top() + 70));
        mouse(QEvent::GraphicsSceneMouseRelease, QPointF(SEL.left() + 250, SEL.top() + 70));
        QCOMPARE(model_->rect(), QRectF(310, 90, 60, 140));
        // The body moves it; a constraint only touches resizes.
        selection_->resize_constraint = [](const QRectF &rect, const QRectF &, int) { return QRectF(rect.topLeft(), QSizeF(rect.width(), rect.width())); };
        mouse(QEvent::GraphicsSceneMousePress, QPointF(340, 160));
        mouse(QEvent::GraphicsSceneMouseMove, QPointF(300, 150));
        mouse(QEvent::GraphicsSceneMouseRelease, QPointF(300, 150));
        QCOMPARE(model_->rect(), QRectF(270, 80, 60, 140));
        mouse(QEvent::GraphicsSceneMousePress, QPointF(330, 220));
        mouse(QEvent::GraphicsSceneMouseMove, QPointF(350, 220));
        mouse(QEvent::GraphicsSceneMouseRelease, QPointF(350, 220));
        QCOMPARE(model_->rect(), QRectF(270, 80, 80, 80));
    }

    void textUnderTheSelectionKeepsItsOwnCursor() {
        auto *text = new QGraphicsTextItem("text");
        text->setPos(SEL.center());
        selection_scene_->addItem(text);
        hover(SEL.center() + QPointF(5, 5));
        QCOMPARE(selection_->cursor().shape(), Qt::ArrowCursor);
        QVERIFY(!selection_->hasCursor());
    }

    // ---- Watermark text, style and drawing ----

    void timeTokensExpandAndStrayDollarsStay() {
        QCOMPARE(expand_text("ACME $yyyy-MM-dd$ $HH:mm$", NOW), QString("ACME 2026-10-11 09:05"));
        QCOMPARE(expand_text("  line1\n$HH:mm:ss$  ", NOW), QString("line1\n09:05:07"));
        QCOMPARE(expand_text("cost $5", NOW), QString("cost $5"));
        QCOMPARE(expand_text("$$", NOW), QString("$"));
        QCOMPARE(expand_text("$yyyyMMddHHmmss$", NOW), QString("20261011090507"));
        for (const auto &format : watermark_time_formats()) {
            const auto text = expand_text(time_token(format.format), NOW);
            QVERIFY2(!text.isEmpty() && !text.contains('$') && text != format.format, format.name);
            QCOMPARE(time_format_short_name(format.format), QString(format.name));
        }
    }

    void watermarkStyleIsClamped() {
        const auto style = WatermarkStyle::from_values({{"color", "nope"}, {"font_size", 1000}, {"opacity", 3}, {"layout", "spiral"},
                                                        {"spacing", -5}, {"angle", 400}, {"font_family", QVariant()}});
        QCOMPARE(style.color, WatermarkStyle().color);
        QCOMPARE(style.font_size, MAX_FONT_SIZE);
        QCOMPARE(style.opacity, 1.0);
        QCOMPARE(style.layout, LAYOUT_TILE);
        QCOMPARE(style.spacing, MIN_SPACING);
        QCOMPARE(style.angle, MAX_ANGLE);
        QCOMPARE(style.font_family, QString());
        QVERIFY(style == style.normalized());
        QCOMPARE(watermark_font(ink_style()).pixelSize(), 16);
        auto arial = ink_style();
        arial.font_family = "Arial";
        QCOMPARE(watermark_font(arial).family(), QString("Arial"));
        QVERIFY(arial.with_changes({{"angle", 10}}).angle == 10);
        QVERIFY(arial != arial.with_changes({{"angle", 10}}));
    }

    void drawingOnlyExposedTilesMatchesDrawingEverything_data() {
        QTest::addColumn<QString>("text");
        QTest::newRow("one line") << "WATERMARK";
        QTest::newRow("two lines") << "LINE ONE\nTWO";
    }

    void drawingOnlyExposedTilesMatchesDrawingEverything() {
        QFETCH(QString, text);
        const QRectF rect(13, 7, 290, 181);
        const auto whole = paint(rect, ink_style(), text);
        QImage pieces = blank();
        const int step = 37;
        for (int x = 0; x < 320; x += step)
            for (int y = 0; y < 200; y += step) {
                const QRectF piece(x, y, step, step);
                pieces = paint(rect, ink_style(), text, piece, piece, pieces);
            }
        QCOMPARE(pieces, whole);
        QVERIFY(inked(whole) > 0);
    }

    void tilePlacementFollowsSpacingAngleAndExposure() {
        WatermarkStyle style;
        style.font_size = 24;
        const QRectF screen(0, 0, 2560, 1440);
        const auto everything = watermark_placements(screen, "2026-10-11 14:30", style);
        const auto nearby = watermark_placements(screen, "2026-10-11 14:30", style, QRectF(1000, 600, 400, 300));
        QVERIFY(!nearby.empty() && nearby.size() <= everything.size() / 4);
        QList<size_t> counts;
        for (int spacing : {0, 40, 100}) counts.append(watermark_placements(QRectF(0, 0, 1600, 900), "MARK", ink_style().with_changes({{"spacing", spacing}})).size());
        QVERIFY(counts[0] > counts[1] && counts[1] > counts[2] && counts[2] > 0);
        for (const auto &[centre, angle] : watermark_placements(QRectF(0, 0, 400, 300), "MARK", ink_style().with_changes({{"angle", 15}}))) QCOMPARE(angle, 15.0);
        QCOMPARE(watermark_placements(QRectF(0, 0, 400, 300), "MARK", ink_style().with_changes({{"layout", "center"}, {"angle", 15}})).front().second, 0.0);
        // Tiles never spill outside the rect.
        const QRectF small(60, 40, 120, 80);
        const auto image = paint(small, ink_style());
        QVERIFY(inked(image, small.toRect()) > 0);
        QCOMPARE(inked(image), inked(image, small.toRect()));
    }

    void eachPositionLandsInItsPartOfTheSelection() {
        const QRectF rect(20, 10, 280, 180);
        for (const auto &[layout, anchor] : watermark_anchors()) {
            const auto style = ink_style().with_changes({{"layout", layout}});
            const auto image = paint(rect, style, "MARK");
            QVERIFY2(inked(image) == inked(image, rect.toRect()) && inked(image) > 0, qPrintable(layout));
            const auto placements = watermark_placements(rect, "MARK", style);
            QCOMPARE(placements.size(), size_t(1));
            const auto centre = placements.front().first;
            const double thirds[2] = {(centre.x() - rect.left()) / rect.width(), (centre.y() - rect.top()) / rect.height()};
            const double anchors[2] = {anchor.x(), anchor.y()};
            for (int i = 0; i < 2; ++i) {
                const bool ok = anchors[i] == 0.0 ? thirds[i] < 1.0 / 3 : anchors[i] == 1.0 ? thirds[i] > 2.0 / 3 : (thirds[i] >= 1.0 / 3 && thirds[i] <= 2.0 / 3);
                QVERIFY2(ok, qPrintable(layout));
            }
        }
        QVERIFY(watermark_layouts().contains(LAYOUT_TILE));
        QVERIFY(watermark_layouts().contains(LAYOUT_CENTER));
    }

    void multilineAndNarrowSelectionsStayCentred() {
        const QRectF rect(0, 0, 320, 200);
        const auto style = ink_style().with_changes({{"layout", "center"}});
        const auto one = inked_rows(paint(rect, style, "LINE"));
        const auto two = inked_rows(paint(rect, style, "LINE\nLINE"));
        QVERIFY(two.last() - two.first() > 1.6 * (one.last() - one.first()));
        const QRectF narrow(100, 100, 20, 20);
        const auto placements = watermark_placements(narrow, "A VERY LONG WATERMARK", ink_style().with_changes({{"layout", "top_left"}}));
        QCOMPARE(placements.front().first, narrow.center());
    }

    // ---- The watermark in a scene ----

    void theWatermarkFollowsTheSelectionAndExports() {
        auto *canvas = scene();
        auto *item = WatermarkTool::add(canvas);
        QVERIFY(item);
        QCOMPARE(item->text(), QString("WATERMARK"));
        QCOMPARE(item->rect(), QRectF(40, 20, 240, 160));
        QCOMPARE(item->zValue(), WatermarkItem::Z_VALUE);
        QRectF moved(10, 10, 200, 150);
        canvas->selection_model->set_rect(moved);
        QCOMPARE(item->rect(), moved);
        ExportService exporter(canvas);
        QVERIFY(inked(exporter.export_image(moved)) > 0);
        // Pins and OCR get the plain background.
        QCOMPARE(inked(exporter.export_base_image_only(moved)), 0);
        // Exporting directly gives the same pixels as the canvas.
        const auto direct = apply_watermark(blank().copy(moved.toRect()), item->text(), item->style());
        QCOMPARE(direct.convertToFormat(QImage::Format_ARGB32), exporter.export_image(moved).convertToFormat(QImage::Format_ARGB32));
    }

    void addingIsUndoableAndThereIsOnlyOne() {
        auto *canvas = scene();
        auto *item = WatermarkTool::add(canvas);
        QCOMPARE(WatermarkTool::add(canvas), item);
        QCOMPARE(items_of<WatermarkItem>(canvas), QList<WatermarkItem *>{item});
        canvas->undo_stack->undo();
        QCOMPARE(WatermarkItem::find(canvas), nullptr);
        // Redo puts it on the current selection.
        QRectF now(0, 0, 100, 100);
        canvas->selection_model->set_rect(now);
        canvas->undo_stack->redo();
        QCOMPARE(WatermarkItem::find(canvas), item);
        QCOMPARE(item->rect(), now);

        auto *automatic = scene();
        QVERIFY(WatermarkTool::add(automatic, false));
        QVERIFY(!automatic->undo_stack->canUndo());
    }

    void styleChangesBecomeTheDefaultOutsideUndo() {
        auto *canvas = scene();
        auto *item = WatermarkTool::add(canvas);
        WatermarkTool::change_style(canvas, {{"font_size", 40}, {"layout", "bottom_right"}, {"spacing", 70}, {"angle", 10}});
        QCOMPARE(item->style().font_size, 40);
        QCOMPARE(item->style().layout, QString("bottom_right"));
        QCOMPARE(item->style().spacing, 70);
        QCOMPARE(item->style().angle, 10);
        QVERIFY(WatermarkTool::load_style(config_) == item->style());
        canvas->undo_stack->undo();
        QCOMPARE(WatermarkItem::find(canvas), nullptr);
    }

    void applyingTheTextRestampsTheTime() {
        auto *canvas = scene();
        auto *item = new WatermarkItem("ACME $yyyy-MM-dd HH:mm:ss$", ink_style(), NOW);
        canvas->addItem(item);
        QCOMPARE(item->text(), QString("ACME 2026-10-11 09:05:07"));
        QCOMPARE(WatermarkTool::change_text(canvas, "ACME $yyyy-MM-dd HH:mm:ss$"), item);
        QVERIFY(item->created_at() > NOW);
        QCOMPARE(item->text(), expand_text(item->template_text(), item->created_at()));
        QCOMPARE(config_->get_screenshot_watermark_text(), QString("ACME $yyyy-MM-dd HH:mm:ss$"));
        QCOMPARE(WatermarkTool::current_template(config_), QString("ACME $yyyy-MM-dd HH:mm:ss$"));
    }

    void clicksAndTheEraserPassThroughTheWatermark() {
        auto *canvas = scene();
        auto *rect = new RectItem(QRectF(100, 60, 80, 60), QPen(QColor("red"), 4));
        canvas->addItem(rect);
        WatermarkTool::add(canvas);
        const QPointF point(100, 90);
        for (auto *item : canvas->items(point)) QVERIFY(!dynamic_cast<WatermarkItem *>(item));
        SmartEditController controller(canvas);
        controller.set_tool("rect");
        controller.handle_press(point, point, Qt::LeftButton, Qt::NoModifier);
        QCOMPARE(controller.selected_item(), rect);
        draw(canvas, "eraser", QPointF(45, 25), QPointF(270, 170));
        QVERIFY(WatermarkItem::find(canvas));
        // The watermark tool leaves the drawing context alone.
        canvas->activate_tool("pen");
        auto &ctx = canvas->tool_controller->ctx;
        const auto before = std::make_tuple(ctx.color, ctx.stroke_width, ctx.opacity);
        canvas->activate_tool("watermark");
        canvas->activate_tool("pen");
        QVERIFY(std::make_tuple(ctx.color, ctx.stroke_width, ctx.opacity) == before);
    }

    void autoWatermarkFollowsTheLastCapture() {
        config_->set_app_setting("screenshot_watermark_auto", true);
        auto *with = scene();
        WatermarkTool::add(with);
        WatermarkTool::remember(with);
        QCOMPARE(WatermarkTool::auto_template(config_), QString("WATERMARK"));
        auto *without = scene();
        WatermarkTool::remember(without);
        QCOMPARE(WatermarkTool::auto_template(config_), QString());
        // Captures that bypass the canvas get it stamped directly.
        WatermarkTool::remember(with);
        QVERIFY(inked(WatermarkTool::apply_to_image(blank(), config_, NOW)) > 0);
        config_->set_app_setting("screenshot_watermark_auto", false);
        QCOMPARE(WatermarkTool::auto_template(config_), QString());
        QCOMPARE(inked(WatermarkTool::apply_to_image(blank(), config_, NOW)), 0);
    }

    // ---- Spotlight ----

    void spotlightsAreUndoableAndShareOneCurtain() {
        config_->update_settings("spotlight", {{"opacity", 0.5}});
        auto *canvas = scene(QRectF(0, 0, 64, 64), blank(64, 64, GRAY));
        draw(canvas, "spotlight", QPointF(10, 10), QPointF(14, 14));
        QVERIFY(items_of<SpotlightItem>(canvas).isEmpty());
        draw(canvas, "spotlight", QPointF(10, 10), QPointF(40, 30));
        QCOMPARE(items_of<SpotlightItem>(canvas).size(), 1);
        QVERIFY(items_of<SpotlightItem>(canvas).first()->sceneBoundingRect().contains(QRectF(10, 10, 30, 20)));
        canvas->undo_stack->undo();
        QVERIFY(items_of<SpotlightItem>(canvas).isEmpty());
        ExportService exporter(canvas);
        QVERIFY(color_near(exporter.export_image(QRectF(0, 0, 64, 64)).pixelColor(4, 60), GRAY));

        draw(canvas, "spotlight", QPointF(8, 8), QPointF(40, 40));
        draw(canvas, "spotlight", QPointF(24, 24), QPointF(56, 56));
        QCOMPARE(items_of<SpotlightItem>(canvas).size(), 2);
        QCOMPARE(items_of<SpotlightCurtain>(canvas).size(), 1);
        QCOMPARE(SpotlightCurtain::find(canvas), SpotlightCurtain::of(canvas));
        const auto image = exporter.export_image(QRectF(0, 0, 64, 64));
        for (const QPoint &inside : {QPoint(30, 30), QPoint(12, 12), QPoint(50, 50)}) QVERIFY(color_near(image.pixelColor(inside), GRAY));
        QVERIFY(color_near(image.pixelColor(4, 60), dimmed(GRAY)));
    }

    void theCurtainDimsMosaicButNotAnnotations() {
        config_->update_settings("spotlight", {{"opacity", 0.5}});
        auto *canvas = scene(QRectF(0, 0, 64, 64), blank(64, 64, GRAY));
        QImage red(8, 8, QImage::Format_ARGB32);
        red.fill(QColor(255, 0, 0));
        QPainterPath path;
        path.addRect(QRectF(0, 0, 16, 16));
        canvas->addItem(new MosaicItem(path, 10, 8, red, QRectF(0, 0, 64, 64), true));
        auto *blue = new RectItem(QRectF(48, 0, 16, 16), QPen(Qt::NoPen));
        blue->setBrush(QColor(0, 0, 255));
        canvas->addItem(blue);
        draw(canvas, "spotlight", QPointF(24, 24), QPointF(40, 40));
        const auto image = ExportService(canvas).export_image(QRectF(0, 0, 64, 64));
        QVERIFY(color_near(image.pixelColor(8, 8), dimmed(QColor(255, 0, 0))));
        QVERIFY(color_near(image.pixelColor(56, 8), QColor(0, 0, 255)));
    }

    void darknessFollowsTheSelectedHoleAndTheToolForNewOnes() {
        config_->update_settings("spotlight", {{"opacity", 0.5}});
        auto *canvas = scene(QRectF(0, 0, 64, 64), blank(64, 64, GRAY));
        draw(canvas, "spotlight", QPointF(20, 20), QPointF(40, 40));
        auto *curtain = SpotlightCurtain::find(canvas);
        auto *item = items_of<SpotlightItem>(canvas).first();
        QVERIFY(std::abs(curtain->opacity() - 0.5) < 1e-6);
        QVERIFY(item->set_visual_opacity(0.25));
        QVERIFY(std::abs(curtain->opacity() - 0.25) < 1e-6);
        QVERIFY(std::abs(*item->get_visual_opacity() - 0.25) < 1e-6);
        QVERIFY(!item->get_stroke_width());
        QVERIFY(!item->scale_stroke_width(2));
        canvas->update_style(std::nullopt, std::nullopt, 0.8);
        QVERIFY(std::abs(curtain->opacity() - 0.25) < 1e-6);
        draw(canvas, "spotlight", QPointF(44, 44), QPointF(60, 60));
        QVERIFY(std::abs(curtain->opacity() - 0.8) < 1e-6);
    }

    void spotlightSelectionAndRoundedHoles() {
        config_->update_settings("spotlight", {{"opacity", 0.5}});
        auto *canvas = scene(QRectF(0, 0, 64, 64), blank(64, 64, GRAY));
        draw(canvas, "spotlight", QPointF(10, 10), QPointF(50, 50));
        auto *item = items_of<SpotlightItem>(canvas).first();
        ExportService exporter(canvas);
        QVERIFY(color_near(exporter.export_image(QRectF(0, 0, 64, 64)).pixelColor(11, 11), GRAY));
        item->set_corner_radius(12);
        const auto rounded = exporter.export_image(QRectF(0, 0, 64, 64));
        QVERIFY(color_near(rounded.pixelColor(11, 11), dimmed(GRAY)));
        QVERIFY(color_near(rounded.pixelColor(30, 30), GRAY));
        QVERIFY(!item->hole_path().contains(QPointF(11, 11)));

        // Only the spotlight tool selects holes, and the whole hole counts; the curtain does not.
        auto *rect = new RectItem(QRectF(10, 10, 20, 20), QPen(QColor(255, 0, 0), 3));
        canvas->addItem(rect);
        SmartEditController controller(canvas);
        QCOMPARE(controller.get_item_tool_id(item), QString("spotlight"));
        for (const char *tool : {"rect", "ellipse", "highlighter", "cursor"}) {
            controller.set_tool(tool);
            QVERIFY2(!controller.can_select_item(item, Qt::NoModifier), tool);
            QVERIFY2(!controller.can_show_hover_cursor(item), tool);
        }
        controller.set_tool("spotlight");
        QVERIFY(controller.can_select_item(item, Qt::NoModifier));
        QVERIFY(!controller.can_select_item(rect, Qt::NoModifier));
        controller.cross_tool_select_enabled = true;
        canvas->removeItem(rect);
        delete rect;
        item->set_corner_radius(0);
        for (const auto &[pos, expected] : std::initializer_list<std::pair<QPointF, bool>>{{QPointF(40, 40), true}, {QPointF(10, 30), true}, {QPointF(4, 4), false}}) {
            controller.clear_selection(true);
            controller.set_tool("pen");
            controller.handle_press(pos, pos, Qt::LeftButton, Qt::ControlModifier);
            QVERIFY2((controller.selected_item() == item) == expected, qPrintable(QString("%1,%2 selected %3").arg(pos.x()).arg(pos.y()).arg(controller.selected_item() ? typeid(*controller.selected_item()).name() : "none")));
        }
    }

    // ---- Mosaic ----

    void rectMosaicFillsAndSelectsItselfButFreehandDoesNot() {
        config_->update_settings("mosaic", {{"draw_mode", MosaicTool::MODE_RECT}});
        auto *canvas = scene(QRectF(0, 0, 64, 64), gradient());
        QSignalSpy auto_selected(canvas, &CanvasScene::item_auto_select_requested);
        draw(canvas, "mosaic", QPointF(10, 10), QPointF(50, 40));
        const auto mosaics = items_of<MosaicItem>(canvas);
        QCOMPARE(mosaics.size(), 1);
        QVERIFY(mosaics.first()->fill_mode());
        QVERIFY(mosaics.first()->shows_selection_frame());
        QCOMPARE(mosaics.first()->rect(), QRectF(10, 10, 40, 30));
        QCOMPARE(canvas->undo_stack->count(), 1);
        QCOMPARE(auto_selected.count(), 1);
        // A tiny drag is a misclick.
        draw(canvas, "mosaic", QPointF(10, 50), QPointF(14, 54));
        QCOMPARE(items_of<MosaicItem>(canvas).size(), 1);
        config_->update_settings("mosaic", {{"draw_mode", MosaicTool::MODE_FREEHAND}});
        draw(canvas, "mosaic", QPointF(10, 50), QPointF(50, 55));
        QCOMPARE(items_of<MosaicItem>(canvas).size(), 2);
        QCOMPARE(auto_selected.count(), 1);
        // setRect means nothing for freehand strokes.
        MosaicItem freehand(QPainterPath(QPointF(5, 5)), 8, 8, gradient(4, 4), QRectF(0, 0, 64, 64), false);
        const auto before = freehand.path();
        freehand.setRect(QRectF(0, 0, 30, 30));
        QCOMPARE(freehand.path(), before);
        QVERIFY(!freehand.shows_selection_frame());
    }

    void aSingleClickMosaicPaintsARoundDot() {
        auto *canvas = scene(QRectF(0, 0, 64, 64), gradient());
        draw(canvas, "mosaic", QPointF(32, 32), QPointF(32, 32));
        const auto mosaics = items_of<MosaicItem>(canvas);
        QCOMPARE(mosaics.size(), 1);
        const auto bounds = mosaics.first()->shape().boundingRect();
        QVERIFY(bounds.width() > 2 && std::abs(bounds.width() - bounds.height()) < 1);
        QVERIFY(mosaics.first()->shape().contains(QPointF(32, 32)));
        const auto before = ExportService(canvas).export_base_image_only(QRectF(0, 0, 64, 64));
        const auto after = ExportService(canvas).export_image(QRectF(0, 0, 64, 64));
        QVERIFY(before.pixelColor(31, 33) != after.pixelColor(31, 33) || before.pixelColor(33, 31) != after.pixelColor(33, 31));
    }

    void blockSizeAndStyleChangeTheItemOnly() {
        QPainterPath path;
        path.addRect(QRectF(0, 0, 32, 32));
        MosaicItem item(path, 10, 8, gradient(8, 8), QRectF(0, 0, 64, 64), true);
        QCOMPARE(item.block_size(), 8);
        const auto smaller = gradient(16, 16);
        item.set_block_size(4, smaller);
        QCOMPARE(item.block_size(), 4);
        QCOMPARE(item.reduced_image(), smaller);
        QVERIFY(!item.smooth());
        item.set_smooth(true);
        QVERIFY(item.smooth());
        QCOMPARE(*item.get_stroke_width(), 10.0);
        item.translate_background_anchor(-8, 4);
        QCOMPARE(item.background_rect(), QRectF(-8, 4, 64, 64));
        QCOMPARE(MosaicTool::clamp_block_size(6), 4);
        QCOMPARE(MosaicTool::clamp_block_size(999), MosaicTool::MAX_BLOCK_SIZE);
        QCOMPARE(MosaicTool::clamp_block_size(QString("x")), MosaicTool::DEFAULT_BLOCK_SIZE);
    }

    void theEraserRemovesAndRestoresMosaicAndShapes() {
        auto *canvas = scene(QRectF(0, 0, 64, 64), gradient());
        draw(canvas, "mosaic", QPointF(10, 10), QPointF(50, 12));
        auto *rect = new RectItem(QRectF(5, 5, 50, 10), QPen(QColor("red"), 3));
        canvas->addItem(rect);
        canvas->undo_stack->push(new AddItemCommand(canvas, ItemReference(rect)));
        QCOMPARE(items_of<MosaicItem>(canvas).size(), 1);
        draw(canvas, "eraser", QPointF(30, 11), QPointF(30, 11));
        // One click takes everything under it, as one step.
        QVERIFY(items_of<MosaicItem>(canvas).isEmpty());
        QVERIFY(!rect->scene());
        canvas->undo_stack->undo();
        QCOMPARE(items_of<MosaicItem>(canvas).size(), 1);
        QCOMPARE(rect->scene(), canvas);
        // Mosaic stays below normal annotations.
        QVERIFY(items_of<MosaicItem>(canvas).first()->zValue() < rect->zValue());
    }

    // ---- Smart selection levels ----

    void smartSelectionLevelsOnlyGrowOutwards() {
        std::vector<SmartRect> chain;
        append_level(chain, {10, 10, 50, 50});
        // Within two pixels on every side it is the same level.
        append_level(chain, {9, 9, 51, 52});
        QCOMPARE(chain.size(), size_t(1));
        // A smaller rect never steps outwards.
        append_level(chain, {20, 20, 30, 30});
        QCOMPARE(chain.size(), size_t(1));
        append_level(chain, {0, 0, 100, 100});
        append_level(chain, {0, 0, 1920, 1080});
        QCOMPARE(chain.size(), size_t(3));
        QCOMPARE(chain.back(), (SmartRect{0, 0, 1920, 1080}));
    }
};

QTEST_MAIN(CanvasItemTests)
#include "test_canvas_items.moc"
