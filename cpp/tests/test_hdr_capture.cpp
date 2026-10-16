#include "capture/capture_service.h"
#include "core/native.h"
#include <QGuiApplication>
#include <QScreen>
#include <QTest>
#include <vector>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace jietuba;

namespace {
// Read independently of capture_service, so the engine choice is checked against the displays.
bool any_hdr_display() {
    std::vector<JbHdrMonitor> displays(16);
    size_t count = 0;
    char message[256] = {};
    if (!jb_hdr_displays(displays.data(), displays.size(), &count, message, sizeof message)) return false;
    for (size_t i = 0; i < std::min(count, displays.size()); ++i)
        if (displays[i].hdr_enabled) return true;
    return false;
}

struct ResourceCounts {
    DWORD gdi = 0;
    DWORD user = 0;
    DWORD handles = 0;
};

ResourceCounts resource_counts() {
    const auto process = GetCurrentProcess();
    ResourceCounts counts;
    counts.gdi = GetGuiResources(process, GR_GDIOBJECTS);
    counts.user = GetGuiResources(process, GR_USEROBJECTS);
    GetProcessHandleCount(process, &counts.handles);
    return counts;
}
}

// Runs against the real desktop. On SDR displays the static and adaptive mappings
// leave pixels unchanged, so the HDR engine must match GDI exactly.
class HdrCaptureTests : public QObject {
    Q_OBJECT
private slots:
    void cleanupTestCase() { shutdown_hdr_session(); }
    void hdrRegionMatchesGdiOnSdr() {
        if (hdr_display_active()) QSKIP("A display has HDR on; the engines legitimately differ there.");
        const auto screen = QGuiApplication::primaryScreen()->geometry();
        const QRect rect(screen.x() + 40, screen.y() + 40, 160, 120);
        CaptureService gdi("mss"), hdr("hdr");
        QImage hdr_image;
        try {
            hdr_image = hdr.capture_region(rect);
        } catch (const std::exception &e) {
            QSKIP(qPrintable(QString("No HDR session here: %1").arg(QString::fromUtf8(e.what()))));
        }
        QCOMPARE(hdr_image.size(), rect.size());
        // The desktop may change between grabs (a clock, a caret), so either neighbour counts.
        const auto before = gdi.capture_region(rect).convertToFormat(QImage::Format_RGB32);
        const auto again = hdr.capture_region(rect).convertToFormat(QImage::Format_RGB32);
        const auto after = gdi.capture_region(rect).convertToFormat(QImage::Format_RGB32);
        QVERIFY(again == before || again == after || hdr_image.convertToFormat(QImage::Format_RGB32) == before);
    }
    void hdrWholeDesktopHasVirtualDesktopGeometry() {
        CaptureService hdr("hdr");
        std::pair<QImage, QRectF> result;
        try {
            result = hdr.capture_all_screens();
        } catch (const std::exception &e) {
            QSKIP(qPrintable(QString("No HDR session here: %1").arg(QString::fromUtf8(e.what()))));
        }
        QCOMPARE(result.first.size(), result.second.size().toSize());
        QVERIFY(!result.first.isNull());
    }
    void gdiWholeDesktopIsTheVirtualScreen() {
        const auto [image, rect] = CaptureService("mss").capture_all_screens();
        QCOMPARE(rect, QRectF(GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), GetSystemMetrics(SM_CXVIRTUALSCREEN),
                              GetSystemMetrics(SM_CYVIRTUALSCREEN)));
        QCOMPARE(image.size(), rect.size().toSize());
    }
    void engineChoiceFollowsSettingAndDisplays() {
        QVERIFY(uses_hdr_engine("hdr"));
        QVERIFY(!uses_hdr_engine("mss"));
        QCOMPARE(uses_hdr_engine("auto"), any_hdr_display());
        QCOMPARE(hdr_display_active(), any_hdr_display());
    }
    // Every grab creates and frees its own DCs and bitmap, so repeated captures leave
    // the process's GDI, USER and kernel handle counts where they were.
    void gdiCaptureReleasesItsHandles() {
        CaptureService gdi("mss");
        const auto screen = QGuiApplication::primaryScreen()->geometry();
        const QRect rect(screen.x() + 40, screen.y() + 40, 160, 120);
        gdi.capture_region(rect);
        gdi.capture_all_screens();
        const auto before = resource_counts();
        for (int i = 0; i < 20; ++i) gdi.capture_region(rect);
        for (int i = 0; i < 5; ++i) gdi.capture_all_screens();
        const auto after = resource_counts();
        QCOMPARE(after.gdi, before.gdi);
        QCOMPARE(after.user, before.user);
        QCOMPARE(after.handles, before.handles);
    }
};

QTEST_MAIN(HdrCaptureTests)
#include "test_hdr_capture.moc"
