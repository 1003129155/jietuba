"""Qt worker shared by every translation provider."""

from __future__ import annotations

from typing import Any, Mapping

from PySide6.QtCore import QThread, Signal

from core.logger import log_error, T

from .models import TranslationErrorCode, TranslationRequest, TranslationResult
from .provider import TranslationProvider
from .service import TranslationService


class TranslationWorker(QThread):
    finished_signal = Signal(object)
    # 翻译成功后由工作线程发、在主线程记账（工作线程不碰 QSettings）。
    _record_signal = Signal(str, int)

    def __init__(
        self,
        service: TranslationService,
        request: TranslationRequest,
        *,
        provider_id: str | None = None,
        provider_overrides: Mapping[str, Any] | None = None,
        parent=None,
    ) -> None:
        super().__init__(parent)
        self._service = service
        self._request = request
        self._provider: TranslationProvider | None = None
        self._configuration_error = ""
        self._quota_error: TranslationResult | None = None
        # Resolve configuration on the GUI thread. The worker thread only owns
        # the provider's synchronous network call and never touches QSettings.
        try:
            self._provider = service.provider(
                provider_id, provider_overrides
            )
            # 额度闸门也在 GUI 线程做：截图翻译/设置页测试都不经过
            # TranslationService.translate()，这里补上「创建 worker 前检查」。
            self._quota_error = service.check_quota(self._provider)
        except ValueError as exc:
            self._configuration_error = str(exc)
        except Exception:
            # 配置对象不实现额度协议时静默放行，不因额度机制挡住翻译。
            self._quota_error = None
        # 记账信号从工作线程发、槽在构造线程（GUI 线程）排队执行。
        self._record_signal.connect(self._record_usage_on_main_thread)

    def effective_timeout_ms(self) -> int:
        if self._provider is None:
            return 0
        return int(self._provider.effective_timeout(self._request) * 1000)

    def run(self) -> None:
        try:
            if self._provider is None:
                result = TranslationResult(
                    success=False,
                    error_code=TranslationErrorCode.NOT_CONFIGURED,
                    error_message=self._configuration_error,
                )
            elif not self._provider.is_configured():
                result = TranslationResult(
                    success=False,
                    error_code=TranslationErrorCode.NOT_CONFIGURED,
                    error_message=(
                        f"{self._provider.display_name} is not configured"
                    ),
                )
            elif self._quota_error is not None:
                # 额度已满：不发请求，直接把锁定结果交给界面。
                result = self._quota_error
            else:
                result = self._provider.translate(self._request)
                if result.success:
                    self._record_signal.emit(
                        self._provider.provider_id,
                        len(self._request.text or ""),
                    )
        except Exception as exc:
            log_error(T("翻译线程异常: {exc}", exc=exc), "Translation")
            result = TranslationResult(
                success=False,
                error_code=TranslationErrorCode.UNKNOWN,
                error_message=f"Translation failed: {exc}",
            )
        if not self.isInterruptionRequested():
            self.finished_signal.emit(result)

    def _record_usage_on_main_thread(self, provider_id: str, chars: int) -> None:
        """翻译成功后累计本月用量。在工作线程（GUI 线程）执行。

        与 service._record_usage 一致：只在 provider 声明了额度上限时记账，
        这样额度锁关掉时用量仍能显示，但不影响未声明额度的引擎。
        """
        try:
            provider = self._provider
            if provider is None:
                return
            limit = getattr(provider, "QUOTA_LIMIT_CHARS", None)
            if limit is None or limit <= 0:
                return
            self._service.config.add_translation_usage_chars(provider_id, chars)
        except Exception:
            pass
