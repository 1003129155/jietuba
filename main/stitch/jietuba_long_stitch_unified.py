#!/usr/bin/env python3
"""
长截图拼接统一接口
支持 Python哈希识别 版本和 rust特征点位 版本的自动切换
"""

from PIL import Image
from typing import List, Optional
import os
from core import log_debug, log_info, log_warning, log_error

from .jietuba_long_stitch import AllOverlapShrinkError


def normalize_engine_value(value):
    """
    规范化引擎设置值
    将用户输入的各种可能值统一为标准的 'auto', 'rust', 'hash_rust', 'hash_python'
    
    算法说明:
        'auto'        -> 自动选择（优先特征匹配，失败自动回退到哈希匹配Rust版）
        'rust'        -> 强制使用特征点匹配算法（FAST+ORB+HNSW，纯 Rust）
        'hash_rust'   -> 强制使用哈希值匹配算法（纯 Rust LCS，最快）
        'hash_python' -> 强制使用哈希值匹配算法（纯 Python LCS，调试用）
    
    参数:
        value: 引擎设置值（支持多种别名）
    
    返回:
        标准化的引擎值: 'auto', 'rust', 'hash_rust', 'hash_python'
    """
    if not value or not isinstance(value, str):
        return "auto"
    
    value_lower = value.lower().strip()
    
    # 自动模式的各种别名
    if value_lower in ("auto", "automatic", "自动", "自動"):
        return "auto"
    
    # 特征匹配的各种别名
    elif value_lower in (
        "rust", "rs", "rust版本", "rust版",
        "feature", "feature_matching", "特征", "特征匹配",
        "ピクセル特徴", "特徴点"
    ):
        return "rust"
    
    # Rust哈希匹配的各种别名
    elif value_lower in (
        "hash_rust", "hashrust", "rustハッシュ値", "rust_hash",
        "rust哈希", "rust_lcs"
    ):
        return "hash_rust"
    
    # Python哈希匹配的各种别名
    elif value_lower in (
        "hash_python", "hashpython", "pythonハッシュ値", "python_hash",
        "python哈希", "python_lcs",
        "python", "py", "python版本", "python版",
        "hash", "hash_matching", "哈希", "哈希匹配",
        "画像ハッシュ値", "ハッシュ値", "lcs"
    ):
        return "hash_python"
    
    else:
        # 未知值，返回默认值
        return "auto"


class LongStitchConfig:
    """长截图拼接配置"""
    
    # 引擎选择（新命名 - 反映实际算法）
    ENGINE_AUTO = "auto"                    # 自动选择（优先特征匹配）
    ENGINE_FEATURE_MATCHING = "rust"        # 特征点匹配算法（纯 Rust 实现）
    ENGINE_HASH_RUST = "hash_rust"          # 哈希值匹配算法（纯 Rust，最快）
    ENGINE_HASH_PYTHON = "hash_python"      # 哈希值匹配算法（纯 Python，调试）
    
    # 向后兼容的别名（保持旧代码可用）
    ENGINE_RUST = "rust"           # 别名：特征点匹配（纯 Rust）
    ENGINE_PYTHON = "hash_python"  # 别名：哈希值匹配（Python）
    
    def __init__(self):
        # 默认配置
        self.engine = self.ENGINE_AUTO
        
        # 通用参数
        self.direction = 0  # 0=垂直, 1=水平
        self.verbose = True
        self.cancel_on_shrink = False  # 是否在检测到缩短风险时直接取消
        
        # Python 版本参数
        self.ignore_right_pixels = 20  # 忽略右侧像素（滚动条）
        
        # Rust 版本参数
        self.sample_rate = 0.6          # 采样率 (0.0-1.0，提高到0.6增加精度)
        self.min_sample_size = 300      # 最小采样尺寸 (像素)
        self.max_sample_size = 800      # 最大采样尺寸 (像素)
        self.corner_threshold = 30      # 特征点阈值 (越低检测越多特征点)
        self.descriptor_patch_size = 9  # 描述符块大小 (像素)
        self.min_size_delta = 1         # 索引重建阈值 (像素，设为1强制每张都更新)
        self.try_rollback = True        # 是否尝试回滚匹配
        self.distance_threshold = 0.1   # 特征匹配距离阈值 (0.05-0.3，越低越严格)
        self.ef_search = 32             # HNSW搜索参数 (16-128，越高准确率越高但速度越慢)


# 全局配置实例
config = LongStitchConfig()


