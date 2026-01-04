# -*- coding: utf-8 -*-
"""
ocr_manager.py - OCR 功能模块

为截图工具提供 OCR 文字识别功能。
支持多种 OCR 引擎：
- ocr_rs: 基于 PaddleOCR + MNN 的高性能 OCR (需要模型文件，~100MB)
- windows_media_ocr: Windows 系统自带 OCR API (轻量级，仅几MB)

主要功能:
- 识别截图区域的文字
- 支持中英日文识别
- 单例模式管理 OCR 引擎
- 支持图像预处理(灰度转换、图像放大)
- 支持引擎切换

依赖:
- ocr_rs: pip install ocr_rs-2.0.1-cp39-cp39-win_amd64.whl
- windows_media_ocr: pip install windows_media_ocr
"""
from PyQt6.QtGui import QPixmap, QImage
from PyQt6.QtCore import QBuffer, QIODevice, Qt
from typing import Optional, Dict, Any
import io
import time
import os

# 尝试导入 ocr_rs
try:
    from ocr_rs import OcrEngine
    import ocr_rs
    OCR_RS_AVAILABLE = True
    print(f"✅ [OCR] ocr_rs 模块加载成功，版本: {ocr_rs.__version__}")
except ImportError as e:
    print(f"⚠️ [OCR] ocr_rs 模块导入失败: {e}")
    OCR_RS_AVAILABLE = False
    OcrEngine = None

# 尝试导入 windows_media_ocr
try:
    import windows_media_ocr
    WINDOWS_OCR_AVAILABLE = True
    print("✅ [OCR] windows_media_ocr 模块加载成功")
    try:
        available_langs = windows_media_ocr.get_available_languages()
        print(f"📖 [OCR] Windows OCR 支持的语言: {available_langs}")
    except Exception:
        available_langs = []
except ImportError as e:
    print(f"⚠️ [OCR] windows_media_ocr 模块导入失败: {e}")
    WINDOWS_OCR_AVAILABLE = False
    windows_media_ocr = None
    available_langs = []

# 至少有一个引擎可用
OCR_AVAILABLE = OCR_RS_AVAILABLE or WINDOWS_OCR_AVAILABLE


