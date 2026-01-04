#!/usr/bin/env python3
"""
长截图拼接脚本 - Rust版本
使用 Rust 实现的特征点匹配算法进行高性能图片拼接
"""

from PIL import Image
import io
from typing import List, Optional
import sys
from core import log_debug, log_info, log_warning, log_error

# 模块标签
_MODULE_TAG = "RustStitch"

def _stitch_log(message: str, level: str = "debug"):
    """统一的拼接日志函数"""
    if level == "info":
        log_info(message, module=_MODULE_TAG)
    elif level == "warning":
        log_warning(message, module=_MODULE_TAG)
    elif level == "error":
        log_error(message, module=_MODULE_TAG)
    else:
        log_debug(message, module=_MODULE_TAG)


class RustLongStitch:
    """使用 Rust 算法的长截图拼接类"""

    def __init__(
        self,
        direction: int = 0,  # 0=垂直, 1=水平
        sample_rate: float = 0.5,
        min_sample_size: int = 300,
        max_sample_size: int = 800,
        corner_threshold: int = 64,
        descriptor_patch_size: int = 9,
        min_size_delta: int = 64,
        try_rollback: bool = True,
        distance_threshold: float = 0.1,
        ef_search: int = 32,
    ):
        """
        初始化长截图拼接器

        参数:
            direction: 滚动方向 (0=垂直滚动, 1=水平滚动)
            sample_rate: 采样率 (0.0-1.0，用于缩放图片以加快处理)
            min_sample_size: 最小采样尺寸
            max_sample_size: 最大采样尺寸
            corner_threshold: 特征点检测阈值 (越低检测越多特征点)
            descriptor_patch_size: 特征描述符块大小
            min_size_delta: 最小变化量阈值
            try_rollback: 是否尝试回滚匹配
            distance_threshold: 特征匹配距离阈值 (越低越严格，推荐0.05-0.3)
            ef_search: HNSW搜索参数 (越高准确率越高但速度越慢，推荐16-128)
        """
        try:
            import longstitch
        except ImportError:
            raise ImportError(
                "无法导入 longstitch 模块。请先安装:\n"
                "  pip install longstitch\n"
                "或者从源码编译:\n"
                "  cd rust_libs/longstitch\n"
                "  maturin develop --release"
            )

        self.service = longstitch.PyScrollScreenshotService()
        self.service.init(
            direction,
            sample_rate,
            min_sample_size,
            max_sample_size,
            corner_threshold,
            descriptor_patch_size,
            min_size_delta,
            try_rollback,
            distance_threshold,
            ef_search,
        )
        self.direction = direction
        
        # 保存参数用于调试
        self._corner_threshold = corner_threshold
        self._sample_rate = sample_rate
        self._min_size_delta = min_size_delta
        self._try_rollback = try_rollback
        self._distance_threshold = distance_threshold
        self._ef_search = ef_search

    def add_image(self, image: Image.Image, direction: int = 1, debug: bool = True) -> Optional[int]:
        """
        添加一张图片到拼接队列

        参数:
            image: PIL Image 对象
            direction: 0=上/左图片列表, 1=下/右图片列表
            debug: 是否打印调试信息

        返回:
            重叠尺寸 (像素)，如果未找到重叠则返回 None
        """
        # 将 PIL Image 转换为字节
        buffer = io.BytesIO()
        image.save(buffer, format="PNG")
        image_bytes = buffer.getvalue()

        if debug:
            # 获取添加前的状态
            top_count_before, bottom_count_before = self.get_image_count()
            direction_name = "Top/Left" if direction == 0 else "Bottom/Right"
            log_debug(f"添加图片到 {direction_name} 列表", module="RustStitch")
            log_debug(f"图片尺寸: {image.size}", module="RustStitch")
            log_debug(f"字节大小: {len(image_bytes):,} bytes", module="RustStitch")
            log_debug(f"添加前状态: top={top_count_before}, bottom={bottom_count_before}", module="RustStitch")

        # 调用 Rust 接口
        overlap_size, is_rollback, result_direction = self.service.add_image(
            image_bytes, direction
        )

        if debug:
            # 获取添加后的状态
            top_count_after, bottom_count_after = self.get_image_count()
            result_dir_name = "Top/Left" if result_direction == 0 else "Bottom/Right"
            
            log_debug(f"添加后状态: top={top_count_after}, bottom={bottom_count_after}", module="RustStitch")
            log_debug(f"实际添加到: {result_dir_name} 列表", module="RustStitch")
            
            # 详细分析
            if is_rollback:
                log_debug("发生回滚:", module="RustStitch")
                log_debug(f"在 {direction_name} 列表中未找到匹配", module="RustStitch")
                log_debug("回滚到另一个列表查找", module="RustStitch")
                if overlap_size is not None:
                    log_debug(f"回滚后找到重叠: {overlap_size} 像素", module="RustStitch")
                else:
                    log_debug("回滚后仍未找到匹配", module="RustStitch")
            
            if overlap_size is not None:
                overlap_percent = (overlap_size / image.size[1 if self.direction == 0 else 0]) * 100
                log_debug("找到重叠区域", module="RustStitch")
                log_debug(f"重叠尺寸: {overlap_size} 像素", module="RustStitch")
                log_debug(f"重叠比例: {overlap_percent:.1f}%", module="RustStitch")
                log_debug("特征点匹配成功", module="RustStitch")
            else:
                log_warning("未找到重叠区域", module="RustStitch")
        return overlap_size

    def export(self) -> Optional[Image.Image]:
        """
        导出最终合成的长截图

        返回:
            PIL Image 对象，如果没有图片则返回 None
        """
        result_bytes = self.service.export()

        if result_bytes is None:
            return None

        # 将字节转换为 PIL Image
        return Image.open(io.BytesIO(result_bytes))

    def clear(self):
        """清除所有已添加的图片"""
        self.service.clear()

    def get_image_count(self) -> tuple:
        """
        获取当前图片数量

        返回:
            (top_count, bottom_count) 元组
        """
        return self.service.get_image_count()