def set_engine(engine: str):
    """
    设置拼接引擎
    
    参数:
        engine: 引擎类型
            - "auto"   : 自动选择（优先特征匹配）
            - "rust"   : 特征点匹配算法（纯 Rust，FAST+ORB+HNSW）
            - "python" : 哈希值匹配算法（Python/混合，LCS 最长公共子串）
    """
    # 规范化输入
    engine = normalize_engine_value(engine)
    
    if engine not in [LongStitchConfig.ENGINE_AUTO, 
                      LongStitchConfig.ENGINE_RUST, 
                      LongStitchConfig.ENGINE_PYTHON]:
        raise ValueError(f"Invalid engine: {engine}. Must be 'auto', 'rust', or 'python'")
    
    config.engine = engine
    if config.verbose:
        engine_name = {
            "auto": "自动选择",
            "rust": "特征点匹配（Rust）",
            "python": "哈希值匹配（Python/混合）"
        }.get(engine, engine)
        log_info(f"引擎设置为: {engine_name}", module="长截图")


def get_active_engine() -> str:
    """
    获取当前实际激活的引擎类型
    如果设置为 "auto"，则返回实际检测到的引擎（rust 或 python）
    
    返回:
        "rust" 或 "python"
    """
    return _detect_engine()


def configure(
    engine: str = "auto",
    direction: int = 0,
    verbose: bool = True,
    # 哈希匹配算法参数（engine="python"）
    ignore_right_pixels: int = 20,
    # 特征匹配算法参数（engine="rust"）
    sample_rate: float = 0.6,
    min_sample_size: int = 300,
    max_sample_size: int = 800,
    corner_threshold: int = 30,
    descriptor_patch_size: int = 9,
    min_size_delta: int = 1,
    try_rollback: bool = True,
    distance_threshold: float = 0.1,
    ef_search: int = 32,
    cancel_on_shrink: Optional[bool] = None,
):
    """
    配置长截图拼接参数
    
    参数:
        engine: 引擎选择
            - "auto"   : 自动选择（优先特征匹配）
            - "rust"   : 特征点匹配算法（纯 Rust，FAST+ORB+HNSW）
            - "python" : 哈希值匹配算法（Python/混合，LCS）
        direction: 滚动方向 (0=垂直, 1=水平)
        verbose: 是否显示详细信息
        
        # 哈希匹配算法参数（仅 engine="python" 时生效）
        ignore_right_pixels: 忽略右侧像素数（排除滚动条）
        
        # 特征匹配算法参数（仅 engine="rust" 时生效）
        sample_rate: 采样率 (0.0-1.0，越高精度越高但速度越慢)
        min_sample_size: 最小采样尺寸 (像素)
        max_sample_size: 最大采样尺寸 (像素)
        corner_threshold: 特征点阈值 (越低检测越多特征点，推荐10-64)
        descriptor_patch_size: 描述符块大小 (像素，推荐9或11)
        min_size_delta: 索引重建阈值 (像素，设为1强制每张都更新)
        try_rollback: 是否启用回滚检测 (允许在另一个队列中查找)
        distance_threshold: 特征匹配距离阈值 (0.05-0.3，越低越严格)
        ef_search: HNSW搜索参数 (16-128，越高准确率越高但速度越慢)
    """
    config.engine = engine
    config.direction = direction
    config.verbose = verbose
    
    # Python 参数
    config.ignore_right_pixels = ignore_right_pixels
    
    # Rust 参数
    config.sample_rate = sample_rate
    config.min_sample_size = min_sample_size
    config.max_sample_size = max_sample_size
    config.corner_threshold = corner_threshold
    config.descriptor_patch_size = descriptor_patch_size
    config.min_size_delta = min_size_delta
    config.try_rollback = try_rollback
    config.distance_threshold = distance_threshold
    config.ef_search = ef_search
    config.min_size_delta = min_size_delta
    config.try_rollback = try_rollback
    if cancel_on_shrink is not None:
        config.cancel_on_shrink = cancel_on_shrink
    
    if verbose:
        log_info(f"配置已更新: engine={engine}, direction={direction}", module="长截图")


