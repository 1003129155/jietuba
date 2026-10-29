#pragma once
#include <QImage>
#include <QRect>
#include <atomic>

namespace jietuba {
// Which reconstruction filled the hole; the values are jb_smart_erase's.
enum class SmartEraseSolver { Unchanged = 0, Surface = 1, Periodic = 2, Membrane = 3, Patches = 4 };

struct SmartEraseResult {
    // rect of the source with the masked pixels replaced; null when cancelled, when the
    // arguments do not fit, or when nothing around the hole can be sampled.
    QImage image;
    SmartEraseSolver solver = SmartEraseSolver::Unchanged;
};

// Fills the pixels of rect in source where mask (rect's size) is nonzero from their
// surroundings. Safe on any thread; cancelled may be null.
SmartEraseResult smart_erase(const QImage &source, const QRect &rect, const QImage &mask, const std::atomic_bool *cancelled = nullptr);
}
