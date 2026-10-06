"""MP4 录制参数；与 GIF 帧缓存设置分开保存。"""

from dataclasses import dataclass, fields


@dataclass(frozen=True)
class RecordingOptions:
    fps: int = 30
    bitrate: int = 4_000_000
    system_audio: bool = True
    hardware: bool = True
    cursor: bool = True

    def __post_init__(self):
        if type(self.fps) is not int or not 1 <= self.fps <= 60:
            raise ValueError("Recording FPS must be between 1 and 60")
        if type(self.bitrate) is not int or not 500_000 <= self.bitrate <= 50_000_000:
            raise ValueError("Video bitrate must be between 500000 and 50000000 bits/s")
        for name in ("system_audio", "hardware", "cursor"):
            if type(getattr(self, name)) is not bool:
                raise ValueError(f"{name} must be a boolean")

    @classmethod
    def from_config(cls, config):
        defaults = cls()
        values = {}
        for field in fields(cls):
            default = getattr(defaults, field.name)
            # 损坏或旧类型的配置单项回退，不改变其它有效选项。
            try:
                value = config.get_app_setting(f"video_record_{field.name}", default)
                cls(**{field.name: value})
            except (TypeError, ValueError):
                value = default
            values[field.name] = value
        return cls(**values)

    def save_to_config(self, config):
        for field in fields(self):
            config.set_app_setting(f"video_record_{field.name}", getattr(self, field.name))

def get_recording_mode(config):
    mode = config.get_app_setting("recording_format", "gif")
    return mode if mode in ("gif", "mp4") else "gif"


def set_recording_mode(config, mode):
    if mode not in ("gif", "mp4"):
        raise ValueError("Recording format must be gif or mp4")
    config.set_app_setting("recording_format", mode)