def _detect_engine() -> str:
    """
    检测可用的引擎
    
    返回:
        "rust"        - 特征点匹配算法（Rust FAST+ORB）
        "hash_rust"   - 哈希值匹配算法（Rust LCS）
        "hash_python" - 哈希值匹配算法（Python LCS）
    """
    # 强制指定哈希匹配（Python版）
    if config.engine == LongStitchConfig.ENGINE_HASH_PYTHON:
        return "hash_python"
    # 强制指定哈希匹配（Rust版）
    elif config.engine == LongStitchConfig.ENGINE_HASH_RUST:
        return "hash_rust"
    # 强制指定特征匹配（Rust版）
    elif config.engine == LongStitchConfig.ENGINE_RUST:
        return "rust"
    
    # AUTO 模式：优先尝试特征匹配（Rust）
    try:
        import longstitch
        return "rust"  # 特征点匹配
    except ImportError:
        if config.verbose:
            log_info("特征匹配模块（longstitch）未安装，使用哈希匹配（Rust）", module="长截图")
        return "hash_rust"  # 哈希值匹配（优先Rust）


def stitch_images(images: List[Image.Image]) -> Optional[Image.Image]:
    """
    拼接多张图片（统一接口）
    
    参数:
        images: PIL Image 对象列表
    
    返回:
        拼接后的图片，失败返回 None
    """
    if not images or len(images) == 0:
        if config.verbose:
            log_error("错误: 没有图片需要拼接", module="长截图")
        return None
    
    if len(images) == 1:
        if config.verbose:
            log_debug("只有一张图片，直接返回", module="长截图")
        return images[0]
    
    # 检测使用哪个引擎
    engine = _detect_engine()
    
    if config.verbose:
        engine_name = {
            "rust": "特征点匹配（Rust FAST+ORB）",
            "hash_rust": "哈希值匹配（Rust LCS，快11倍）",
            "hash_python": "哈希值匹配（Python LCS，调试）"
        }.get(engine, engine.upper())
        log_info(f"🚀 使用 {engine_name} 拼接 {len(images)} 张图片", module="长截图")
    
    try:
        if engine == "rust":
            result = _stitch_with_rust(images)
            if result:
                if config.verbose:
                    log_info("[OK] 特征点匹配拼接成功", module="长截图")
                return result
            else:
                # Rust 返回 None（拼接失败）
                if config.verbose:
                    log_warning("特征点匹配返回 None", module="长截图")
                # 如果是 AUTO 模式，尝试回退
                if config.engine == LongStitchConfig.ENGINE_AUTO:
                    if config.verbose:
                        log_warning("🔄 自动回退到哈希匹配算法...", module="长截图")
                    try:
                        result = _stitch_with_hash_rust(images)
                        if result and config.verbose:
                            log_info("[OK] 哈希匹配拼接成功（回退到Rust哈希）", module="长截图")
                        return result
                    except Exception as e2:
                        if config.verbose:
                            log_error(f"[ERROR] 哈希匹配也失败: {e2}", module="长截图")
                        return None
                return None
        elif engine == "hash_rust":
            result = _stitch_with_hash_rust(images)
            if result and config.verbose:
                log_info("[OK] Rust哈希匹配拼接成功", module="长截图")
            return result
        elif engine == "hash_python":
            result = _stitch_with_hash_python(images)
            if result and config.verbose:
                log_info("[OK] Python哈希匹配拼接成功", module="长截图")
            return result
        else:
            # 默认使用hash_python
            result = _stitch_with_hash_python(images)
            if result and config.verbose:
                log_info("[OK] 哈希匹配拼接成功", module="长截图")
            return result
    except AllOverlapShrinkError:
        raise
    except Exception as e:
        if config.verbose:
            algorithm_name = {
                "rust": "特征点匹配",
                "hash_rust": "Rust哈希匹配",
                "hash_python": "Python哈希匹配"
            }.get(engine, "未知算法")
            log_error(f"[ERROR] {algorithm_name}拼接失败: {e}", module="长截图")
        
        # 如果特征匹配失败且是 AUTO 模式，尝试回退到哈希匹配
        if engine == "rust" and config.engine == LongStitchConfig.ENGINE_AUTO:
            if config.verbose:
                log_warning("🔄 自动回退到哈希匹配算法...", module="长截图")
            try:
                result = _stitch_with_hash_rust(images)
                if result and config.verbose:
                    log_info("[OK] 哈希匹配拼接成功（回退）", module="长截图")
                return result
            except Exception as e2:
                if config.verbose:
                    log_error(f"[ERROR] 哈希匹配也失败: {e2}", module="长截图")
                return None
        
        return None