class OCRManager:
    """OCR 管理器 - 单例模式，支持多引擎切换"""
    
    _instance = None
    _initialized = False
    
    # OCR 引擎类型常量
    ENGINE_OCR_RS = "ocr_rs"
    ENGINE_WINDOWS_OCR = "windows_media_ocr"
    
    # 模型路径配置 (用于 ocr_rs)
    MODEL_DIR = "models"
    DET_MODEL = "PP-OCRv5_mobile_det.mnn"
    REC_MODEL = "PP-OCRv5_mobile_rec.mnn"
    CHARSET_FILE = "ppocr_keys_v5.txt"
    
    # 语言映射：应用语言 -> windows_media_ocr 语言代码
    LANGUAGE_MAP = {
        "日本語": "ja",
        "Japanese": "ja",
        "ja": "ja",
        "中文": "zh-Hans-CN",
        "Chinese": "zh-Hans-CN",
        "zh": "zh-Hans-CN",
        "English": "en-US",
        "英语": "en-US",
        "en": "en-US",
    }
    
    def __new__(cls):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
        return cls._instance
    
    def __init__(self):
        """初始化 OCR 管理器"""
        if not self._initialized:
            self._initialized = True
            self._last_error = None
            self._current_engine = None  # 当前使用的引擎类型
            self._ocr_rs_engine = None   # ocr_rs 引擎实例
            self._windows_ocr_language = None  # windows_media_ocr 语言设置
    
    @property
    def is_available(self) -> bool:
        """检查 OCR 功能是否可用"""
        return OCR_AVAILABLE
    
    def get_available_engines(self) -> list:
        """获取可用的 OCR 引擎列表"""
        engines = []
        if OCR_RS_AVAILABLE:
            engines.append(self.ENGINE_OCR_RS)
        if WINDOWS_OCR_AVAILABLE:
            engines.append(self.ENGINE_WINDOWS_OCR)
        return engines
    
    def set_engine(self, engine_type: str):
        """
        设置当前使用的 OCR 引擎
        
        Args:
            engine_type: 引擎类型 ("ocr_rs" 或 "windows_media_ocr")
        """
        if engine_type == self.ENGINE_OCR_RS and not OCR_RS_AVAILABLE:
            print(f"⚠️ [OCR] ocr_rs 引擎不可用")
            return False
        
        if engine_type == self.ENGINE_WINDOWS_OCR and not WINDOWS_OCR_AVAILABLE:
            print(f"⚠️ [OCR] windows_media_ocr 引擎不可用")
            return False
        
        if self._current_engine != engine_type:
            print(f"🔄 [OCR] 切换引擎: {self._current_engine} -> {engine_type}")
            self._current_engine = engine_type
            return True
        
        return True
    
    def get_current_engine(self) -> Optional[str]:
        """获取当前使用的引擎类型"""
        return self._current_engine
    
    def _get_model_path(self, filename: str) -> str:
        """获取模型文件路径 (用于 ocr_rs)"""
        possible_paths = [
            os.path.join(self.MODEL_DIR, filename),
            os.path.join(os.path.dirname(__file__), "..", "..", self.MODEL_DIR, filename),
            os.path.join(os.path.dirname(__file__), self.MODEL_DIR, filename),
        ]
        
        for path in possible_paths:
            abs_path = os.path.abspath(path)
            if os.path.exists(abs_path):
                return abs_path
        
        return os.path.join(self.MODEL_DIR, filename)
    
    def initialize(self, language: str = "日本語", engine_type: Optional[str] = None) -> bool:
        """
        初始化 OCR 引擎
        
        Args:
            language: 识别语言
            engine_type: 指定引擎类型，如果为 None 则使用当前引擎
        
        Returns:
            bool: 是否初始化成功
        """
        if not OCR_AVAILABLE:
            self._last_error = "没有可用的 OCR 引擎"
            return False
        
        # 如果指定了引擎，切换到该引擎
        if engine_type:
            self.set_engine(engine_type)
        
        # 如果没有设置当前引擎，自动选择第一个可用引擎
        if not self._current_engine:
            available = self.get_available_engines()
            if not available:
                self._last_error = "没有可用的 OCR 引擎"
                return False
            self._current_engine = available[0]
            print(f"📖 [OCR] 自动选择引擎: {self._current_engine}")
        
        # 根据引擎类型初始化
        if self._current_engine == self.ENGINE_OCR_RS:
            return self._initialize_ocr_rs()
        elif self._current_engine == self.ENGINE_WINDOWS_OCR:
            return self._initialize_windows_ocr(language)
        else:
            self._last_error = f"未知的引擎类型: {self._current_engine}"
            return False
    
    def _initialize_ocr_rs(self) -> bool:
        """初始化 ocr_rs 引擎"""
        if not OCR_RS_AVAILABLE:
            self._last_error = "ocr_rs 模块不可用"
            return False
        
        # 如果引擎已经初始化，直接返回成功
        if self._ocr_rs_engine is not None:
            print("📖 [OCR] ocr_rs 引擎已初始化")
            return True
        
        try:
            print(f"📖 [OCR] 初始化 ocr_rs 引擎...")
            
            det_path = self._get_model_path(self.DET_MODEL)
            rec_path = self._get_model_path(self.REC_MODEL)
            charset_path = self._get_model_path(self.CHARSET_FILE)
            
            print(f"   检测模型: {det_path}")
            print(f"   识别模型: {rec_path}")
            print(f"   字符集: {charset_path}")
            
            # 🔧 办公电脑优化配置：最小化资源占用（~120MB 内存，低 CPU 负载）
            # - num_threads: 2 (双线程，适合办公环境，降低 CPU 占用)
            # - batch_size: 1 (单张推理，最小化内存)
            # - max_side_len: 640 (降低图像尺寸，减少计算量和内存，适合办公文档/截图)
            # 注意：640 适合常见办公场景，如果需要识别高分辨率图片中的小字，可改回 960
            self._ocr_rs_engine = OcrEngine(
                det_model_path=det_path,
                rec_model_path=rec_path,
                charset_path=charset_path,
                num_threads=2,          # 双线程，平衡性能与资源占用
                max_side_len=640,       # 640px，办公环境推荐值，降低内存和 CPU
                box_threshold=0.3,
                min_score=0.3,
                batch_size=1            # 单张推理，最小化内存
            )
            
            print("✅ [OCR] ocr_rs 引擎初始化成功")
            return True
            
        except Exception as e:
            self._last_error = f"ocr_rs 初始化失败: {str(e)}"
            print(f"❌ [OCR] {self._last_error}")
            import traceback
            traceback.print_exc()
            return False
    
    def _initialize_windows_ocr(self, language: str) -> bool:
        """初始化 windows_media_ocr 引擎"""
        if not WINDOWS_OCR_AVAILABLE:
            self._last_error = "windows_media_ocr 模块不可用"
            return False
        
        try:
            # 映射语言代码
            self._windows_ocr_language = self.LANGUAGE_MAP.get(language, "zh-Hans-CN")
            print(f"📖 [OCR] 初始化 windows_media_ocr 引擎(语言配置: {language} -> {self._windows_ocr_language})")
            print("✅ [OCR] windows_media_ocr 引擎初始化成功")
            return True
            
        except Exception as e:
            self._last_error = f"windows_media_ocr 初始化失败: {str(e)}"
            print(f"❌ [OCR] {self._last_error}")
            import traceback
            traceback.print_exc()
            return False
    
    def recognize_pixmap(
        self, 
        pixmap: QPixmap, 
        return_format: str = "dict"
    ) -> Any:
        """
        识别 QPixmap 图像中的文字
        
        Args:
            pixmap: QPixmap 图像对象
            return_format: 返回格式 ("text", "list", "dict")
        
        Returns:
            识别结果(格式取决于 return_format)
        """
        # 确保引擎已初始化
        if not self._current_engine:
            if not self.initialize():
                return self._format_error(return_format)
        
        # 根据当前引擎调用对应的识别方法
        if self._current_engine == self.ENGINE_OCR_RS:
            return self._recognize_with_ocr_rs(pixmap, return_format)
        elif self._current_engine == self.ENGINE_WINDOWS_OCR:
            return self._recognize_with_windows_ocr(pixmap, return_format
            )
        else:
            return self._format_error(return_format, f"未知的引擎: {self._current_engine}")
    
    def _recognize_with_ocr_rs(
        self,
        pixmap: QPixmap,
        return_format: str
    ) -> Any:
        """使用 ocr_rs 引擎识别"""
        # 确保 ocr_rs 引擎已初始化
        if self._ocr_rs_engine is None:
            if not self._initialize_ocr_rs():
                return self._format_error(return_format)
        
        try:
            start_time = time.time()
            
            # 直接转换为 bytes
            image_bytes = self._pixmap_to_bytes(pixmap)
            
            # 调用 ocr_rs 识别
            results = self._ocr_rs_engine.recognize_from_bytes(image_bytes)
            
            elapse = time.time() - start_time
            
            # 检查识别结果
            if results is None or len(results) == 0:
                return self._format_empty_result(return_format)
            
            # 构建结果列表：[[box, text, score], ...]
            ocr_results = []
            for item in results:
                bbox = item['bbox']
                x, y, w, h = bbox['x'], bbox['y'], bbox['width'], bbox['height']
                
                # 构建 box: [[x1,y1], [x2,y2], [x3,y3], [x4,y4]]
                box = [
                    [x, y],
                    [x + w, y],
                    [x + w, y + h],
                    [x, y + h]
                ]
                text = item['text']
                score = item['confidence']
                ocr_results.append([box, text, score])
            
            # 格式化输出
            return self._format_result(ocr_results, return_format, elapse)
                
        except Exception as e:
            error_msg = f"ocr_rs 识别失败: {str(e)}"
            print(f"❌ [OCR] {error_msg}")
            import traceback
            traceback.print_exc()
            return self._format_error(return_format, error_msg)
    
    def _recognize_with_windows_ocr(
        self,
        pixmap: QPixmap,
        return_format: str
    ) -> Any:
        """使用 windows_media_ocr 引擎识别"""
        if not WINDOWS_OCR_AVAILABLE:
            return self._format_error(return_format, "windows_media_ocr 不可用")
        
        if not self._windows_ocr_language:
            if not self._initialize_windows_ocr("日本語"):
                return self._format_error(return_format)
        
        try:
            start_time = time.time()
            
            # 直接转换为 bytes
            image_bytes = self._pixmap_to_bytes(pixmap)
            
            # 调用 windows_media_ocr 识别
            result = windows_media_ocr.recognize_from_bytes(
                image_bytes, 
                language=self._windows_ocr_language
            )
            
            elapse = time.time() - start_time
            
            # 检查识别结果
            if result is None or not result.text or not result.lines:
                return self._format_empty_result(return_format)
            
            # 构建结果列表：[[box, text, score], ...]
            ocr_results = []
            for line in result.lines:
                # 从 bounds 构建 box: [[x1,y1], [x2,y2], [x3,y3], [x4,y4]]
                bounds = line.bounds
                box = [
                    [bounds.x, bounds.y],
                    [bounds.x + bounds.width, bounds.y],
                    [bounds.x + bounds.width, bounds.y + bounds.height],
                    [bounds.x, bounds.y + bounds.height]
                ]
                text = line.text
                # windows_media_ocr 没有置信度分数，设为 1.0
                score = 1.0
                ocr_results.append([box, text, score])
            
            # 格式化输出
            return self._format_result(ocr_results, return_format, elapse)
                
        except Exception as e:
            error_msg = f"windows_media_ocr 识别失败: {str(e)}"
            print(f"❌ [OCR] {error_msg}")
            import traceback
            traceback.print_exc()
            return self._format_error(return_format, error_msg)
    
    def _pixmap_to_bytes(self, pixmap: QPixmap) -> bytes:
        """
        将 QPixmap 转换为 PNG bytes
        
        Args:
            pixmap: QPixmap 对象
        
        Returns:
            PNG 格式的 bytes 数据
        """
        buffer = QBuffer()
        buffer.open(QIODevice.OpenModeFlag.WriteOnly)
        pixmap.save(buffer, "PNG")
        buffer.close()
        
        return bytes(buffer.data())

    def _format_result(self, result: list, return_format: str, elapse: float) -> Any:
        """
        格式化 OCR 识别结果
        
        Args:
            result: 原始结果 [[[box], text, confidence], ...]
            return_format: 返回格式 ("text", "list", "dict")
            elapse: 识别耗时(秒)
        
        Returns:
            格式化后的结果
        """
        if return_format == "text":
            # 纯文本格式:拼接所有识别的文字
            texts = [item[1] for item in result if len(item) > 1]
            return "\n".join(texts) if texts else "[未识别到文字]"
        
        elif return_format == "list":
            # 列表格式:[text1, text2, ...]
            return [item[1] for item in result if len(item) > 1]
        
        elif return_format == "dict":
            # 字典格式(兼容旧版 API)
            data = []
            for item in result:
                if len(item) >= 2:
                    box = item[0]
                    text = item[1]
                    confidence = item[2] if len(item) > 2 else 0.0
                    
                    # 确保 box 是普通列表而不是 numpy 数组
                    if hasattr(box, 'tolist'):
                        box = box.tolist()
                    
                    data.append({
                        "box": box,
                        "text": text,
                        "score": confidence
                    })
            
            return {
                "code": 100,
                "msg": "成功",
                "data": data,
                "elapse": elapse
            }
        
        else:
            # 默认返回原始结果
            return result
    
    def _format_empty_result(self, return_format: str) -> Any:
        """格式化空结果"""
        if return_format == "text":
            return "[未识别到文字]"
        elif return_format == "list":
            return []
        elif return_format == "dict":
            return {
                "code": 100,
                "msg": "未识别到文字",
                "data": [],
                "elapse": 0.0
            }
        else:
            return None
    
    def _format_error(self, return_format: str, error_msg: str = None) -> Any:
        """格式化错误结果"""
        msg = error_msg or self._last_error or "OCR 不可用"
        
        if return_format == "text":
            return f"[错误] {msg}"
        elif return_format == "list":
            return []
        elif return_format == "dict":
            return {
                "code": -1,
                "msg": msg,
                "data": [],
                "elapse": 0.0
            }
        else:
            return None
    
    def get_last_error(self) -> str:
        """获取最后一次错误信息"""
        return self._last_error or "无错误"
    
    def close(self):
        """关闭 OCR 引擎"""
        self.release_engine()
    
    def release_engine(self):
        """
        🔥 内存优化：释放 OCR 相关资源
        
        适用场景：
        - 长时间不使用 OCR 功能时
        - 内存紧张时主动释放
        - 钉图窗口关闭后
        """
        try:
            self._ocr_rs_engine = None
            self._windows_ocr_language = None
            
            # 🔥 强制触发垃圾回收
            import gc
            gc.collect()
            
            print("🗑️ [OCR] 资源已释放")
        except Exception as e:
            print(f"⚠️ [OCR] 释放 OCR 资源时出错: {e}")
    
    def is_engine_loaded(self) -> bool:
        """检查 OCR 引擎是否已初始化"""
        if self._current_engine == self.ENGINE_OCR_RS:
            return self._ocr_rs_engine is not None
        elif self._current_engine == self.ENGINE_WINDOWS_OCR:
            return self._windows_ocr_language is not None
        return False
    
    def get_memory_status(self) -> str:
        """获取 OCR 引擎内存状态（用于调试）"""
        if not self._current_engine:
            return "未初始化"
        
        if self._current_engine == self.ENGINE_OCR_RS:
            if self._ocr_rs_engine is not None:
                return "已初始化 (ocr_rs 引擎)"
            else:
                return "未初始化"
        elif self._current_engine == self.ENGINE_WINDOWS_OCR:
            if self._windows_ocr_language:
                return f"已初始化 (windows_media_ocr 引擎, 语言: {self._windows_ocr_language})"
            else:
                return "未初始化"
        
        return "未知状态"


