#include "text_recognition/layout.h"
#include "text_recognition/placement.h"
#include "ui/submenu_position.h"
#include <QTest>

using namespace jietuba;
using namespace jietuba::text_layout;
using namespace jietuba::text_placement;

// Cases ported from the reference's tests (test_text_recognition_layout.py,
// test_text_recognition_placement.py, test_submenu_position.py).
namespace {
constexpr int HEIGHT = 20;
constexpr int LEADING = 8;

OcrTextLine line(const QString &text, int row, double left = 0, double right = 400) {
    const double top = row * (HEIGHT + LEADING);
    return {text, left, right, top, top + HEIGHT};
}

QVector<OcrTextLine> paragraph(const QStringList &texts) {
    QVector<OcrTextLine> lines;
    for (int row = 0; row < texts.size(); ++row) lines << line(texts[row], row);
    return lines;
}

ScreenArea screen_area(int x, int y, int width, int height, int taskbar = 40) {
    const QRect rect(x, y, width, height);
    return {rect, rect.adjusted(0, 0, 0, -taskbar)};
}

const QSize SIZE(520, 420);
const ScreenArea MAIN = screen_area(0, 0, 1920, 1080);
const ScreenArea SIDE = screen_area(1920, 0, 2560, 1440);
const std::vector<ScreenArea> SCREENS = {MAIN, SIDE};
const QRect AVAILABLE = MAIN.available;
const QRect ON_MAIN(300, 200, 400, 300);
const QRect ON_SIDE(2100, 200, 400, 300);

bool same(const ScreenArea &a, const ScreenArea &b) { return a.geometry == b.geometry && a.available == b.available; }

ScreenArea pick(const QString &position, std::optional<QRect> anchor = std::nullopt, std::optional<QPoint> cursor = QPoint(100, 100),
                std::optional<QPoint> last_center = std::nullopt) {
    return pick_screen(position, anchor, cursor, last_center, SCREENS, MAIN);
}

QRect placed(const QString &position, const ScreenArea &screen = MAIN, std::optional<QRect> anchor = std::nullopt, std::optional<QPoint> cursor = std::nullopt,
             std::optional<QPoint> last_center = std::nullopt) {
    return QRect(window_position(position, SIZE, screen, anchor, cursor, last_center), SIZE);
}
}

