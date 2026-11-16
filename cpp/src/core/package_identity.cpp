#include "core/package_identity.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <appmodel.h>

namespace jietuba {
namespace {
std::optional<bool> &forced() {
    static std::optional<bool> value;
    return value;
}

bool query_packaged() {
    UINT32 length = 0;
    return GetCurrentPackageFullName(&length, nullptr) != APPMODEL_ERROR_NO_PACKAGE;
}
}

bool is_packaged() {
    if (forced()) return *forced();
    static const bool packaged = query_packaged();
    return packaged;
}

void set_packaged_for_test(std::optional<bool> packaged) { forced() = packaged; }
}
