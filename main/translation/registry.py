"""Registry and factory for translation providers."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import replace

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
        self._metadata[normalized] = self._build_metadata(
            normalized, factory, display_name
        )

    @staticmethod
    def _build_metadata(
        provider_id: str,
        factory: ProviderFactory,
        display_name: str | None,
    ) -> ProviderMetadata:
        """以 provider 自己的 metadata() 为准，注册时显式给的名字仍然优先。

        用 provider 的那份是为了带上凭据字段声明；普通工厂函数（测试里常见）
        没有 metadata()，退回最小信息。
        """
        own = getattr(factory, "metadata", None)
        metadata = own() if callable(own) else None
        if not isinstance(metadata, ProviderMetadata):
            return ProviderMetadata(provider_id, display_name or provider_id)
        return replace(
            metadata,
            provider_id=provider_id,
            display_name=display_name or metadata.display_name,
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