def stitch_pil_images(
    images: List[Image.Image],
    direction: int = 0,
    sample_rate: float = 0.6,
    min_sample_size: int = 300,
    max_sample_size: int = 800,
    corner_threshold: int = 30,
    descriptor_patch_size: int = 9,
    min_size_delta: int = 1,
    try_rollback: bool = True,
    distance_threshold: float = 0.1,
    ef_search: int = 32,
    verbose: bool = True,
) -> Optional[Image.Image]:
    """
    拼接多张PIL图片对象（兼容原有接口）

    参数:
        images: PIL Image对象列表
        direction: 滚动方向 (0=垂直, 1=水平)
        sample_rate: 采样率，控制特征提取的图片缩放比例 (0.0-1.0)
        min_sample_size: 最小采样尺寸 (像素)
        max_sample_size: 最大采样尺寸 (像素)
        corner_threshold: 特征点阈值（越低检测越多特征点，推荐10-64）
        descriptor_patch_size: 特征描述符块大小 (像素)
        min_size_delta: 索引重建阈值（像素），设为1强制每张都更新
        try_rollback: 是否尝试回滚匹配
        distance_threshold: 特征匹配距离阈值 (0.05-0.3，越低越严格)
        ef_search: HNSW搜索参数 (16-128，越高准确率越高但速度越慢)
        verbose: 是否输出详细信息

    返回:
        拼接后的PIL Image对象，失败返回None
    """
    if not images or len(images) == 0:
        if verbose:
            log_error("没有图片需要拼接", module="RustStitch")
        return None

    if len(images) == 1:
        if verbose:
            _stitch_log("只有一张图片，直接返回")
        return images[0]

    if verbose:
        _stitch_log(f"{'='*60}")
        _stitch_log(f"🦀 Rust 长截图拼接引擎")
        _stitch_log(f"{'='*60}")
        _stitch_log(f"开始拼接 {len(images)} 张图片")
        _stitch_log(f"📋 参数配置:")
        _stitch_log(f"   滚动方向: {'垂直 ↕️' if direction == 0 else '水平 ↔️'}")
        _stitch_log(f"   采样率: {sample_rate} (图片缩放比例)")
        _stitch_log(f"   采样尺寸范围: {min_sample_size} - {max_sample_size} 像素")
        _stitch_log(f"   特征点阈值: {corner_threshold} (越低=越多特征点)")
        _stitch_log(f"   描述符块大小: {descriptor_patch_size} 像素")
        _stitch_log(f"   索引重建阈值: {min_size_delta} 像素")
        _stitch_log(f"   回滚匹配: {'启用' if try_rollback else '禁用'}")
        _stitch_log(f"   距离阈值: {distance_threshold} (越低=越严格)")
        _stitch_log(f"   HNSW搜索参数: {ef_search} (越高=越准确)")
        _stitch_log(f"{'='*60}")

    try:
        # 创建拼接器
        stitcher = RustLongStitch(
            direction=direction,
            sample_rate=sample_rate,
            min_sample_size=min_sample_size,
            max_sample_size=max_sample_size,
            corner_threshold=corner_threshold,
            descriptor_patch_size=descriptor_patch_size,
            min_size_delta=min_size_delta,
            try_rollback=try_rollback,
            distance_threshold=distance_threshold,
            ef_search=ef_search,
        )

        # 添加所有图片
        has_failure = False  # 🆕 标记是否有图片失败
        success_count = 0
        fail_count = 0
        
        for i, img in enumerate(images):
            if verbose:
                _stitch_log(f"{'='*60}")
                _stitch_log(f"处理第 {i+1}/{len(images)} 张图片: {img.size}")
                _stitch_log(f"{'='*60}")

            # 向下滚动：所有图片都用 direction=1 (Bottom)
            # 第1张:添加到bottom,建立top_index
            # 第2张:在bottom_index中查找失败 → 回滚到top_index查找成功 → 添加到bottom
            overlap = stitcher.add_image(img, direction=1, debug=verbose)
            
            # 🆕 检测添加是否失败（除第一张外）
            if i > 0 and overlap is None:
                has_failure = True
                fail_count += 1
                if verbose:
                    _stitch_log(f"❌ 第 {i+1} 张图片添加失败!", "warning")
                    _stitch_log(f"   累计成功: {success_count}/{i}")
                    _stitch_log(f"   累计失败: {fail_count}")
            elif i > 0:
                success_count += 1

            top_count, bottom_count = stitcher.get_image_count()
            if verbose:
                _stitch_log(f"📊 当前状态汇总:")
                _stitch_log(f"   队列: top={top_count}, bottom={bottom_count}")
                _stitch_log(f"   成功率: {success_count}/{max(1, i)} = {success_count/max(1, i)*100:.1f}%")

        # 🆕 如果有图片失败，直接返回 None 触发引擎切换
        if has_failure:
            if verbose:
                _stitch_log(f"{'='*60}", "warning")
                _stitch_log(f"❌ 拼接失败总结", "warning")
                _stitch_log(f"{'='*60}", "warning")
                _stitch_log(f"总图片数: {len(images)}", "warning")
                _stitch_log(f"成功: {success_count}", "warning")
                _stitch_log(f"失败: {fail_count}", "warning")
                _stitch_log(f"成功率: {success_count/(len(images)-1)*100:.1f}%", "warning")
                _stitch_log(f"🔄 系统将自动切换到 Python 哈希引擎（基于像素哈希，更鲁棒）...", "warning")
                _stitch_log(f"{'='*60}", "warning")
            return None

        # 导出结果
        if verbose:
            _stitch_log(f"{'='*60}")
            _stitch_log(f"🎨 正在合成最终图片...")
            _stitch_log(f"{'='*60}")

        result = stitcher.export()

        if result:
            if verbose:
                _stitch_log(f"✅ 拼接完成!", "info")
                _stitch_log(f"📊 最终统计:", "info")
                _stitch_log(f"   输入图片: {len(images)} 张", "info")
                _stitch_log(f"   成功拼接: {success_count} 处", "info")
                _stitch_log(f"   最终尺寸: {result.size[0]} x {result.size[1]} 像素", "info")
                _stitch_log(f"   成功率: {success_count/(len(images)-1)*100:.1f}%", "info")
                _stitch_log(f"{'='*60}", "info")
            return result
        else:
            if verbose:
                _stitch_log(f"❌ 拼接失败: 无法生成结果", "error")
                _stitch_log(f"   可能原因: Rust 引擎内部错误", "error")
            return None

    except Exception as e:
        if verbose:
            _stitch_log(f"拼接过程出错: {e}", "error")
        import traceback
        traceback.print_exc()
        return None


