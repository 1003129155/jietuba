#pragma once
#include <QByteArrayView>
#include <QString>

namespace jietuba {
// The full package's exe carries onnxruntime.dll as a resource. An update replaces only the exe, and
// the models/ folder it leaves behind may have no DLL (earlier full packages kept ONNX Runtime
// inside their exe), so the DLL is put there from the exe when it is missing or unusable.
namespace bundled_runtime {
// The embedded DLL; empty in a build that has none (the lite package).
QByteArrayView data();

enum class SeedResult { Unneeded, Written, Failed };
// Writes bytes to path unless a usable ONNX Runtime is already there. The file appears whole or
// not at all; *error says why a write failed.
SeedResult seed(QByteArrayView bytes, const QString &path, QString *error = nullptr);
// The embedded DLL to models/onnxruntime.dll.
SeedResult seed_embedded(QString *error = nullptr);
}
}
