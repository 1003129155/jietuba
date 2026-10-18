#include "ocr/bundled_runtime.h"
#include "ocr/ocr_manager.h"
#include "ocr/ppocr_models.h"
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <windows.h>

namespace jietuba::bundled_runtime {
QByteArrayView data() {
    static const QByteArrayView view = [] {
        HRSRC resource = FindResourceW(nullptr, L"ORT_RUNTIME", RT_RCDATA);
        HGLOBAL loaded = resource ? LoadResource(nullptr, resource) : nullptr;
        const void *bytes = loaded ? LockResource(loaded) : nullptr;
        const DWORD size = resource ? SizeofResource(nullptr, resource) : 0;
        return bytes && size ? QByteArrayView(static_cast<const char *>(bytes), static_cast<qsizetype>(size)) : QByteArrayView();
    }();
    return view;
}

SeedResult seed(QByteArrayView bytes, const QString &path, QString *error) {
    if (bytes.isEmpty() || (QFileInfo::exists(path) && ppocr_runtime_supported(path))) return SeedResult::Unneeded;
    auto fail = [error](const QString &reason) {
        if (error) *error = reason;
        return SeedResult::Failed;
    };
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return fail("Cannot create " + QFileInfo(path).absolutePath());
    // QSaveFile renames a finished temporary file over the target, so a reader never sees half a DLL.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return fail(file.errorString());
    if (file.write(bytes.data(), bytes.size()) != bytes.size()) return fail(file.errorString());
    if (!file.commit()) return fail(file.errorString());
    return SeedResult::Written;
}

SeedResult seed_embedded(QString *error) { return seed(data(), ppocr_models::runtime_path(), error); }
}
