#include "core/last_capture_region.h"
namespace jietuba {
namespace { std::optional<QRect> last_region; }
void set_last_region(const QRect &rect) { last_region = rect; }
std::optional<QRect> get_last_region() { return last_region; }
void reset_last_region_for_test() { last_region.reset(); }
}