# 全局单例实例
_ocr_manager = OCRManager()


def is_ocr_available() -> bool:
    """检查 OCR 功能是否可用"""
    return _ocr_manager.is_available


def get_available_engines() -> list:
    """获取可用的 OCR 引擎列表"""
    return _ocr_manager.get_available_engines()


def set_ocr_engine(engine_type: str) -> bool:
    """设置当前使用的 OCR 引擎"""
    return _ocr_manager.set_engine(engine_type)


def get_current_engine() -> Optional[str]:
    """获取当前使用的 OCR 引擎"""
    return _ocr_manager.get_current_engine()


def initialize_ocr(language: str = "日本語", engine_type: Optional[str] = None) -> bool:
    """
    初始化 OCR 引擎
    
    Args:
        language: 识别语言
        engine_type: 指定引擎类型（可选）
    
    Returns:
        bool: 是否初始化成功
    """
    return _ocr_manager.initialize(language, engine_type)


def recognize_text(pixmap: QPixmap, **kwargs) -> Any:
    """
    识别图像中的文字
    
    Args:
        pixmap: QPixmap 图像对象
        **kwargs: 其他参数(return_format)
    
    Returns:
        识别结果
    """
    return _ocr_manager.recognize_pixmap(pixmap, **kwargs)


def release_ocr_engine():
    """
    🔥 内存优化：释放 OCR 引擎，回收内存
    
    建议在以下场景调用：
    - 钉图窗口关闭后
    - 长时间不使用 OCR 时
    - 应用切换到后台时
    """
    _ocr_manager.release_engine()


