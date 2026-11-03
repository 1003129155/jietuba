#pragma once
#include <QPoint>
#include <QString>
#include <functional>

struct IDropTarget;

namespace jietuba {

// Lets what is dragged be dropped onto the folders a window shows, as onto folders in Explorer: the drop goes to that
// folder's own shell drop target, which picks copy or move as Explorer does, asks about name conflicts, shows progress
// and records the undo.
class FolderDrop {
public:
    // The folder at a point of the screen, in physical pixels, which are Qt's global coordinates with its high-DPI
    // scaling off (see setup_environment); empty where nothing may be dropped.
    using FolderAt = std::function<QString(const QPoint &screen)>;
    // The folder a drop would go to as the cursor moves, empty once there is none or the drag leaves or ends.
    using Hovered = std::function<void(const QString &folder)>;

    FolderDrop(FolderAt folder_at, Hovered hovered);
    ~FolderDrop();
    FolderDrop(const FolderDrop &) = delete;
    FolderDrop &operator=(const FolderDrop &) = delete;

    // Takes the drops of the window with handle hwnd in place of Qt's drop target, which Qt registers for every
    // top-level window. Called again whenever Qt may have registered its own anew; false when it could not.
    bool attach(quintptr hwnd);
    // What OLE calls during a drag; tests drive it directly.
    IDropTarget *target() const;

private:
    class Target;
    Target *target_;
    quintptr hwnd_ = 0;
};
}
