#!/usr/bin/env python3
"""
长截图拼接统一接口

仅保留 Rust 哈希匹配引擎（全 Rust 主链路）
"""

from PIL import Image

from core import log_debug, log_info, log_warning, log_error

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
    return "hash_rust"


class LongStitchConfig:
    """长截图拼接配置"""

    def __init__(self):
        # 引擎选择（当前唯一支持 'hash_rust'，字段保留用于持久化/展示）
        self.engine = "hash_rust"

        # 调试开关
        self.verbose = False

        # 忽略右侧像素（部分应用右侧有固定滚动条/装饰时可跳过匹配区域）
        self.ignore_right_pixels = 0

        # 忽略后续截图顶部像素（固定标题栏/状态栏会干扰重叠匹配）
        self.ignore_top_pixels = 0


# 全局配置实例
config = LongStitchConfig()


def configure(engine=None, verbose=None, ignore_right_pixels=None, ignore_top_pixels=None, **_kwargs):
    """
    配置拼接引擎参数

    参数:
        engine: 引擎选择（当前唯一支持 'hash_rust'）
        verbose: 是否输出详细日志
        ignore_right_pixels: 右侧忽略像素数
        ignore_top_pixels: 后续截图顶部忽略像素数
    """
    if engine is not None:
        config.engine = normalize_engine_value(engine)
    if verbose is not None:
        config.verbose = verbose
    if ignore_right_pixels is not None:
        config.ignore_right_pixels = ignore_right_pixels
    if ignore_top_pixels is not None:
        config.ignore_top_pixels = ignore_top_pixels

    log_info(
        f"拼接引擎已配置: engine={config.engine}, verbose={config.verbose}, "
        f"ignore_top_pixels={config.ignore_top_pixels}",
        module="长截图"
    )


def stitch_images(images, ignore_img1_top_ratio=0.0, ignore_img1_bottom_ratio=0.0):
    """
    拼接多张图片

    参数:
        images: PIL Image 列表（按顺序排列）
        ignore_img1_top_ratio: 忽略 img1 顶部比例（下滑正常态用，排除固定标题栏）
        ignore_img1_bottom_ratio: 忽略 img1 底部比例（上滑翻转态用，翻转后标题栏在底部）

    返回:
        拼接后的 PIL Image，失败返回 None
    """
    if not images or len(images) < 2:
        if images:
            return images[0]
        return None

    return _stitch_with_hash_rust(images, ignore_img1_top_ratio, ignore_img1_bottom_ratio)


def _stitch_with_hash_rust(images, ignore_img1_top_ratio=0.0, ignore_img1_bottom_ratio=0.0):
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
                    ignore_top_pixels=config.ignore_top_pixels,
                    ignore_img1_top_ratio=ignore_img1_top_ratio or None,
                    ignore_img1_bottom_ratio=ignore_img1_bottom_ratio or None,
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
                ignore_top_pixels=config.ignore_top_pixels,
            )
        else:
            auto_result = longstitch.stitch_two_images_rust_smart_auto(
                buf1.getvalue(),
                buf2.getvalue(),
                ignore_right_pixels=config.ignore_right_pixels or None,
                ignore_top_pixels=config.ignore_top_pixels,
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


 