def stitch_multiple_images(
    image_paths: List[str],
    output_path: str,
    direction: int = 0,
    sample_rate: float = 0.5,
) -> None:
    """
    从文件路径拼接多张图片并保存

    参数:
        image_paths: 图片文件路径列表
        output_path: 输出文件路径
        direction: 滚动方向 (0=垂直, 1=水平)
        sample_rate: 采样率
    """
    if len(image_paths) < 2:
        _stitch_log("至少需要两张图片进行拼接", "warning")
        return

    _stitch_log(f"加载 {len(image_paths)} 张图片...", "info")

    # 加载所有图片
    images = []
    for path in image_paths:
        try:
            img = Image.open(path)
            images.append(img)
            _stitch_log(f"  加载: {path} ({img.size})")
        except Exception as e:
            _stitch_log(f"  错误: 无法加载 {path}: {e}", "error")
            return

    # 拼接图片
    result = stitch_pil_images(images, direction=direction, sample_rate=sample_rate)

    if result:
        # 保存结果
        result.save(output_path, "PNG", quality=95)
        _stitch_log(f"结果已保存到: {output_path}", "info")
        _stitch_log(f"最终尺寸: {result.size}", "info")
    else:
        _stitch_log("拼接失败", "error")


# 示例用法
if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(
        description="长截图拼接工具 - Rust 加速版本",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例用法:
  python jietuba_long_stitch_rust.py image1.png image2.png image3.png -o output.png
  python jietuba_long_stitch_rust.py *.png -o result.png --horizontal
  python jietuba_long_stitch_rust.py img*.jpg -o long.png --sample-rate 0.3
        """,
    )

    parser.add_argument("images", nargs="+", help="要拼接的图片文件路径")
    parser.add_argument("-o", "--output", required=True, help="输出文件路径")
    parser.add_argument(
        "--horizontal",
        action="store_true",
        help="水平拼接（默认为垂直拼接）",
    )
    parser.add_argument(
        "--sample-rate",
        type=float,
        default=0.5,
        help="采样率 (0.0-1.0，默认0.5)",
    )

    args = parser.parse_args()

    direction = 1 if args.horizontal else 0

    try:
        stitch_multiple_images(
            args.images,
            args.output,
            direction=direction,
            sample_rate=args.sample_rate,
        )
    except KeyboardInterrupt:
        _stitch_log("操作已取消", "warning")
        sys.exit(1)
    except Exception as e:
        _stitch_log(f"错误: {e}", "error")
        import traceback
        traceback.print_exc()
        sys.exit(1)
