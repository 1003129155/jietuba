"""
钉图 OCR 管理器

负责 OCR 初始化、异步识别线程管理和结果回调。
"""

import time
from PySide6.QtCore import QThread
from core import log_debug, log_info, log_warning, log_error
from core.logger import log_exception


class _OCRThread(QThread):
    """OCR 异步识别线程（内部类）
    
    线程安全设计：接收纯 QImage（值类型拷贝），不持有 QWidget 引用。
    即使窗口在识别期间关闭，线程也不会访问悬空对象。
    """

    def __init__(self, image, parent=None):
        super().__init__(parent)
        self._image = image  # QImage（值类型，线程安全）
        self.result = None
        self.prepared_items = None
        self.prepared_union_rect = None

    def run(self):
        start_time = time.time()
        try:
            from ocr import recognize_text
            from pin.ocr_text_layer import OCRTextLayer

            self.result = recognize_text(self._image, return_format="dict")
            if self.result and isinstance(self.result, dict):
                self.prepared_items, self.prepared_union_rect = (
                    OCRTextLayer.prepare_ocr_items(self.result)
                )

            elapsed = time.time() - start_time
            log_debug(f"OCR处理完成，总耗时: {elapsed:.3f}秒", "OCR")
        except Exception as e:
            elapsed = time.time() - start_time
            log_error(f"识别失败: {e}，耗时: {elapsed:.3f}秒", "OCR")
            import traceback
            traceback.print_exc()
            self.result = None
            self.prepared_items = None
            self.prepared_union_rect = None
        finally:
            self._image = None  # 释放图像数据


