"""把一张图交给 OCR 引擎在后台线程里跑，结果报成 (文字, 失败原因键)

识别是一次不可中断的同步 FFI 调用：cancel 只能让结果不再报出去，打不断正在跑的那一次。
而解释器终止时若还有线程停在 FFI 里，进程会直接 abort（实测退出码 0xC0000409），不是
"安静地在后台跑完"。所以每个线程都登记在 _running 里，退出前由 shutdown_recognition
逐个等回来。

失败只报一个原因键、不报现成的句子：句子要翻译，而译文查的是主线程装好的 translator，
工作线程不该去碰；窗口拿到键再取对应的文案。
"""

from PySide6.QtCore import QThread, Signal
from PySide6.QtGui import QImage

from core.logger import log_debug, log_error, log_warning, T

# 失败原因键，窗口按它取提示文案
UNAVAILABLE = "unavailable"   # 本版本不带 OCR 引擎
NO_TEXT = "no_text"           # 引擎跑完了，选区里没有文字
FAILED = "failed"             # 识别本身出错

_running = set()


class RecognizeThread(QThread):
    """一次识别。

    只持有 QImage——值类型，与 GUI 资源无关。窗口关掉、底图释放都影响不到它，所以线程
    这边永远不会碰到悬空的 QWidget。
    """

    recognized = Signal(str, str)   # (识别出的文字, 失败原因键)；成功时原因为空串

    def __init__(self, image: QImage):
        super().__init__()
        self._image = image
        self._cancelled = False

    def cancel(self):
        """丢弃这次识别的结果。识别本身打不断，只是不再报出去"""
        self._cancelled = True

    def run(self):
        text, reason = "", FAILED
        try:
            from ocr import format_ocr_result_text, is_ocr_available, recognize_text

            if not is_ocr_available():
                reason = UNAVAILABLE
            else:
                result = recognize_text(self._image, return_format="dict")
                text = format_ocr_result_text(result).strip() if isinstance(result, dict) else ""
                reason = "" if text else NO_TEXT
        except Exception as e:
            log_error(T("文字识别失败: {e}", e=e), "OCR")
        finally:
            self._image = None   # 识别完就放掉，线程对象还要等主线程来收

        if not self._cancelled:
            self.recognized.emit(text, reason)


def recognize_async(image: QImage, on_recognized) -> RecognizeThread:
    """后台识别 image，结果发给 on_recognized(文字, 失败原因键)。

    on_recognized 要是某个 QObject 的方法：Qt 在那个对象销毁时会自己断开连接，窗口先关、
    识别后完成也打不到已经销毁的对象上。

    信号在 start 之前连好：识别可能在调用方拿到返回值之前就结束（本版本不带 OCR 时立刻
    就失败了），那时还没连上的话结果就丢了。
    """
    thread = RecognizeThread(image)
    thread.recognized.connect(on_recognized)
    thread.finished.connect(lambda: _retire(thread))
    _running.add(thread)
    thread.start()
    log_debug(T("文字识别线程已启动"), "OCR")
    return thread


def _retire(thread):
    """线程跑完了（finished 派到主线程才走到这）：登记表里划掉，对象交给事件循环回收"""
    _running.discard(thread)
    thread.deleteLater()


def shutdown_recognition(timeout_ms: int = 3000) -> None:
    """退出前等还在跑的识别线程结束。

    只 cancel 不等是不够的：打不断的正是识别本身，wait() 才是让退出安全的那一步。等不到
    的线程照旧留在登记表里——这时候把最后一个引用丢掉，QThread 会在还运行着的时候被
    析构，那是必崩的一种死法。
    """
    for thread in list(_running):
        thread.cancel()
        if thread.isRunning() and not thread.wait(max(0, timeout_ms)):
            log_warning(T("退出时文字识别线程未在期限内结束"), "OCR")
