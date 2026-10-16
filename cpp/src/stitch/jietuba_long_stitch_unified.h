#pragma once
#include <QString>
#include <optional>

// Long-screenshot stitch configuration. Only the Rust hash-matching engine remains.
// The whole-image stitch_images / stitch_images_auto entry points are not part of
// this build: no caller uses them, scrolling capture goes through IncrementalStitcher.
namespace jietuba::long_stitch {
// Normalizes the stored engine setting; always "hash_rust".
inline QString normalize_engine_value(const QString &) { return "hash_rust"; }

struct LongStitchConfig {
    // The only supported engine; the field stays for persistence and display.
    QString engine = "hash_rust";
    bool verbose = false;
    // Pixels ignored at the right (fixed scroll bars or decoration).
    int ignore_right_pixels = 0;
    // Pixels ignored at the top of later frames (fixed title or status bars disturb matching).
    int ignore_top_pixels = 0;
};

// The global configuration.
LongStitchConfig &config();

void configure(const std::optional<QString> &engine = std::nullopt, std::optional<bool> verbose = std::nullopt,
               std::optional<int> ignore_right_pixels = std::nullopt, std::optional<int> ignore_top_pixels = std::nullopt);
}