def _stitch_with_rust(images: List[Image.Image]) -> Optional[Image.Image]:
    """使用特征点匹配算法拼接（纯 Rust 实现）"""
    from .jietuba_long_stitch_rust import stitch_pil_images
    
    result = stitch_pil_images(
        images,
        direction=config.direction,
        sample_rate=config.sample_rate,
        min_sample_size=config.min_sample_size,
        max_sample_size=config.max_sample_size,
        corner_threshold=config.corner_threshold,
        descriptor_patch_size=config.descriptor_patch_size,
        min_size_delta=config.min_size_delta,
        try_rollback=config.try_rollback,
        distance_threshold=config.distance_threshold,
        ef_search=config.ef_search,
        verbose=config.verbose,
    )
    
    return result


def _stitch_with_python(images: List[Image.Image]) -> Optional[Image.Image]:
    """使用哈希匹配算法拼接（Python + Rust 混合加速）- 已弃用"""
    # 这个函数保留是为了兼容，实际应该使用 _stitch_with_hash_python
    return _stitch_with_hash_python(images)


def _stitch_with_hash_rust(images: List[Image.Image]) -> Optional[Image.Image]:
    """使用哈希匹配算法拼接"""
    from .jietuba_long_stitch import stitch_images_rust
    
    if len(images) == 0:
        return None
    if len(images) == 1:
        return images[0]
    
    # 逐对拼接
    result = images[0]
    for i in range(1, len(images)):
        result = stitch_images_rust(
            result,
            images[i],
            ignore_right_pixels=config.ignore_right_pixels,
            debug=config.verbose,  # 根据配置决定是否输出调试信息
        )
        if result is None:
            if config.verbose:
                log_warning(f"第{i+1}张图片拼接失败", module="长截图")
            return None
    
    return result


def _stitch_with_hash_python(images: List[Image.Image]) -> Optional[Image.Image]:
    """使用哈希匹配算法拼接（Python LCS，用于调试）"""
    from .jietuba_long_stitch import stitch_images_python
    
    if len(images) == 0:
        return None
    if len(images) == 1:
        return images[0]
    
    # 逐对拼接
    result = images[0]
    for i in range(1, len(images)):
        result = stitch_images_python(
            result,
            images[i],
            ignore_right_pixels=config.ignore_right_pixels,
            debug=config.verbose,  # 根据配置决定是否输出调试信息
            cancel_on_shrink=config.cancel_on_shrink,
        )
        if result is None:
            if config.verbose:
                log_warning(f"第{i+1}张图片拼接失败", module="长截图")
            return None
    
    return result


def stitch_files(
    image_paths: List[str],
    output_path: str,
    **kwargs
) -> bool:
    """
    从文件拼接图片并保存
    
    参数:
        image_paths: 图片文件路径列表
        output_path: 输出文件路径
        **kwargs: 其他配置参数（传递给 configure）
    
    返回:
        True=成功, False=失败
    """
    # 应用配置
    if kwargs:
        configure(**kwargs)
    
    if config.verbose:
        log_info(f"加载 {len(image_paths)} 张图片...", module="长截图")
    
    # 加载图片
    images = []
    for path in image_paths:
        try:
            img = Image.open(path)
            images.append(img)
            if config.verbose:
                log_debug(f"[v] {path} ({img.size})", module="长截图")
        except Exception as e:
            if config.verbose:
                log_error(f"✗ {path}: {e}", module="长截图")
            return False
    
    # 拼接
    result = stitch_images(images)
    
    if result:
        # 保存
        try:
            result.save(output_path, "PNG", quality=95)
            if config.verbose:
                log_info(f"[v] 拼接成功，已保存到: {output_path}", module="长截图")
                log_debug(f"最终尺寸: {result.size}", module="长截图")
            return True
        except Exception as e:
            if config.verbose:
                log_error(f"✗ 保存失败: {e}", module="长截图")
            return False
    else:
        if config.verbose:
            log_error("✗ 拼接失败", module="长截图")
        return False


# 便捷函数（向后兼容）
def stitch_pil_images(
    images: List[Image.Image],
    ignore_right_pixels: int = None,
    direction: int = None,
) -> Optional[Image.Image]:
    """
    向后兼容的接口（自动参数适配）
    
    参数:
        images: PIL Image 对象列表
        ignore_right_pixels: Python 版本参数（可选）
        direction: 方向（可选）
    
    返回:
        拼接后的图片
    """
    # 临时保存配置
    old_direction = config.direction
    old_ignore = config.ignore_right_pixels
    
    try:
        # 应用参数
        if direction is not None:
            config.direction = direction
        if ignore_right_pixels is not None:
            config.ignore_right_pixels = ignore_right_pixels
        
        # 拼接
        return stitch_images(images)
    finally:
        # 恢复配置
        config.direction = old_direction
        config.ignore_right_pixels = old_ignore