class PinOCRManager:
    """
    钉图 OCR 管理器

    职责：
    - 检查 OCR 可用性和配置
    - 创建 OCRTextLayer
    - 管理异步 OCR 线程的生命周期
    - 处理 OCR 完成后的结果加载
    - 安全清理（窗口关闭时）
    """

    def __init__(self, pin_window, config_manager):
        self._win = pin_window
        self._cfg = config_manager
        self.ocr_text_layer = None
        self.ocr_thread = None
        self._ocr_has_result = False
        self._translate_pending = False

    # ------------------------------------------------------------------
    # 公开属性（供 PinWindow 读取）
    # ------------------------------------------------------------------

    @property
    def has_result(self) -> bool:
        return self._ocr_has_result

    @property
    def translate_pending(self) -> bool:
        return self._translate_pending

    @translate_pending.setter
    def translate_pending(self, value: bool):
        self._translate_pending = value

    @property
    def is_running(self) -> bool:
        return self.ocr_thread is not None

    # ------------------------------------------------------------------
    # 初始化
    # ------------------------------------------------------------------

    def init_now(self):
        """立即初始化 OCR：创建文字层 + 启动异步识别线程"""
        try:
            from ocr import is_ocr_available, initialize_ocr
            from pin.ocr_text_layer import OCRTextLayer

            if not self._cfg:
                return

            if not self._cfg.get_ocr_enabled():
                log_info("OCR 功能已禁用，跳过初始化", "OCR")
                return

            if not is_ocr_available():
                log_debug("OCR 模块不可用（无OCR版本），静默跳过", "OCR")
                return

            if not initialize_ocr():
                log_warning("OCR 引擎初始化失败", "OCR")
                return

            log_debug("OCR 引擎已就绪（支持中日韩英混合识别）", "OCR")

            # 创建透明文字层
            self.ocr_text_layer = OCRTextLayer(self._win)
            cr = self._win.content_rect()
            self.ocr_text_layer.setGeometry(cr.toRect())
            self.ocr_text_layer.set_enabled(True)
            log_debug(f"OCR层初始化几何: {cr.toRect()}", "OCR")

            # 立即启动异步识别
            self._start_recognition()

        except ImportError:
            pass  # OCR 模块不存在，静默跳过
        except Exception as e:
            log_exception(e, "OCR初始化", silent=False)
            import traceback
            traceback.print_exc()

    def _start_recognition(self):
        """启动异步 OCR 识别线程
        
        关键：在主线程获取图像（QImage 值类型拷贝），
        子线程只接收纯数据，不持有任何 QWidget 引用。
        """
        pixmap = self._win._base_pixmap
        if not pixmap:
            return
        original_width = pixmap.width()
        original_height = pixmap.height()

        # 主线程获取图像（安全），传给子线程
        image = self._win.get_current_image()

        log_debug("开始异步识别文字...", "OCR")
        self.ocr_thread = _OCRThread(image, parent=self._win)
        self.ocr_thread.finished.connect(
            lambda: self._on_finished(original_width, original_height)
        )
        self.ocr_thread.start()

    def _on_finished(self, original_width: int, original_height: int):
        """OCR 线程完成回调（主线程）"""
        try:
            # 检查窗口 C++ 对象是否还有效
            try:
                is_closed = self._win._is_closed
            except RuntimeError:
                log_debug("窗口C++对象已销毁，跳过OCR结果加载", "OCR")
                return

            if is_closed:
                log_debug("窗口已关闭，跳过结果加载", "OCR")
                return

            if self.ocr_text_layer is None:
                log_debug("OCR 文字层已被清理，跳过结果加载", "OCR")
                return

            # 检查文字层 C++ 对象是否还有效
            try:
                _ = self.ocr_text_layer.isVisible
            except RuntimeError:
                log_debug("OCR文字层C++对象已销毁，跳过结果加载", "OCR")
                return

            if self.ocr_thread is None:
                return

            if self.ocr_thread.prepared_items:
                self.ocr_text_layer.load_prepared_ocr_items(
                    self.ocr_thread.prepared_items,
                    self.ocr_thread.prepared_union_rect,
                    original_width,
                    original_height,
                )

                text_count = len(self.ocr_thread.prepared_items)
                if text_count > 0:
                    self._ocr_has_result = True

                    if self._translate_pending:
                        self._translate_pending = False
                        log_info("OCR 完成，执行等待中的翻译", "Translate")
                        self._win._on_translate_clicked()

                log_info(f"钉图文字层已就绪，识别到 {text_count} 个文字块", "OCR")
        except Exception as e:
            log_error(f"加载OCR结果失败: {e}", "OCR")
            import traceback
            traceback.print_exc()
        finally:
            try:
                if self.ocr_thread:
                    self.ocr_thread.deleteLater()
                    self.ocr_thread = None
            except Exception as e:
                log_exception(e, "清理OCR线程")

    # ------------------------------------------------------------------
    # 清理
    # ------------------------------------------------------------------

    def cleanup(self):
        """安全清理所有 OCR 资源（PinWindow 关闭时调用）"""
        # 清理 OCR 线程
        if self.ocr_thread is not None:
            try:
                if self.ocr_thread.isRunning():
                    log_warning("窗口关闭，OCR 线程仍在运行，将其安全分离...", "OCR")
                    from core.qt_utils import safe_disconnect
                    safe_disconnect(self.ocr_thread.finished)
                    self.ocr_thread.setParent(None)
                    thread_ref = self.ocr_thread
                    self.ocr_thread.finished.connect(
                        lambda: thread_ref.deleteLater() if thread_ref else None
                    )
                else:
                    self.ocr_thread.deleteLater()
            except Exception as e:
                log_exception(e, "清理OCR线程")
            finally:
                self.ocr_thread = None

        # 清理文字层
        if self.ocr_text_layer is not None:
            try:
                self.ocr_text_layer.set_enabled(False)
                if hasattr(self.ocr_text_layer, 'cleanup'):
                    self.ocr_text_layer.cleanup()
                self.ocr_text_layer.deleteLater()
            except Exception as e:
                log_exception(e, "清理OCR文字层")
            finally:
                self.ocr_text_layer = None

    # ------------------------------------------------------------------
    # OCR 层状态控制（供 PinWindow / 缩略图模式 调用）
    # ------------------------------------------------------------------

    def set_enabled(self, enabled: bool):
        """启用/禁用 OCR 文字层交互"""
        if self.ocr_text_layer:
            self.ocr_text_layer.set_enabled(enabled)

    def set_drawing_mode(self, active: bool):
        """设置绘图模式（绘图时隐藏 OCR 层）"""
        if self.ocr_text_layer:
            self.ocr_text_layer.set_drawing_mode(active)

    def update_geometry(self, rect):
        """更新 OCR 文字层几何（resize 时调用）"""
        if self.ocr_text_layer:
            self.ocr_text_layer.setGeometry(rect)
 