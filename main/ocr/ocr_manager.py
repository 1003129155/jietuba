# -*- coding: utf-8 -*-
"""
ocr_manager.py - OCR 功能模块

为截图工具提供 OCR 文字识别功能。
支持 OCR 引擎：
- windows_media_ocr: Windows 系统自带 OCR API (轻量级，仅几MB)

主要功能:
- 识别截图区域的文字
- 支持中英日文识别
- 单例模式管理 OCR 引擎
- 支持图像预处理(灰度转换、图像放大)

依赖:
- windows_media_ocr: pip install windows_media_ocr
"""
from PyQt6.QtGui import QPixmap, QImage
from PyQt6.QtCore import QBuffer, QIODevice, Qt
from typing import Optional, Dict, Any
import io
import time
import os
import sys
import ctypes
import traceback as _tb

def _ocr_log(msg: str, level: str = "INFO"):
    """写入日志（打包后使用 core.logger，否则 print）"""
    try:
        from core.logger import log_info, log_warning, log_error, log_debug
        if level == "ERROR":
            log_error(msg, "OCR")
        elif level == "WARN":
            log_warning(msg, "OCR")
        elif level == "DEBUG":
            log_debug(msg, "OCR")
        else:
            log_info(msg, "OCR")
    except Exception:
        print(f"[{level}] [OCR] {msg}", flush=True)

def _preload_crt_for_pyinstaller():
    """
    在 PyInstaller 打包环境中预加载 MSVC CRT 运行时库。
    
    这是解决 ocr_rs (MNN/PaddleOCR) 在 PyInstaller 打包后崩溃的关键修复。
    MNN 在初始化时依赖 CRT，如果 CRT 没有正确初始化会导致 ACCESS_VIOLATION。
    通过预先加载 CRT DLL，确保运行时环境正确初始化。
    
    注意：主入口 main_app.py 也有此预加载，这里作为备份确保安全。
    """
    if not getattr(sys, 'frozen', False):
        return  # 非打包环境不需要
    
    crt_dlls = [
        "ucrtbase.dll",
        "vcruntime140.dll", 
        "vcruntime140_1.dll",
        "msvcp140.dll",
    ]
    
    for dll in crt_dlls:
        try:
            ctypes.CDLL(dll)
        except OSError:
            pass  # DLL 可能已加载或不存在，忽略

# 检测可用的 OCR 引擎
# ocr_rs 在 PyInstaller 打包后无法正常工作，已禁用
OCR_RS_AVAILABLE = False
_ocr_rs_version = None

# 尝试导入 windos_ocr（静默检测）
try:
    from . import windos_ocr
    WINDOS_OCR_AVAILABLE = True
    _ocr_log("windos_ocr 引擎可用", "INFO")
except Exception as e:
    WINDOS_OCR_AVAILABLE = False
    windos_ocr = None
    _ocr_log(f"windos_ocr 不可用: {e}", "DEBUG")

# 尝试导入 windows_media_ocr（静默检测）
try:
    import windows_media_ocr
    WINDOWS_OCR_AVAILABLE = True
    try:
        available_langs = windows_media_ocr.get_available_languages()
    except Exception:
        available_langs = []
except ImportError as e:
    WINDOWS_OCR_AVAILABLE = False
    windows_media_ocr = None
    available_langs = []

# 至少有一个引擎可用
OCR_AVAILABLE = WINDOS_OCR_AVAILABLE or WINDOWS_OCR_AVAILABLE


