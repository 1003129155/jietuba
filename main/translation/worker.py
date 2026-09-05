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
        self._request = request
        self._provider: TranslationProvider | None = None
        self._configuration_error = ""
        # Resolve configuration on the GUI thread. The worker thread only owns
        # the provider's synchronous network call and never touches QSettings.
        try:
            self._provider = service.provider(
                provider_id, provider_overrides
            )
        except ValueError as exc:
            self._configuration_error = str(exc)

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
            else:
                result = self._provider.translate(self._request)
        except Exception as exc:
            log_error(T("翻译线程异常: {exc}", exc=exc), "Translation")
            result = TranslationResult(
                success=False,
                error_code=TranslationErrorCode.UNKNOWN,
                error_message=f"Translation failed: {exc}",
            )
        if not self.isInterruptionRequested():
            self.finished_signal.emit(result)
