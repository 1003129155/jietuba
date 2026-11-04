#include "core/folder_drop.h"
#include <QDir>
#include <atomic>
#include <optional>
#include <string>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace jietuba {
namespace {
QStringList dragged_paths(IDataObject *data) {
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (!data || FAILED(data->GetData(&format, &medium))) return {};
    QStringList paths;
    const auto drop = static_cast<HDROP>(medium.hGlobal);
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        const UINT length = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring path(length + 1, L'\0');
        DragQueryFileW(drop, i, path.data(), length + 1);
        paths.append(QDir::cleanPath(QDir::fromNativeSeparators(QString::fromWCharArray(path.c_str(), static_cast<int>(length)))));
    }
    ReleaseStgMedium(&medium);
    return paths;
}

// Explorer refuses a folder dropped into itself or into one inside it.
bool inside_dragged(const QString &folder, const QStringList &dragged) {
    const QString target = QDir::cleanPath(QDir::fromNativeSeparators(folder));
    for (const auto &path : dragged)
        if (target.compare(path, Qt::CaseInsensitive) == 0 || target.startsWith(path.endsWith(u'/') ? path : path + u'/', Qt::CaseInsensitive)) return true;
    return false;
}

// Its prompts and progress are owned by window, so they show above it even when it is topmost.
ComPtr<IDropTarget> shell_drop_target(const QString &folder, HWND window) {
    const QString native = QDir::toNativeSeparators(folder);
    PIDLIST_ABSOLUTE item = nullptr;
    if (FAILED(SHParseDisplayName(reinterpret_cast<PCWSTR>(native.utf16()), nullptr, &item, 0, nullptr))) return nullptr;
    ComPtr<IShellFolder> parent;
    PCUITEMID_CHILD child = nullptr;
    ComPtr<IDropTarget> target;
    if (SUCCEEDED(SHBindToParent(item, IID_PPV_ARGS(&parent), &child)))
        parent->GetUIObjectOf(window, 1, &child, IID_IDropTarget, nullptr, reinterpret_cast<void **>(target.GetAddressOf()));
    CoTaskMemFree(item);
    return target;
}
}

class FolderDrop::Target final : public IDropTarget {
public:
    Target(FolderAt folder_at, Hovered hovered) : folder_at_(std::move(folder_at)), hovered_(std::move(hovered)) {}

    // The owner is going away; a drag still holding this object sees no folders from now on.
    void detach() {
        folder_at_ = nullptr;
        hovered_ = nullptr;
    }

    void set_window(HWND hwnd) {
        hwnd_ = hwnd;
        // Shows the drag image Explorer and other programs give, over this window too.
        if (hwnd && !helper_) CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper_));
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
        if (!object) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *object = static_cast<IDropTarget *>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (left == 0) delete this;
        return left;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *data, DWORD keys, POINTL pt, DWORD *effect) override {
        if (!effect) return E_INVALIDARG;
        data_ = data;
        dragged_.reset();
        over(keys, pt, effect);
        if (helper_) {
            POINT point{pt.x, pt.y};
            helper_->DragEnter(hwnd_, data, &point, *effect);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL pt, DWORD *effect) override {
        if (!effect) return E_INVALIDARG;
        over(keys, pt, effect);
        if (helper_) {
            POINT point{pt.x, pt.y};
            helper_->DragOver(&point, *effect);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override {
        leave_folder();
        if (helper_) helper_->DragLeave();
        end();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject *data, DWORD keys, POINTL pt, DWORD *effect) override {
        if (!effect) return E_INVALIDARG;
        data_ = data;
        const DWORD allowed = *effect;
        over(keys, pt, effect);
        if (folder_target_ && *effect != DROPEFFECT_NONE) {
            // Passed on as the folder answers: none once it has moved the files itself, so the source deletes nothing.
            *effect = allowed;
            if (FAILED(folder_target_->Drop(data, keys, pt, effect))) *effect = DROPEFFECT_NONE;
            // The folder's target has taken the drop, which ends its drag too.
            folder_target_.Reset();
        } else {
            *effect = DROPEFFECT_NONE;
        }
        leave_folder();
        if (helper_) {
            POINT point{pt.x, pt.y};
            helper_->Drop(data, &point, *effect);
        }
        end();
        return S_OK;
    }

private:
    ~Target() = default;

    // Follows the cursor onto another folder, or off them, and takes the effect from the folder's own target.
    void over(DWORD keys, POINTL pt, DWORD *effect) {
        const DWORD allowed = *effect;
        *effect = DROPEFFECT_NONE;
        const QString folder = folder_at_ ? folder_at_(QPoint(pt.x, pt.y)) : QString();
        if (folder != folder_) {
            leave_folder();
            folder_ = folder;
            if (!folder.isEmpty() && !inside_dragged(folder, dragged())) folder_target_ = shell_drop_target(folder, hwnd_);
            if (folder_target_) {
                *effect = allowed;
                if (FAILED(folder_target_->DragEnter(data_.Get(), keys, pt, effect))) {
                    folder_target_.Reset();
                    *effect = DROPEFFECT_NONE;
                }
            }
        } else if (folder_target_) {
            *effect = allowed;
            if (FAILED(folder_target_->DragOver(keys, pt, effect))) *effect = DROPEFFECT_NONE;
        }
        show(*effect != DROPEFFECT_NONE ? folder_ : QString());
    }

    // Read once a folder is reached: a drag out of this program writes its files only when they are asked for.
    const QStringList &dragged() {
        if (!dragged_) dragged_ = dragged_paths(data_.Get());
        return *dragged_;
    }

    void leave_folder() {
        if (folder_target_) folder_target_->DragLeave();
        folder_target_.Reset();
        folder_.clear();
    }

    void show(const QString &folder) {
        if (folder == shown_) return;
        shown_ = folder;
        if (hovered_) hovered_(folder);
    }

    void end() {
        data_.Reset();
        dragged_.reset();
        show({});
    }

    std::atomic<ULONG> refs_{1};
    FolderAt folder_at_;
    Hovered hovered_;
    HWND hwnd_ = nullptr;
    ComPtr<IDropTargetHelper> helper_;
    ComPtr<IDataObject> data_;
    std::optional<QStringList> dragged_;
    // The folder under the cursor, and its target when a drop may go there.
    QString folder_;
    ComPtr<IDropTarget> folder_target_;
    QString shown_;
};

FolderDrop::FolderDrop(FolderAt folder_at, Hovered hovered) : target_(new Target(std::move(folder_at), std::move(hovered))) {}

FolderDrop::~FolderDrop() {
    target_->detach();
    if (hwnd_ && IsWindow(reinterpret_cast<HWND>(hwnd_))) RevokeDragDrop(reinterpret_cast<HWND>(hwnd_));
    target_->Release();
}

bool FolderDrop::attach(quintptr hwnd) {
    const auto window = reinterpret_cast<HWND>(hwnd);
    if (!window || !IsWindow(window)) return false;
    // Whatever is registered goes, Qt's target included: this window takes no other drops.
    RevokeDragDrop(window);
    if (FAILED(RegisterDragDrop(window, target_))) return false;
    hwnd_ = hwnd;
    target_->set_window(window);
    return true;
}

IDropTarget *FolderDrop::target() const { return target_; }
}
