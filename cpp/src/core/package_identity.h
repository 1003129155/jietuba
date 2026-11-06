#pragma once
#include <optional>

namespace jietuba {
// Whether the process runs from an MSIX package (the Microsoft Store build). A packaged app is
// updated by the Store, cannot be elevated through a task, has a read-only install folder, and its
// registry Run entries are not started at sign-in.
bool is_packaged();
// Tests force the answer; nullopt restores the real one.
void set_packaged_for_test(std::optional<bool> packaged);
}
