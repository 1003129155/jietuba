#pragma once
#include <QString>

// MP4 recording parameters, saved apart from the GIF frame cache settings.
namespace jietuba {
class ToolSettingsManager;

struct RecordingOptions {
    int fps = 30;
    int bitrate = 4000000;
    bool system_audio = true;
    bool hardware = true;
    bool cursor = true;
    // Throws std::invalid_argument when a value is out of range.
    void validate() const;
    // A damaged or outdated single setting falls back alone, leaving the other valid ones.
    static RecordingOptions from_config(const ToolSettingsManager *config);
    void save_to_config(ToolSettingsManager *config) const;
};

QString get_recording_mode(const ToolSettingsManager *config);
// Throws std::invalid_argument for anything but "gif" or "mp4".
void set_recording_mode(ToolSettingsManager *config, const QString &mode);
}
