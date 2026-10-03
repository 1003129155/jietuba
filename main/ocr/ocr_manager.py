# -*- coding: utf-8 -*-
"""
ocr_manager.py - OCR 功能模块

为截图工具提供 OCR 文字识别功能。


主要功能:
- 识别截图区域的文字
- 两个引擎：Windows 截图工具自带的 OCR（oneocr），以及随完整版发布的 PP-OCR
- 单例模式管理 OCR 引擎，按设置自动或手动选择引擎

"""

from PySide6.QtGui import QPixmap, QImage
from typing import Optional, Any
import time
import os
import sys
import ctypes
import importlib.util
import traceback as _tb
import threading

from core.logger import T

def _ocr_log(msg, level: str = "INFO"):
    """写入日志（打包后使用 core.logger，否则 print）。msg 可以是 str 或 T() 构造的可翻译消息。"""
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
        pass

def _preload_crt_for_pyinstaller():
    """
    在 PyInstaller 打包环境中预加载 MSVC CRT 运行时库。
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


# 截图工具 OCR：需要 oneocr 包 + 本机装有带 OCR 文件的截图工具（Windows 11）
try:
    from ocr.snipping_tool_ocr import find_snipping_tool
    _snipping_tool = find_snipping_tool() if importlib.util.find_spec("oneocr") else None
except Exception as e:
    _snipping_tool = None
    _ocr_log(T("截图工具 OCR 检测失败: {e}", e=e), "DEBUG")
ONEOCR_AVAILABLE = _snipping_tool is not None
if ONEOCR_AVAILABLE:
    _ocr_log(T("截图工具 OCR 可用: {package}", package=_snipping_tool[0]), "DEBUG")
else:
    _ocr_log(T("截图工具 OCR 不可用（未安装截图工具或缺少 OCR 文件）"), "DEBUG")

# 尝试检测 ppocr_rust（Rust + ort 的 PP-OCR 引擎，原生线程无 GIL，体积小，开源合规）
# 需要 ppocr_rust 扩展可导入 + det/rec onnx 模型文件存在
def _ppocr_rust_model_paths():
    """返回 (det_path, rec_path)；找不到则返回 (None, None)。

    查找顺序：
      1) 打包后：exe 同级目录的 models/（外置，启动快、可替换）
      2) 打包后回退：_MEIPASS/models/（若模型被打进包内）
      3) 开发环境：仓库根 models/
    """
    candidates = []
    try:
        if getattr(sys, "frozen", False):
            exe_dir = os.path.dirname(sys.executable)
            candidates.append(os.path.join(exe_dir, "models"))
            meipass = getattr(sys, "_MEIPASS", None)
            if meipass:
                candidates.append(os.path.join(meipass, "models"))
        else:
            from core.resource_manager import ResourceManager
            candidates.append(ResourceManager.get_resource_path("models"))
    except Exception as e:
        _ocr_log(T("解析 ppocr_rust 模型路径失败: {e}", e=e), "DEBUG")

    for base in candidates:
        det = os.path.join(base, "PP-OCRv6_det_small.onnx")
        rec = os.path.join(base, "PP-OCRv6_rec_small.onnx")
        if os.path.exists(det) and os.path.exists(rec):
            return det, rec
    return None, None

try:
    _pp_spec = importlib.util.find_spec("ppocr_rust") is not None
    _pp_det, _pp_rec = _ppocr_rust_model_paths() if _pp_spec else (None, None)
    PP_RUST_AVAILABLE = bool(_pp_spec and _pp_det and _pp_rec)
    if PP_RUST_AVAILABLE:
        _ocr_log(T("ppocr_rust 引擎可用 (Rust + ort PP-OCR)"), "DEBUG")
    elif _pp_spec:
        _ocr_log(T("ppocr_rust 已安装但缺少模型文件"), "DEBUG")
    else:
        _ocr_log(T("ppocr_rust 未安装"), "DEBUG")
except Exception as e:
    PP_RUST_AVAILABLE = False
    _pp_det = _pp_rec = None
    _ocr_log(T("ppocr_rust 引擎检测失败: {e}", e=e), "DEBUG")


def _configured_preference() -> str:
    try:
        from settings.tool_settings import get_tool_settings_manager
        return get_tool_settings_manager().get_ocr_engine()
    except Exception as e:
        _ocr_log(T("读取 OCR 引擎设置失败: {e}", e=e), "WARN")
        return OCRManager.ENGINE_AUTO


class OCRManager:
    """OCR 管理器 - 单例模式，支持多引擎切换

    识别、加载、释放都在 _engine_lock 里做：引擎被释放的同时另一个线程还在用它，
    oneocr.dll 会访问已释放的内存直接让进程崩掉。
    """

    _instance = None
    _initialized = False
    
    # OCR 引擎类型常量
    ENGINE_AUTO = "auto"  # 设置值：有截图工具 OCR 就用它，否则用 PP-OCR
    ENGINE_ONEOCR = "oneocr"  # Windows 截图工具自带的 OCR
    ENGINE_PP_RUST = "ppocr_rust"  # Rust + ort 的 PP-OCR，只在完整版里
    ENGINE_PREFERENCES = (ENGINE_AUTO, ENGINE_ONEOCR, ENGINE_PP_RUST)

    def __new__(cls):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
        return cls._instance
    
    def __init__(self):
        """初始化 OCR 管理器"""
        if not self._initialized:
            self._initialized = True
            self._last_error = None
            self._preference = None  # 设置里选的引擎；None 表示还没读过设置
            self._current_engine = None  # 按设置和可用性解析出的引擎
            self._pp_engine = None  # ppocr_rust.Engine 实例，None 即未初始化
            self._one_engine = None  # SnippingToolOcr 实例，None 即未初始化
            self._engine_lock = threading.RLock()

    @property
    def is_available(self) -> bool:
        """检查 OCR 功能是否可用"""
        return ONEOCR_AVAILABLE or PP_RUST_AVAILABLE

    def get_available_engines(self) -> list:
        """获取可用的 OCR 引擎列表"""
        engines = []
        if ONEOCR_AVAILABLE:
            engines.append(self.ENGINE_ONEOCR)
        if PP_RUST_AVAILABLE:
            engines.append(self.ENGINE_PP_RUST)
        return engines

    def resolve_engine(self, preference: str) -> Optional[str]:
        """把设置值解析成实际使用的引擎；选中的引擎在本机不可用时返回 None。"""
        available = self.get_available_engines()
        if preference == self.ENGINE_AUTO:
            return available[0] if available else None
        return preference if preference in available else None

    def set_engine(self, engine_type: str):
        """
        设置引擎偏好并立即生效。

        切走的引擎当场释放；正好有识别在跑时，由那次识别结束后释放。新引擎在下次识别时才加载。

        Args:
            engine_type: "auto" / "oneocr" / "ppocr_rust"
        """
        if engine_type not in self.ENGINE_PREFERENCES:
            _ocr_log(T("不支持的引擎类型: {engine_type}", engine_type=engine_type), "ERROR")
            return False

        self._preference = engine_type
        target = self.resolve_engine(engine_type)
        if target != self._current_engine:
            _ocr_log(T("切换引擎: {old} -> {new}", old=self._current_engine, new=target))
            self._current_engine = target
            if self._engine_lock.acquire(blocking=False):
                try:
                    self._close_stale_engines()
                finally:
                    self._engine_lock.release()

        if target is None:
            if engine_type == self.ENGINE_PP_RUST:
                self._last_error = "此版本不含 PP-OCR，请下载完整版"
            elif engine_type == self.ENGINE_ONEOCR:
                self._last_error = "未找到带 OCR 的 Windows 截图工具"
            else:
                self._last_error = "没有可用的 OCR 引擎"
            _ocr_log(T("OCR 引擎不可用: {engine}", engine=engine_type), "WARN")
            return False
        return True
    
    def get_current_engine(self) -> Optional[str]:
        """获取当前使用的引擎类型"""
        return self._current_engine

    def _ensure_preference(self):
        if self._preference is None:
            self.set_engine(_configured_preference())

    def initialize(self, language: str = "日本語", engine_type: Optional[str] = None) -> bool:
        """
        加载 OCR 引擎

        Args:
            language: 不再使用，两个引擎都不需要指定语言；保留参数兼容旧调用
            engine_type: 指定引擎偏好，为 None 时按设置

        Returns:
            bool: 是否初始化成功
        """
        if engine_type:
            self.set_engine(engine_type)
        else:
            self._ensure_preference()

        # 钉图时在主线程调用；引擎已加载就不碰锁，免得等另一张图的识别跑完
        engine = self._current_engine
        if engine == self.ENGINE_PP_RUST:
            return self._initialize_ppocr_rust()
        if engine == self.ENGINE_ONEOCR:
            return self._initialize_oneocr()
        return False

    def _initialize_ppocr_rust(self) -> bool:
        """初始化 ppocr_rust 引擎 (Rust + ort，加载 det/rec onnx 模型)"""
        if not PP_RUST_AVAILABLE:
            self._last_error = "ppocr_rust 引擎不可用"
            return False
        if self._pp_engine is not None:
            return True
        with self._engine_lock:
            if self._pp_engine is not None:
                return True
            try:
                _ocr_log(T("正在初始化 ppocr_rust 引擎 (Rust + ort)..."), "DEBUG")
                import ppocr_rust
                self._pp_engine = ppocr_rust.Engine(_pp_det, _pp_rec)
                _ocr_log(T("ppocr_rust 引擎初始化成功"), "DEBUG")
                return True
            except Exception as e:
                self._last_error = f"ppocr_rust 初始化失败: {str(e)}"
                tb_str = _tb.format_exc()
                _ocr_log(T("ppocr_rust 初始化失败: {e}\n{tb}", e=str(e), tb=tb_str), "ERROR")
                self._pp_engine = None
                return False

    def _initialize_oneocr(self) -> bool:
        """加载截图工具 OCR；首次运行要先把约 114 MB 的文件复制到应用数据目录。"""
        if not ONEOCR_AVAILABLE:
            self._last_error = "截图工具 OCR 不可用"
            return False
        if self._one_engine is not None:
            return True
        with self._engine_lock:
            if self._one_engine is not None:
                return True
            try:
                _ocr_log(T("正在初始化截图工具 OCR..."), "DEBUG")
                from ocr.snipping_tool_ocr import SnippingToolOcr
                self._one_engine = SnippingToolOcr(_snipping_tool)
                _ocr_log(T("截图工具 OCR 初始化成功"), "DEBUG")
                return True
            except Exception as e:
                self._last_error = f"截图工具 OCR 初始化失败: {str(e)}"
                _ocr_log(T("截图工具 OCR 初始化失败: {e}\n{tb}", e=str(e), tb=_tb.format_exc()), "ERROR")
                self._one_engine = None
                return False

    def _close_stale_engines(self):
        """释放不是当前引擎的那个。调用方持有 _engine_lock。"""
        if self._current_engine != self.ENGINE_PP_RUST and self._pp_engine is not None:
            self._pp_engine.close()
            self._pp_engine = None
            _ocr_log(T("已释放 OCR 引擎: {engine}", engine=self.ENGINE_PP_RUST))
        if self._current_engine != self.ENGINE_ONEOCR and self._one_engine is not None:
            self._one_engine.close()
            self._one_engine = None
            _ocr_log(T("已释放 OCR 引擎: {engine}", engine=self.ENGINE_ONEOCR))

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
        self._ensure_preference()
        with self._engine_lock:
            try:
                engine = self._current_engine
                if engine == self.ENGINE_PP_RUST:
                    return self._recognize_with_ppocr_rust(pixmap, return_format)
                if engine == self.ENGINE_ONEOCR:
                    return self._recognize_with_oneocr(pixmap, return_format)
                return self._format_error(return_format)
            finally:
                self._close_stale_engines()

    @staticmethod
    def _to_qimage(pixmap) -> QImage:
        return pixmap if isinstance(pixmap, QImage) else pixmap.toImage()

    def _qimage_to_rgb_bytes(self, pixmap: QPixmap):
        """将 QPixmap/QImage 转为 (rgb_bytes, w, h, stride)，RGB888 格式，供 ppocr_rust 使用。"""
        image = self._to_qimage(pixmap)
        if image.isNull():
            return None
        if image.format() != QImage.Format.Format_RGB888:
            image = image.convertToFormat(QImage.Format.Format_RGB888)
        w = image.width()
        h = image.height()
        stride = image.bytesPerLine()
        ptr = image.constBits()
        raw = bytes(ptr[: stride * h])
        return raw, w, h, stride

    def _recognize_with_ppocr_rust(
        self,
        pixmap: QPixmap,
        return_format: str
    ) -> Any:
        """使用 ppocr_rust (Rust + ort PP-OCR) 引擎识别。

        Rust 内部用 allow_threads 释放 GIL，推理跑在原生线程，不会拖慢主线程 UI。
        """
        if not PP_RUST_AVAILABLE:
            return self._format_error(return_format, "ppocr_rust 不可用")
        if self._pp_engine is None:
            if not self._initialize_ppocr_rust():
                return self._format_error(return_format)
        try:
            start_time = time.time()
            conv = self._qimage_to_rgb_bytes(pixmap)
            if conv is None:
                return self._format_empty_result(return_format)
            raw, w, h, stride = conv

            # 取到局部变量：release() 可能在别的线程把 self._pp_engine 置空
            engine = self._pp_engine
            if engine is None:
                return self._format_error(return_format, "ppocr_rust 引擎已释放")
            lines = engine.recognize(raw, w, h, stride)
            elapse = time.time() - start_time

            if not lines:
                return self._format_empty_result(return_format)

            # ppocr_rust 返回 list[TextLine]，.points 是四个角点
            ocr_results = []
            for line in lines:
                if not line.text:
                    continue
                ocr_results.append([
                    [[float(x), float(y)] for x, y in line.points],
                    line.text,
                    float(line.score),
                ])
            if not ocr_results:
                return self._format_empty_result(return_format)
            return self._format_result(ocr_results, return_format, elapse)
        except Exception as e:
            error_msg = f"ppocr_rust 识别失败: {str(e)}"
            tb_str = _tb.format_exc()
            _ocr_log(T("ppocr_rust 识别失败: {e}\n{tb}", e=str(e), tb=tb_str), "ERROR")
            return self._format_error(return_format, error_msg)

    def _recognize_with_oneocr(
        self,
        pixmap: QPixmap,
        return_format: str
    ) -> Any:
        """使用截图工具 OCR 识别。"""
        if self._one_engine is None:
            if not self._initialize_oneocr():
                return self._format_error(return_format)
        try:
            start_time = time.time()
            image = self._to_qimage(pixmap)
            if image.isNull():
                return self._format_empty_result(return_format)
            ocr_results = self._one_engine.recognize(image)
            elapse = time.time() - start_time
            if not ocr_results:
                return self._format_empty_result(return_format)
            return self._format_result(ocr_results, return_format, elapse)
        except Exception as e:
            error_msg = f"截图工具 OCR 识别失败: {str(e)}"
            _ocr_log(T("截图工具 OCR 识别失败: {e}\n{tb}", e=str(e), tb=_tb.format_exc()), "ERROR")
            return self._format_error(return_format, error_msg)

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
        释放已加载的引擎（模型几十到上百 MB），下次识别时按当前设置重新加载。
        """
        try:
            with self._engine_lock:
                if self._pp_engine is not None:
                    self._pp_engine.close()
                    self._pp_engine = None
                if self._one_engine is not None:
                    self._one_engine.close()
                    self._one_engine = None
            _ocr_log(T("OCR 管理器状态已重置"))
        except Exception as e:
            _ocr_log(T("释放 OCR 资源时出错: {e}", e=e), "WARN")
    
    def is_engine_loaded(self) -> bool:
        """检查 OCR 引擎是否已初始化"""
        if self._current_engine == self.ENGINE_PP_RUST:
            return self._pp_engine is not None
        elif self._current_engine == self.ENGINE_ONEOCR:
            return self._one_engine is not None
        return False
    
    def get_memory_status(self) -> str:
        """获取 OCR 引擎内存状态（用于调试）"""
        if not self.is_engine_loaded():
            return "未初始化"
        if self._current_engine == self.ENGINE_PP_RUST:
            return "已初始化 (ppocr_rust Rust+ort 引擎)"
        return "已初始化 (截图工具 OCR)"


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
    内存优化：释放 OCR 引擎，回收内存
    
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
    line_tolerance = avg_height * 0.8
    
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
 