def get_ocr_memory_status() -> str:
    """获取 OCR 引擎内存状态"""
    return _ocr_manager.get_memory_status()


def format_ocr_result_text(result: dict, separator: str = "\n") -> str:
    """
    格式化 OCR 结果为阅读顺序文本
    
    智能处理：
    - 按 Y 坐标分行（从上到下）
    - 同一行内按 X 坐标排序（从左到右）
    - 同行文字用空格连接，不同行用 separator 分隔
    
    Args:
        result: OCR 识别结果（dict 格式，包含 code 和 data 字段）
        separator: 行之间的分隔符，默认换行
        
    Returns:
        格式化后的文本字符串
    
    使用示例:
        result = recognize_text(pixmap, return_format="dict")
        text = format_ocr_result_text(result)
    """
    if not result or not isinstance(result, dict):
        return ""
    
    if result.get('code') != 100:
        return ""
    
    data = result.get('data', [])
    if not data:
        return ""
    
    if len(data) == 1:
        return data[0].get('text', '')
    
    # 收集每个文字块的位置信息
    items_with_pos = []
    for item in data:
        box = item.get('box', [])
        text = item.get('text', '')
        if not box or not text:
            continue
        
        # box 格式: [[x1,y1], [x2,y2], [x3,y3], [x4,y4]]
        # 计算中心Y和高度
        y_coords = [pt[1] for pt in box if len(pt) >= 2]
        if not y_coords:
            continue
        
        min_y = min(y_coords)
        max_y = max(y_coords)
        center_y = (min_y + max_y) / 2
        height = max_y - min_y
        
        # 计算左边X（用于同行内排序）
        x_coords = [pt[0] for pt in box if len(pt) >= 2]
        left_x = min(x_coords) if x_coords else 0
        
        items_with_pos.append({
            'text': text,
            'center_y': center_y,
            'height': height,
            'left_x': left_x
        })
    
    if not items_with_pos:
        return ""
    
    # 计算行高容差
    avg_height = sum(b['height'] for b in items_with_pos) / len(items_with_pos)
    line_tolerance = avg_height * 0.5
    
    # 按Y坐标分行
    lines = []
    current_line = []
    current_line_y = None
    
    # 先按Y排序（从上到下）
    items_with_pos.sort(key=lambda x: x['center_y'])
    
    for block in items_with_pos:
        if current_line_y is None:
            current_line = [block]
            current_line_y = block['center_y']
        elif abs(block['center_y'] - current_line_y) <= line_tolerance:
            # 同一行
            current_line.append(block)
        else:
            # 新的一行：先将当前行按X排序后输出
            current_line.sort(key=lambda x: x['left_x'])
            lines.append(" ".join(b['text'] for b in current_line))
            current_line = [block]
            current_line_y = block['center_y']
    
    # 别忘了最后一行
    if current_line:
        current_line.sort(key=lambda x: x['left_x'])
        lines.append(" ".join(b['text'] for b in current_line))
    
    return separator.join(lines)
