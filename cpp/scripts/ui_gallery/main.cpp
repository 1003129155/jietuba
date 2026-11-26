// Development-only design gallery (not part of any release package). Settings go to a temporary
// folder, so the gallery never reads or writes the user's own.
#include "gallery_window.h"
#include "settings/tool_settings.h"
#include <QApplication>
#include <QPointer>
#include <QSettings>
#include <QTemporaryDir>
#include <functional>

using namespace jietuba;
using namespace jietuba::gallery;

int main(int argc, char **argv) {
    QApplication application(argc, argv);
    QTemporaryDir sandbox;
    if (!sandbox.isValid()) return 1;
    QSettings settings(sandbox.filePath("gallery.ini"), QSettings::IniFormat);
    auto *config = get_tool_settings_manager(&settings);
    init_gallery_runtime(application, config);

    QPointer<GalleryWindow> window;
    // A state change builds the next window in place of the current one, at the same position. The
    // current window is deleted later: the change comes from one of its own buttons.
    std::function<void(const GalleryState &)> open = [&](const GalleryState &state) {
        auto *next = new GalleryWindow(state);
        next->setAttribute(Qt::WA_DeleteOnClose);
        QObject::connect(next, &GalleryWindow::state_requested, next, [&open](const GalleryState &requested) { open(requested); });
        if (window) next->move(window->pos());
        next->show();
        if (window) {
            window->hide();
            window->deleteLater();
        }
        window = next;
    };
    open(GalleryState{});
    return application.exec();
}
