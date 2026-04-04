#!/usr/bin/env python3
"""
长截图拼接统一接口

仅保留 Rust 哈希匹配引擎（全 Rust 主链路）
"""

from PIL import Image
from typing import List, Optional

from core import log_debug, log_info, log_warning, log_error

from .jietuba_long_stitch import AllOverlapShrinkError

# 全局拼接计数（用于调试日志展示累计次数）
_stitch_counter = 0


def normalize_engine_value(value):
    """
    规范化引擎设置值

    参数:
        value: 引擎设置值

    返回:
        'hash_rust'
    """
    if not value or not isinstance(value, str):
        return "hash_rust"

    v = value.strip().lower()

    # 其他所有值都映射到 hash_rust
    return "hash_rust"


class LongStitchConfig:
    """长截图拼接配置"""

    # 引擎常量
    ENGINE_AUTO = "hash_rust"
    ENGINE_RUST = "hash_rust"
    ENGINE_HASH_RUST = "hash_rust"

    def __init__(self):
        # 引擎选择
        self.engine = "hash_rust"

        # 拼接方向
        self.direction = "vertical"

        # 哈希算法参数
        self.sample_rate = 2
        self.min_sample_size = 50
        self.max_sample_size = 200
        self.min_overlap = 10
        self.max_overlap_ratio = 0.95
        self.cancel_on_shrink = True

        # 调试开关
        self.verbose = False

        # 忽略边缘像素
        self.ignore_left_pixels = 0
        self.ignore_right_pixels = 0


# 全局配置实例
config = LongStitchConfig()


def configure(
    engine=None,
    direction=None,
    sample_rate=None,
    min_sample_size=None,
    max_sample_size=None,
    min_overlap=None,
    max_overlap_ratio=None,
    cancel_on_shrink=None,
    verbose=None,
    ignore_left_pixels=None,
    ignore_right_pixels=None,
    **_kwargs,
):
    """
    配置拼接引擎参数

    参数:
    engine: 引擎选择 ('hash_rust')
        direction: 拼接方向 ('vertical' / 'horizontal')
        sample_rate: 采样率
        min_sample_size: 最小采样尺寸
        max_sample_size: 最大采样尺寸
        min_overlap: 最小重叠区域
        max_overlap_ratio: 最大重叠比例
        cancel_on_shrink: 拼接缩短时是否取消
        verbose: 是否输出详细日志
        ignore_left_pixels: 左侧忽略像素数
        ignore_right_pixels: 右侧忽略像素数
    """
    if engine is not None:
        config.engine = normalize_engine_value(engine)
    if direction is not None:
        config.direction = direction
    if sample_rate is not None:
        config.sample_rate = sample_rate
    if min_sample_size is not None:
        config.min_sample_size = min_sample_size
    if max_sample_size is not None:
        config.max_sample_size = max_sample_size
    if min_overlap is not None:
        config.min_overlap = min_overlap
    if max_overlap_ratio is not None:
        config.max_overlap_ratio = max_overlap_ratio
    if cancel_on_shrink is not None:
        config.cancel_on_shrink = cancel_on_shrink
    if verbose is not None:
        config.verbose = verbose
    if ignore_left_pixels is not None:
        config.ignore_left_pixels = ignore_left_pixels
    if ignore_right_pixels is not None:
        config.ignore_right_pixels = ignore_right_pixels

    log_info(
        f"拼接引擎已配置: engine={config.engine}, direction={config.direction}, "
        f"sample_rate={config.sample_rate}, min_overlap={config.min_overlap}",
        module="长截图"
    )


def get_active_engine():
    """获取当前活跃的拼接引擎名称"""
    return normalize_engine_value(config.engine)


def get_engine_display_name(engine):
    """获取引擎的显示名称"""
    return {
        "hash_rust": "哈希匹配（Rust LCS）",
    }.get(engine, engine)


def stitch_images(images, engine=None):
    """
    拼接多张图片

    参数:
        images: PIL Image 列表（按顺序排列）
        engine: 可选，指定引擎（不指定则使用全局配置）

    返回:
        拼接后的 PIL Image，失败返回 None

    异常:
        AllOverlapShrinkError: 当连续拼接导致图片缩小时抛出
    """
    if not images or len(images) < 2:
        if images:
            return images[0]
        return None

    active_engine = normalize_engine_value(engine) if engine else get_active_engine()

    return _stitch_with_hash_rust(images)


def _stitch_with_hash_rust(images):
    """使用 Rust 哈希匹配拼接"""
    try:
        import longstitch
        import io

        result = images[0]
        for i in range(1, len(images)):
            try:
                # PIL Image → PNG 字节（PNG压缩后传输量远小于原始RGBA，整体更快）
                buf1 = io.BytesIO()
                result.save(buf1, format="PNG")

                buf2 = io.BytesIO()
                images[i].save(buf2, format="PNG")

                stitch_result = longstitch.stitch_two_images_rust_smart(
                    buf1.getvalue(),
                    buf2.getvalue(),
                    ignore_right_pixels=config.ignore_right_pixels or None,
                )

                if stitch_result is None:
                    log_warning(
                        f"第 {i}/{len(images)-1} 次拼接失败（无重叠）",
                        module="长截图"
                    )
                    return None

                # 返回 PNG 字节，解码为 PIL Image
                result = Image.open(io.BytesIO(stitch_result))

                if config.verbose:
                    global _stitch_counter
                    _stitch_counter += 1
                    log_debug(
                        f"第 {i}/{len(images)-1} 次拼接完成（累计{_stitch_counter}次）: "
                        f"{result.size[0]}x{result.size[1]}",
                        module="长截图"
                    )

            except AllOverlapShrinkError:
                raise
            except Exception as e:
                log_error(f"第 {i} 次拼接异常: {e}", module="长截图")
                return None

        return result

    except ImportError:
        log_warning("longstitch 模块未安装，无法使用 Rust 拼接", module="长截图")
        return None


def stitch_images_auto(img1, img2, debug=False):
    """
    自动方向检测拼接（仅用于第一次拼接时检测方向）

    在 Rust 内部完成正向/反向尝试，避免 Python 层面翻转+重传的开销。

    参数:
        img1: 第一张 PIL Image（已拼接的结果）
        img2: 第二张 PIL Image（新截图）
        debug: 是否开启调试输出

    返回:
        (result_image, direction) 元组
        result_image: 拼接后的 PIL Image，失败返回 None
        direction: "forward" 或 "reverse"
    """
    try:
        import longstitch
        import io

        buf1 = io.BytesIO()
        img1.save(buf1, format="PNG")

        buf2 = io.BytesIO()
        img2.save(buf2, format="PNG")

        if debug:
            auto_result = longstitch.stitch_two_images_rust_smart_auto_debug(
                buf1.getvalue(),
                buf2.getvalue(),
                ignore_right_pixels=config.ignore_right_pixels or None,
            )
        else:
            auto_result = longstitch.stitch_two_images_rust_smart_auto(
                buf1.getvalue(),
                buf2.getvalue(),
                ignore_right_pixels=config.ignore_right_pixels or None,
            )

        if auto_result is None:
            return None, "forward"

        png_bytes, direction = auto_result
        result = Image.open(io.BytesIO(png_bytes))
        return result, direction

    except ImportError:
        log_warning("longstitch 模块未安装，无法使用自动方向检测", module="长截图")
        return None, "forward"
    except Exception as e:
        log_error(f"自动方向检测拼接异常: {e}", module="长截图")
        return None, "forward"


 