#pragma once
#include <QRect>
#include <optional>

namespace jietuba {
// Process memory only, value copies.
void set_last_region(const QRect &rect);
std::optional<QRect> get_last_region();
// Test reset only; no persistence or additional normalization.
void reset_last_region_for_test();
}
