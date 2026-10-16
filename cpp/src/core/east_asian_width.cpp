#include "core/east_asian_width.h"
#include <algorithm>
#include <iterator>
#include <utility>

namespace jietuba {
namespace {
const std::pair<char32_t, char32_t> WIDE_RANGES[] = {
#include "core/east_asian_width.inc"
};
}

bool is_east_asian_wide(char32_t code_point) {
    // The first range whose end is not below the code point.
    const auto it = std::lower_bound(std::begin(WIDE_RANGES), std::end(WIDE_RANGES), code_point,
                                     [](const std::pair<char32_t, char32_t> &range, char32_t value) { return range.second < value; });
    return it != std::end(WIDE_RANGES) && it->first <= code_point;
}
}
