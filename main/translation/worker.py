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
    # 从 run() 发出，排队到 GUI 线程记账，工作线程不碰 QSettings。
    _usage_signal = Signal(int)

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
        except ValueError as exc:
            self._configuration_error = str(exc)
        else:
            self._quota_error = service.check_quota(self._provider)
        self._usage_signal.connect(self._record_usage)

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
                result = self._quota_error
            else:
                result = self._provider.translate(self._request)
                if result.success:
                    self._usage_signal.emit(len(self._request.text or ""))
        except Exception as exc:
            log_error(T("翻译线程异常: {exc}", exc=exc), "Translation")
            result = TranslationResult(
                success=False,
                error_code=TranslationErrorCode.UNKNOWN,
                error_message=f"Translation failed: {exc}",
            )
        if not self.isInterruptionRequested():
            self.finished_signal.emit(result)

    def _record_usage(self, chars: int) -> None:
        self._service.record_usage(self._provider, chars)
