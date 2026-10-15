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
    // QVariant::Int / Bool keep the reference's strict type check: a value of the wrong
    // type falls back to its default.
    const auto read_int = [config](const QString &name, int fallback, int low, int high) {
        const auto value = config->get_app_setting("video_record_" + name, fallback);
        bool ok = false;
        const int number = value.toInt(&ok);
        return ok && value.typeId() != QMetaType::Bool && number >= low && number <= high ? number : fallback;
    };
    const auto read_bool = [config](const QString &name, bool fallback) {
        const auto value = config->get_app_setting("video_record_" + name, fallback);
        return value.typeId() == QMetaType::Bool ? value.toBool() : fallback;
    };
    values.fps = read_int("fps", defaults.fps, 1, 60);
    values.bitrate = read_int("bitrate", defaults.bitrate, 500000, 50000000);
    values.system_audio = read_bool("system_audio", defaults.system_audio);
    values.hardware = read_bool("hardware", defaults.hardware);
    values.cursor = read_bool("cursor", defaults.cursor);
    return values;
}

void RecordingOptions::save_to_config(ToolSettingsManager *config) const {
    config->set_app_setting("video_record_fps", fps);
    config->set_app_setting("video_record_bitrate", bitrate);
    config->set_app_setting("video_record_system_audio", system_audio);
    config->set_app_setting("video_record_hardware", hardware);
    config->set_app_setting("video_record_cursor", cursor);
}

QString get_recording_mode(const ToolSettingsManager *config) {
    const auto mode = config->get_app_setting("recording_format", "gif").toString();
    return mode == "gif" || mode == "mp4" ? mode : QString("gif");
}

void set_recording_mode(ToolSettingsManager *config, const QString &mode) {
    if (mode != "gif" && mode != "mp4") throw std::invalid_argument("Recording format must be gif or mp4");
    config->set_app_setting("recording_format", mode);
}
}
