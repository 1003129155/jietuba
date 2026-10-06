"""视频设置边界和落盘兼容性。"""

from dataclasses import replace

import pytest

from gif.video_settings import RecordingOptions, get_recording_mode, set_recording_mode


class Config:
    def __init__(self, values=None):
        self.values = values or {}

    def get_app_setting(self, name, default):
        return self.values.get(name, default)

    def set_app_setting(self, name, value):
        self.values[name] = value


def test_custom_video_options_round_trip_without_changing_gif_settings():
    config = Config({"gif_fps": 12})
    options = RecordingOptions(fps=17, bitrate=1_500_000, system_audio=False, hardware=False, cursor=False)
    options.save_to_config(config)
    set_recording_mode(config, "mp4")
    assert RecordingOptions.from_config(config) == options
    assert get_recording_mode(config) == "mp4"
    assert config.values["gif_fps"] == 12



@pytest.mark.parametrize("name,value", [
    ("fps", 0), ("fps", 61), ("fps", True), ("fps", 29.5),
    ("bitrate", 499999), ("bitrate", 50000001), ("bitrate", "4000000"),
    ("system_audio", "false"), ("hardware", 1), ("cursor", None),
])
def test_invalid_option_is_rejected_but_bad_saved_value_falls_back(name, value):
    with pytest.raises(ValueError):
        replace(RecordingOptions(), **{name: value})
    config = Config({f"video_record_{name}": value})
    assert RecordingOptions.from_config(config) == RecordingOptions()


@pytest.mark.parametrize("fps,bitrate", [(1, 500000), (60, 50000000)])
def test_custom_parameter_boundaries_are_supported(fps, bitrate):
    assert RecordingOptions(fps=fps, bitrate=bitrate).fps == fps


def test_old_or_invalid_format_keeps_gif_mode():
    assert get_recording_mode(Config()) == "gif"
    assert get_recording_mode(Config({"recording_format": "unknown"})) == "gif"
    with pytest.raises(ValueError):
        set_recording_mode(Config(), "unknown")