class OCRManager:
    """OCR 管理器 - 单例模式，支持多引擎切换"""
    
    _instance = None
    _initialized = False
    
    # OCR 引擎类型常量
    ENGINE_OCR_RS = "ocr_rs"
    ENGINE_WINDOS_OCR = "windos_ocr"  # Windows ScreenSketch OCR (高性能)
    ENGINE_WINDOWS_OCR = "windows_media_ocr"  # Windows Media OCR (轻量级)
    
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
            self._windos_ocr_engine = None  # windos_ocr 引擎实例 (常驻)
            self._windows_ocr_language = None  # windows_media_ocr 语言设置
    
    @property
    def is_available(self) -> bool:
        """检查 OCR 功能是否可用"""
        return OCR_AVAILABLE
    
    def get_available_engines(self) -> list:
        """获取可用的 OCR 引擎列表"""
        engines = []
        if WINDOS_OCR_AVAILABLE:
            engines.append(self.ENGINE_WINDOS_OCR)
        if WINDOWS_OCR_AVAILABLE:
            engines.append(self.ENGINE_WINDOWS_OCR)
        return engines
    
    def set_engine(self, engine_type: str):
        """
        设置当前使用的 OCR 引擎
        
        Args:
            engine_type: 引擎类型 ("windos_ocr" 或 "windows_media_ocr")
        """
        # 检查引擎是否可用
        if engine_type == self.ENGINE_WINDOS_OCR and not WINDOS_OCR_AVAILABLE:
            _ocr_log("windos_ocr 引擎不可用", "WARN")
            return False
        
        if engine_type == self.ENGINE_WINDOWS_OCR and not WINDOWS_OCR_AVAILABLE:
            _ocr_log("windows_media_ocr 引擎不可用", "WARN")
            return False
        
        # 只支持这两个引擎
        if engine_type not in [self.ENGINE_WINDOS_OCR, self.ENGINE_WINDOWS_OCR]:
            _ocr_log(f"不支持的引擎类型: {engine_type}", "ERROR")
            return False
        
        if self._current_engine != engine_type:
            _ocr_log(f"切换引擎: {self._current_engine} -> {engine_type}")
            self._current_engine = engine_type
            
            if engine_type == self.ENGINE_WINDOS_OCR:
                _ocr_log(f"使用 windos_ocr 引擎 (Windows ScreenSketch OCR)")
            else:
                _ocr_log(f"使用 windows_media_ocr 引擎")
                _ocr_log(f"Windows OCR 支持的语言: {available_langs}")
            return True
        
        return True
    
    def get_current_engine(self) -> Optional[str]:
        """获取当前使用的引擎类型"""
        return self._current_engine
    
    def _get_model_path(self, filename: str) -> str:
        """获取模型文件路径 (用于 ocr_rs)"""
        import sys
        
        # 检测是否在 PyInstaller 打包环境
        if getattr(sys, 'frozen', False):
            # PyInstaller 打包后的环境
            # 优先使用项目根目录的 models（与 IDE 运行一致），再回退到打包内嵌的
            base_dir = sys._MEIPASS
            exe_dir = os.path.dirname(sys.executable)
            # 尝试找到项目根目录（kaifajietu）
            project_root = r"C:\Users\10031\Desktop\kaifajietu"
            
            # 记录一次基础目录，便于诊断
            try:
                if not hasattr(self, "_printed_model_base"):
                    _ocr_log(f"PyInstaller 基础目录: {base_dir}")
                    _ocr_log(f"exe 目录: {exe_dir}")
                    _ocr_log(f"项目根目录: {project_root}")
                    self._printed_model_base = True
            except Exception:
                pass

            possible_paths = [
                # 1. 项目根目录的 models（与 IDE 一致）
                os.path.join(project_root, self.MODEL_DIR, filename),
                # 2. _internal/models
                os.path.join(base_dir, self.MODEL_DIR, filename),
                os.path.join(base_dir, "models", filename),
                # 3. exe 同级
                os.path.join(exe_dir, self.MODEL_DIR, filename),
                os.path.join(exe_dir, "_internal", self.MODEL_DIR, filename),
            ]
        else:
            # 开发环境
            possible_paths = [
                os.path.join(self.MODEL_DIR, filename),
                os.path.join(os.path.dirname(__file__), "..", "..", self.MODEL_DIR, filename),
                os.path.join(os.path.dirname(__file__), self.MODEL_DIR, filename),
            ]
        
        for path in possible_paths:
            abs_path = os.path.abspath(path)
            if os.path.exists(abs_path):
                return abs_path
        
        # 如果都找不到，返回第一个可能的路径（用于错误提示）
        return os.path.abspath(possible_paths[0])
    
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
        
        # 如果没有设置当前引擎，自动选择（优先 windos_ocr）
        if not self._current_engine:
            if WINDOS_OCR_AVAILABLE:
                self._current_engine = self.ENGINE_WINDOS_OCR
                _ocr_log(f"自动选择引擎: {self._current_engine} (windos_ocr 高性能引擎)")
            elif WINDOWS_OCR_AVAILABLE:
                self._current_engine = self.ENGINE_WINDOWS_OCR
                _ocr_log(f"自动选择引擎: {self._current_engine} (windows_media_ocr)")
            else:
                self._last_error = "没有可用的 OCR 引擎"
                return False
        
        # 根据引擎类型初始化
        if self._current_engine == self.ENGINE_WINDOS_OCR:
            return self._initialize_windos_ocr()
        elif self._current_engine == self.ENGINE_WINDOWS_OCR:
            return self._initialize_windows_ocr(language)
        else:
            self._last_error = f"不支持的引擎类型: {self._current_engine}"
            return False
    
    def _initialize_windos_ocr(self) -> bool:
        """初始化 windos_ocr 引擎 (常驻模式)"""
        if not WINDOS_OCR_AVAILABLE:
            self._last_error = "windos_ocr 模块不可用"
            return False
        
        # 如果已经初始化,直接返回
        if self._windos_ocr_engine is not None:
            _ocr_log("windos_ocr 引擎已初始化 (常驻)")
            return True
        
        try:
            _ocr_log("正在加载 windos_ocr 引擎 (首次加载约1-2秒)...")
            from .windos_ocr import OcrEngine
            self._windos_ocr_engine = OcrEngine()  # 加载模型,常驻内存
            _ocr_log("windos_ocr 引擎初始化成功 (已常驻内存)")
            return True
            
        except Exception as e:
            self._last_error = f"windos_ocr 初始化失败: {str(e)}"
            tb_str = _tb.format_exc()
            _ocr_log(f"{self._last_error}\n{tb_str}", "ERROR")
            return False
    
    def _initialize_windows_ocr(self, language: str) -> bool:
        """初始化 windows_media_ocr 引擎"""
        if not WINDOWS_OCR_AVAILABLE:
            self._last_error = "windows_media_ocr 模块不可用"
            return False
        
        try:
            # 映射语言代码
            self._windows_ocr_language = self.LANGUAGE_MAP.get(language, "zh-Hans-CN")
            _ocr_log(f"初始化 windows_media_ocr 引擎(语言配置: {language} -> {self._windows_ocr_language})")
            _ocr_log("windows_media_ocr 引擎初始化成功")
            return True
            
        except Exception as e:
            self._last_error = f"windows_media_ocr 初始化失败: {str(e)}"
            tb_str = _tb.format_exc()
            _ocr_log(f"{self._last_error}\n{tb_str}", "ERROR")
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
        
        # 根据引擎类型调用对应的识别方法
        if self._current_engine == self.ENGINE_WINDOS_OCR:
            return self._recognize_with_windos_ocr(pixmap, return_format)
        elif self._current_engine == self.ENGINE_WINDOWS_OCR:
            return self._recognize_with_windows_ocr(pixmap, return_format)
        else:
            return self._format_error(return_format, f"不支持的引擎: {self._current_engine}")
    
    def _recognize_with_windos_ocr(
        self,
        pixmap: QPixmap,
        return_format: str
    ) -> Any:
        """使用 windos_ocr 引擎识别"""
        # 确保引擎已初始化
        if self._windos_ocr_engine is None:
            if not self._initialize_windos_ocr():
                return self._format_error(return_format)
        
        try:
            start_time = time.time()
            
            # 转换 QPixmap → PIL Image
            from PIL import Image
            from io import BytesIO
            
            buffer = QBuffer()
            buffer.open(QBuffer.OpenModeFlag.ReadWrite)
            pixmap.save(buffer, "PNG")
            image_bytes = buffer.data().data()
            pil_image = Image.open(BytesIO(image_bytes))
            
            # 调用 windos_ocr 识别 (常驻引擎,速度快)
            result = self._windos_ocr_engine.recognize_pil(pil_image)
            
            elapse = time.time() - start_time
            
            # 检查识别结果
            if not result or not result.get('lines'):
                return self._format_empty_result(return_format)
            
            # 转换为统一格式: [[box, text, score], ...]
            ocr_results = []
            for line in result['lines']:
                if line['text']:
                    # 转换 bounding_rect 为 box 格式
                    bbox = line['bounding_rect']
                    if bbox:
                        box = [
                            [bbox['x1'], bbox['y1']],
                            [bbox['x2'], bbox['y2']],
                            [bbox['x3'], bbox['y3']],
                            [bbox['x4'], bbox['y4']]
                        ]
                    else:
                        # 如果没有 bbox,使用默认值
                        box = [[0, 0], [100, 0], [100, 20], [0, 20]]
                    
                    # 计算平均置信度(从词级别)
                    confidences = [word['confidence'] for word in line.get('words', []) 
                                 if word.get('confidence') is not None]
                    score = sum(confidences) / len(confidences) if confidences else 1.0
                    
                    ocr_results.append([box, line['text'], score])
            
            # 格式化输出
            return self._format_result(ocr_results, return_format, elapse)
                
        except Exception as e:
            error_msg = f"windos_ocr 识别失败: {str(e)}"
            tb_str = _tb.format_exc()
            _ocr_log(f"{error_msg}\n{tb_str}", "ERROR")
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
            tb_str = _tb.format_exc()
            _ocr_log(f"{error_msg}\n{tb_str}", "ERROR")
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
            # 释放 windos_ocr 引擎 (约150MB)
            if self._windos_ocr_engine is not None:
                self._windos_ocr_engine = None  # __del__ 会自动释放 DLL 资源
                _ocr_log("windos_ocr 引擎已释放")
            
            self._windows_ocr_language = None
            
            # 🔥 强制触发垃圾回收
            import gc
            gc.collect()
            
            _ocr_log("资源已释放")
        except Exception as e:
            _ocr_log(f"释放 OCR 资源时出错: {e}", "WARN")
    
    def is_engine_loaded(self) -> bool:
        """检查 OCR 引擎是否已初始化"""
        if self._current_engine == self.ENGINE_WINDOS_OCR:
            return self._windos_ocr_engine is not None
        elif self._current_engine == self.ENGINE_WINDOWS_OCR:
            return self._windows_ocr_language is not None
        return False
    
    def get_memory_status(self) -> str:
        """获取 OCR 引擎内存状态（用于调试）"""
        if not self._current_engine:
            return "未初始化"
        
        if self._current_engine == self.ENGINE_WINDOS_OCR:
            if self._windos_ocr_engine is not None:
                return "已初始化 (windos_ocr 引擎, 常驻内存 ~150MB)"
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
