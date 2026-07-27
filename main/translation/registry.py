"""Registry and factory for translation providers."""

from __future__ import annotations

from collections.abc import Callable

from .provider import ProviderConfig, ProviderMetadata, TranslationProvider


ProviderFactory = Callable[[ProviderConfig], TranslationProvider]


class ProviderRegistry:
    def __init__(self) -> None:
        self._factories: dict[str, ProviderFactory] = {}
        self._metadata: dict[str, ProviderMetadata] = {}

    def register(
        self,
        provider_id: str,
        factory: ProviderFactory,
        *,
        display_name: str | None = None,
    ) -> None:
        normalized = provider_id.strip().lower()
        if not normalized:
            raise ValueError("provider_id must not be empty")
        self._factories[normalized] = factory
        self._metadata[normalized] = ProviderMetadata(
            normalized, display_name or normalized
        )

    def create(
        self, provider_id: str, config: ProviderConfig
    ) -> TranslationProvider:
        normalized = provider_id.strip().lower()
        try:
            factory = self._factories[normalized]
        except KeyError as exc:
            raise ValueError(
                f"Unknown translation provider: {provider_id}"
            ) from exc
        return factory(config)

    def metadata(self, provider_id: str) -> ProviderMetadata:
        try:
            return self._metadata[provider_id.strip().lower()]
        except KeyError as exc:
            raise ValueError(
                f"Unknown translation provider: {provider_id}"
            ) from exc

    def available_providers(self) -> tuple[ProviderMetadata, ...]:
        return tuple(self._metadata.values())

