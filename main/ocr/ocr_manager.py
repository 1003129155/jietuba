# -*- coding: utf-8 -*-
"""
ocr_manager.py - OCR 功能模块

为截图工具提供 OCR 文字识别功能。
使用 RapidOCR 引擎(Python API 版本),完全离线识别。

主要功能:
- 识别截图区域的文字
- 支持多语言识别
- 单例模式管理 OCR 引擎
- 支持图像预处理(灰度转换、图像放大)

依赖:
- pip install rapidocr onnxruntime
"""
from PyQt6.QtGui import QPixmap, QImage
from PyQt6.QtCore import QBuffer, QIODevice, Qt
from typing import Optional, Dict, Any
import io

# 尝试导入 RapidOCR
try:
    from rapidocr import RapidOCR
    import numpy as np
    from PIL import Image
    OCR_AVAILABLE = True
    print("✅ [OCR] RapidOCR 模块加载成功")
except ImportError as e:
    print(f"⚠️ [OCR] RapidOCR 模块导入失败: {e}")
    print("💡 [OCR] 请运行: pip install rapidocr onnxruntime")
    OCR_AVAILABLE = False
    RapidOCR = None
    np = None
    Image = None


class OCRManager:
    """OCR 管理器 - 单例模式"""
    
    _instance = None
    _ocr_engine = None
    _initialized = False
    _current_language = None
    
    def __new__(cls):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
        return cls._instance
    
    def __init__(self):
        """初始化 OCR 管理器"""
        if not self._initialized:
            self._initialized = True
            self._ocr_engine = None
            self._last_error = None
            self._current_language = None
    
    @property
    def is_available(self) -> bool:
        """检查 OCR 功能是否可用"""
        return OCR_AVAILABLE
    
    def initialize(self, language: str = "日本語") -> bool:
        """
        初始化 OCR 引擎
        
        Args:
            language: 识别语言(当前使用 PP-OCRv5 中英日混合模型,此参数保留但不影响模型选择)
        
        Returns:
            bool: 是否初始化成功
        """
        if not OCR_AVAILABLE:
            self._last_error = "RapidOCR 模块不可用,请运行: pip install rapidocr onnxruntime"
            return False
        
        # 如果已经初始化且语言相同,直接返回
        if self._ocr_engine is not None and self._current_language == language:
            return True
        
        # 如果语言改变,需要重新初始化
        if self._ocr_engine is not None and self._current_language != language:
            self._ocr_engine = None
        
        try:
            print(f"📖 [OCR] 初始化 RapidOCR 引擎(语言配置: {language})")
            
            # 使用 PP-OCRv5 中英日混合识别模型
            # 参考文档: https://rapidai.github.io/RapidOCRDocs/model_list/
            from rapidocr import OCRVersion
            
            params = {
                "Det.ocr_version": OCRVersion.PPOCRV5,  # 使用 v5 检测模型
                "Rec.ocr_version": OCRVersion.PPOCRV5,  # 使用 v5 识别模型
            }
            
            self._ocr_engine = RapidOCR(params=params)
            self._current_language = language
            
            print("✅ [OCR] RapidOCR 引擎初始化成功(PP-OCRv5 中英日混合识别)")
            return True
            
        except Exception as e:
            self._last_error = f"OCR 初始化失败: {str(e)}"
            print(f"❌ [OCR] {self._last_error}")
            import traceback
            traceback.print_exc()
            return False
    
    def recognize_pixmap(
        self, 
        pixmap: QPixmap, 
        return_format: str = "dict",
        enable_grayscale: bool = True,
        enable_upscale: bool = True,
        upscale_factor: float = 1.5
    ) -> Any:
        """
        识别 QPixmap 图像中的文字
        
        Args:
            pixmap: QPixmap 图像对象
            return_format: 返回格式 ("text", "list", "dict")
            enable_grayscale: 是否启用灰度转换
            enable_upscale: 是否启用图像放大
            upscale_factor: 图像放大倍数(1.0-3.0)
        
        Returns:
            识别结果(格式取决于 return_format)
        """
        if not self._ocr_engine:
            if not self.initialize():
                return self._format_error(return_format)
        
        try:
            # 预处理图像
            processed_pixmap = self._preprocess_image(
                pixmap, 
                enable_grayscale=enable_grayscale,
                enable_upscale=enable_upscale,
                upscale_factor=upscale_factor
            )
            
            # 转换为 PIL Image
            pil_image = self._pixmap_to_pil(processed_pixmap)
            
            # 调用 RapidOCR 识别（返回 RapidOCROutput 对象）
            result = self._ocr_engine(pil_image)
            
            # 检查识别结果
            if result is None or result.boxes is None or len(result.boxes) == 0:
                return self._format_empty_result(return_format)
            
            # 构建结果列表：[[box, text, score], ...]
            ocr_results = []
            for box, text, score in zip(result.boxes, result.txts, result.scores):
                ocr_results.append([box, text, score])
            
            # 如果启用了放大,需要转换坐标回原始尺寸
            if enable_upscale and upscale_factor > 1.0:
                ocr_results = self._convert_coordinates(ocr_results, scale_factor=upscale_factor)
            
            # 格式化输出
            return self._format_result(ocr_results, return_format, result.elapse)
                
        except Exception as e:
            error_msg = f"OCR 识别失败: {str(e)}"
            print(f"❌ [OCR] {error_msg}")
            import traceback
            traceback.print_exc()
            return self._format_error(return_format, error_msg)
    
    def _preprocess_image(
        self, 
        pixmap: QPixmap,
        enable_grayscale: bool = True,
        enable_upscale: bool = True,
        upscale_factor: float = 1.5
    ) -> QPixmap:
        """
        图像预处理
        
        Args:
            pixmap: 输入图像
            enable_grayscale: 是否启用灰度转换
            enable_upscale: 是否启用图像放大
            upscale_factor: 图像放大倍数
        
        Returns:
            处理后的 QPixmap
        """
        image = pixmap.toImage()
        
        # 1. 灰度转换(可选, ~5ms)
        if enable_grayscale:
            if image.format() != QImage.Format.Format_Grayscale8:
                image = image.convertToFormat(QImage.Format.Format_Grayscale8)
        
        # 2. 图像放大(可选, ~30-50ms)
        if enable_upscale and upscale_factor > 1.0:
            new_width = int(image.width() * upscale_factor)
            new_height = int(image.height() * upscale_factor)
            image = image.scaled(
                new_width, 
                new_height, 
                Qt.AspectRatioMode.IgnoreAspectRatio, 
                Qt.TransformationMode.SmoothTransformation
            )
        
        return QPixmap.fromImage(image)
    
    def _pixmap_to_pil(self, pixmap: QPixmap):
        """
        将 QPixmap 转换为 PIL Image
        
        Args:
            pixmap: QPixmap 对象
        
        Returns:
            PIL Image 对象
        """
        # 转换为 bytes
        buffer = QBuffer()
        buffer.open(QIODevice.OpenModeFlag.WriteOnly)
        pixmap.save(buffer, "PNG")
        buffer.close()
        
        # 使用 PIL 打开
        img_bytes = buffer.data().data()
        pil_image = Image.open(io.BytesIO(img_bytes))
        
        return pil_image
    
    def _convert_coordinates(self, result: list, scale_factor: float) -> list:
        """
        将放大后的坐标转换回原始图像坐标
        
        Args:
            result: RapidOCR 识别结果 [[[box], text, confidence], ...]
            scale_factor: 放大倍数(如 1.5)
        
        Returns:
            转换后的结果
        """
        converted_result = []
        
        for item in result:
            if len(item) >= 2:
                box = item[0]  # numpy array: [[x1,y1], [x2,y2], [x3,y3], [x4,y4]]
                text = item[1]
                confidence = item[2] if len(item) > 2 else 0.0
                
                # 转换坐标 (numpy数组可以直接除法)
                converted_box = (box / scale_factor).astype(int).tolist()
                
                converted_result.append([converted_box, text, confidence])
            else:
                converted_result.append(item)
        
        return converted_result
    
    def _format_result(self, result: list, return_format: str, elapse: float) -> Any:
        """
        格式化 OCR 识别结果
        
        Args:
            result: RapidOCR 原始结果 [[[box], text, confidence], ...]
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
        if self._ocr_engine:
            try:
                # RapidOCR 会自动管理资源,无需手动关闭
                self._ocr_engine = None
                self._current_language = None
                print("✅ [OCR] OCR 引擎已关闭")
            except Exception as e:
                print(f"⚠️ [OCR] 关闭 OCR 引擎时出错: {e}")


# 全局单例实例
_ocr_manager = OCRManager()


def is_ocr_available() -> bool:
    """检查 OCR 功能是否可用"""
    return _ocr_manager.is_available


def initialize_ocr() -> bool:
    """
    初始化 OCR 引擎
    
    Returns:
        bool: 是否初始化成功
    """
    return _ocr_manager.initialize()


def recognize_text(pixmap: QPixmap, **kwargs) -> Any:
    """
    识别图像中的文字
    
    Args:
        pixmap: QPixmap 图像对象
        **kwargs: 其他参数(return_format, enable_grayscale, enable_upscale, upscale_factor)
    
    Returns:
        识别结果
    """
    return _ocr_manager.recognize_pixmap(pixmap, **kwargs)