class TextRecognitionTests : public QObject {
    Q_OBJECT
private slots:
    void layoutModes() {
        QCOMPARE(render(paragraph({"第一行", "第二行"}), MODE_MULTILINE), QString("第一行\n第二行"));
        QCOMPARE(render(paragraph({"第一段。", "第二段。"}), MODE_SINGLE_LINE), QString("第一段。第二段。"));
        QCOMPARE(render(paragraph({"hello", "world"}), MODE_SINGLE_LINE), QString("hello world"));
        QCOMPARE(render({}, MODE_AUTO), QString());
        QCOMPARE(render(paragraph({"只有一行"}), MODE_AUTO), QString("只有一行"));
    }
    void layoutAuto() {
        QCOMPARE(render(paragraph({"这一行排到了右边界", "接着往下排"})), QString("这一行排到了右边界接着往下排"));
        QCOMPARE(render(paragraph({"上一段到此结束。", "下一段从这里开始"})), QString("上一段到此结束。\n下一段从这里开始"));
        QCOMPARE(render({line("这一行没排到右边", 0, 0, 200), line("下一行", 1)}), QString("这一行没排到右边\n下一行"));
        QCOMPARE(render({line("上一行满到右边", 0), line("缩进开头的一行", 1, 60)}), QString("上一行满到右边\n缩进开头的一行"));
        QCOMPARE(render({line("上一行满到右边", 0), line("隔了一大段距离", 3)}), QString("上一行满到右边\n隔了一大段距离"));
        QCOMPARE(render(paragraph({"recog-", "nition"})), QString("recognition"));
        // A large heading: a mean line height would misread body spacing as a paragraph break.
        QCOMPARE(render({OcrTextLine{"标题", 0, 400, 0, 80}, line("正文第一行", 4), line("正文第二行", 5)}), QString("标题\n正文第一行正文第二行"));
    }
    void removeSpaces() {
        QCOMPARE(render(paragraph({"识 别 出 的 文 字。", "下 一 行"}), MODE_AUTO, true), QString("识别出的文字。\n下一行"));
        QCOMPARE(render(paragraph({"全角　空格"}), MODE_AUTO, true), QString("全角空格"));
        QCOMPARE(render(paragraph({"Remove the noise, not the words."}), MODE_AUTO, true), QString("Remove the noise, not the words."));
        QCOMPARE(render(paragraph({"打开 Visual Studio Code 看看"}), MODE_AUTO, true), QString("打开Visual Studio Code看看"));
    }
    void punctuation() {
        QCOMPARE(render(paragraph({"你好，世界（真的）！"}), MODE_AUTO, false, PUNCT_HALFWIDTH), QString("你好,世界(真的)!"));
        QCOMPARE(render(paragraph({"你好,世界!"}), MODE_AUTO, false, PUNCT_FULLWIDTH), QString("你好，世界！"));
        QCOMPARE(render(paragraph({"圆周率是 3.14 左右"}), MODE_AUTO, false, PUNCT_FULLWIDTH), QString("圆周率是 3.14 左右"));
        QCOMPARE(render(paragraph({"Hello, world!"}), MODE_AUTO, false, PUNCT_FULLWIDTH), QString("Hello, world!"));
    }
    void pickScreen() {
        QVERIFY(same(pick("region_screen", ON_SIDE), SIDE));
        QVERIFY(same(pick("region_screen", std::nullopt, QPoint(2000, 100)), SIDE));
        const QRect off(9000, 9000, 10, 10);
        QVERIFY(same(pick("region_screen", off, QPoint(2000, 100)), SIDE));
        QVERIFY(same(pick("region_screen", off, QPoint(9000, 9000)), MAIN));
        QVERIFY(same(pick("primary_screen", ON_SIDE, QPoint(2000, 100), QPoint(2100, 100)), MAIN));
        QVERIFY(same(pick("beside_region", ON_SIDE), SIDE));
        QVERIFY(same(pick("near_cursor", ON_SIDE, QPoint(100, 100)), MAIN));
        QVERIFY(same(pick("near_cursor", ON_SIDE, QPoint(9000, 9000)), SIDE));
        QVERIFY(same(pick("last_position", ON_MAIN, QPoint(100, 100), QPoint(2100, 100)), SIDE));
        QVERIFY(same(pick("last_position", ON_SIDE, QPoint(100, 100), std::nullopt), SIDE));
        QVERIFY(same(pick("last_position", ON_SIDE, QPoint(100, 100), QPoint(9000, 9000)), SIDE));
    }
    void centerPositions() {
        QCOMPARE(placed("region_screen", MAIN, QRect(50, 50, 200, 100)).center(), AVAILABLE.center());
        QCOMPARE(placed("primary_screen", MAIN, QRect(50, 50, 200, 100)).center(), AVAILABLE.center());
        QVERIFY(placed("region_screen").center() != MAIN.geometry.center());
    }
    void besideRegion() {
        const auto at = [](const QRect &anchor, const QSize &size = SIZE) { return window_position("beside_region", size, MAIN, anchor); };
        QCOMPARE(placed("beside_region").center(), AVAILABLE.center());
        QRect anchor(100, 200, 300, 200);
        QCOMPARE(at(anchor), QPoint(anchor.right() + 1 + GAP, 200));
        QCOMPARE(at(QRect(1500, 200, 300, 200)), QPoint(1500 - GAP - SIZE.width(), 200));
        anchor = QRect(100, 100, 1700, 300);
        QCOMPARE(at(anchor), QPoint(100, anchor.bottom() + 1 + GAP));
        anchor = QRect(100, 700, 1700, 300);
        QCOMPARE(at(anchor), QPoint(100, anchor.top() - GAP - SIZE.height()));
        QCOMPARE(placed("beside_region", MAIN, QRect(10, 10, 1900, 1020)).center(), AVAILABLE.center());
        anchor = QRect(100, 900, 300, 130);
        QCOMPARE(at(anchor), QPoint(anchor.right() + 1 + GAP, AVAILABLE.bottom() + 1 - SIZE.height()));
        anchor = QRect(900, 100, 1000, 300);
        QCOMPARE(at(anchor, QSize(1200, 420)), QPoint(AVAILABLE.right() + 1 - 1200, anchor.bottom() + 1 + GAP));
        for (const auto &region : {QRect(0, 0, 100, 100), QRect(1820, 0, 100, 100), QRect(0, 940, 100, 100), QRect(1820, 940, 100, 100),
                                   QRect(900, 500, 100, 100), QRect(0, 0, 1920, 200), QRect(0, 840, 1920, 200), QRect(0, 0, 200, 1040),
                                   QRect(1720, 0, 200, 1040), QRect(300, 100, 1300, 800), QRect(0, 0, 1920, 1040)}) {
            const QRect rect(at(region), SIZE);
            QVERIFY(AVAILABLE.contains(rect));
            QVERIFY(rect.center() == AVAILABLE.center() || !rect.intersects(region));
        }
    }
    void nearCursor() {
        const auto at = [](const QPoint &cursor, const ScreenArea &screen = MAIN) { return window_position("near_cursor", SIZE, screen, std::nullopt, cursor); };
        QCOMPARE(at(QPoint(300, 200)), QPoint(300 + CURSOR_OFFSET, 200 + CURSOR_OFFSET));
        QCOMPARE(at(QPoint(1800, 200)), QPoint(1800 - CURSOR_OFFSET - SIZE.width(), 200 + CURSOR_OFFSET));
        QCOMPARE(at(QPoint(300, 900)), QPoint(300 + CURSOR_OFFSET, 900 - CURSOR_OFFSET - SIZE.height()));
        QCOMPARE(at(QPoint(1900, 1000)), QPoint(1900 - CURSOR_OFFSET - SIZE.width(), 1000 - CURSOR_OFFSET - SIZE.height()));
        QCOMPARE(at(QPoint(200, 150), screen_area(0, 0, 400, 300, 0)), QPoint(0, 0));
        QCOMPARE(placed("near_cursor", MAIN, std::nullopt, QPoint(2100, 100)).center(), AVAILABLE.center());
        QCOMPARE(placed("near_cursor").center(), AVAILABLE.center());
    }
    void lastPosition() {
        QCOMPARE(placed("last_position", MAIN, std::nullopt, std::nullopt, QPoint(900, 500)).center(), QPoint(900, 500));
        QCOMPARE(window_position("last_position", SIZE, MAIN, std::nullopt, std::nullopt, QPoint(10, 10)), QPoint(0, 0));
        QCOMPARE(window_position("last_position", SIZE, MAIN, std::nullopt, std::nullopt, QPoint(1915, 1035)),
                 QPoint(AVAILABLE.right() + 1 - SIZE.width(), AVAILABLE.bottom() + 1 - SIZE.height()));
        QCOMPARE(placed("last_position", MAIN, std::nullopt, std::nullopt, QPoint(2100, 100)).center(), AVAILABLE.center());
        QCOMPARE(placed("last_position").center(), AVAILABLE.center());
        QCOMPARE(placed("last_position", SIDE, std::nullopt, std::nullopt, QPoint(3000, 700)).center(), QPoint(3000, 700));
    }
    void multiMonitorLayouts() {
        const auto left = screen_area(-1920, 0, 1920, 1080), above = screen_area(0, -1080, 1920, 1080), portrait = screen_area(-3000, -420, 1080, 1920);
        const std::vector<ScreenArea> layout = {MAIN, SIDE, left, above, portrait};
        for (const auto &screen : layout) {
            const QRect anchor(screen.geometry.left() + 100, screen.geometry.top() + 100, 300, 200);
            const auto point = anchor.center();
            for (const QString position : {"region_screen", "beside_region", "near_cursor", "last_position"}) {
                const auto picked = pick_screen(position, anchor, point, point, layout, MAIN);
                QVERIFY(same(picked, screen));
                QVERIFY(picked.available.contains(QRect(window_position(position, SIZE, picked, anchor, point, point), SIZE)));
            }
        }
        QRect anchor(-1800, 200, 300, 200);
        QCOMPARE(window_position("beside_region", SIZE, left, anchor), QPoint(anchor.right() + 1 + GAP, 200));
        const QPoint cursor(-500, 300);
        QCOMPARE(window_position("near_cursor", SIZE, left, std::nullopt, cursor), QPoint(cursor.x() - CURSOR_OFFSET - SIZE.width(), cursor.y() + CURSOR_OFFSET));
    }
    void submenuPosition() {
        QCOMPARE(submenu_position(QSize(180, 160), 200, {QRect(400, 200, 200, 120), QRect(600, 100, 200, 400)}, QRect(0, 0, 1000, 800)), QPoint(220, 200));
        QCOMPARE(submenu_position(QSize(180, 160), 200, {QRect(300, 200, 200, 120), QRect(100, 100, 200, 400)}, QRect(0, 0, 1000, 800)), QPoint(500, 200));
        QCOMPARE(submenu_position(QSize(180, 160), 200, {QRect(400, 200, 200, 120), QRect(200, 100, 200, 400)}, QRect(0, 0, 750, 800)), QPoint(570, 200));
        QCOMPARE(submenu_position(QSize(180, 200), 650, {QRect(-250, 600, 200, 120)}, QRect(-1000, 0, 1000, 800)), QPoint(-430, 600));
        const QRect screen(0, 0, 1000, 800), parent(300, 200, 200, 120), action(305, 230, 190, 30);
        QCOMPARE(submenu_position(QSize(180, 160), 230, {parent}, screen, action), QPoint(495, 230));
        QCOMPARE(submenu_position(QSize(180, 160), 230, {parent, QRect(500, 100, 200, 400)}, screen, action), QPoint(125, 230));
    }
};

QTEST_MAIN(TextRecognitionTests)
#include "test_text_recognition.moc"
