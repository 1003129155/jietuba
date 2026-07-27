"""Translation provider contract."""

from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass
from typing import Any, Mapping

from .models import TranslationRequest, TranslationResult


@dataclass(frozen=True)
class ProviderMetadata:
    provider_id: str
    display_name: str


class TranslationProvider(ABC):
    provider_id: str
    display_name: str

    @abstractmethod
    def is_configured(self) -> bool:
        """Return whether the provider has enough configuration to run."""

    @abstractmethod
    def translate(self, request: TranslationRequest) -> TranslationResult:
        """Perform one synchronous translation request."""

    def supported_target_languages(self) -> set[str] | None:
        return None

    @classmethod
    def metadata(cls) -> ProviderMetadata:
        return ProviderMetadata(cls.provider_id, cls.display_name)


ProviderConfig = Mapping[str, Any]

