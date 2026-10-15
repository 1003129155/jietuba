#include "stitch/jietuba_long_stitch_unified.h"
#include "core/logger.h"

namespace jietuba::long_stitch {
LongStitchConfig &config() {
    static LongStitchConfig instance;
    return instance;
}

void configure(const std::optional<QString> &engine, std::optional<bool> verbose, std::optional<int> ignore_right_pixels, std::optional<int> ignore_top_pixels) {
    auto &current = config();
    if (engine) current.engine = normalize_engine_value(*engine);
    if (verbose) current.verbose = *verbose;
    if (ignore_right_pixels) current.ignore_right_pixels = *ignore_right_pixels;
    if (ignore_top_pixels) current.ignore_top_pixels = *ignore_top_pixels;
    log_info(T("拼接引擎已配置: engine={engine}, verbose={verbose}, ignore_top_pixels={ignore_top_pixels}",
               {{"engine", current.engine}, {"verbose", current.verbose}, {"ignore_top_pixels", current.ignore_top_pixels}}),
             "LongStitch");
}
}
