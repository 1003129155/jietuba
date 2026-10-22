#include "gif/video_settings.h"
#include "settings/tool_settings.h"
#include <stdexcept>

namespace jietuba {
void RecordingOptions::validate() const {
    if (fps < 1 || fps > 60) throw std::invalid_argument("Recording FPS must be between 1 and 60");
    if (bitrate < 500000 || bitrate > 50000000) throw std::invalid_argument("Video bitrate must be between 500000 and 50000000 bits/s");
}

RecordingOptions RecordingOptions::from_config(const ToolSettingsManager *config) {
    const RecordingOptions defaults;
    RecordingOptions values;
    // Out of range, unreadable text included (it reads as 0), falls back to the default.
    const auto in_range = [](int value, int fallback, int low, int high) { return value >= low && value <= high ? value : fallback; };
    values.fps = in_range(config->get_video_record_fps(defaults.fps), defaults.fps, 1, 60);
    values.bitrate = in_range(config->get_video_record_bitrate(defaults.bitrate), defaults.bitrate, 500000, 50000000);
    values.system_audio = config->get_video_record_system_audio(defaults.system_audio);
    values.hardware = config->get_video_record_hardware(defaults.hardware);
    values.cursor = config->get_video_record_cursor(defaults.cursor);
    return values;
}

void RecordingOptions::save_to_config(ToolSettingsManager *config) const {
    config->set_video_record_fps(fps);
    config->set_video_record_bitrate(bitrate);
    config->set_video_record_system_audio(system_audio);
    config->set_video_record_hardware(hardware);
    config->set_video_record_cursor(cursor);
}

QString get_recording_mode(const ToolSettingsManager *config) {
    const auto mode = config->get_recording_format();
    return mode == "gif" || mode == "mp4" ? mode : QString("gif");
}

void set_recording_mode(ToolSettingsManager *config, const QString &mode) {
    if (mode != "gif" && mode != "mp4") throw std::invalid_argument("Recording format must be gif or mp4");
    config->set_recording_format(mode);
}
}
