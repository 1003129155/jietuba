"""Provider selection and translation orchestration."""

from __future__ import annotations

from collections.abc import Callable, Mapping
from typing import Any, Protocol

from .models import TranslationErrorCode, TranslationRequest, TranslationResult
from .registry import ProviderRegistry


class TranslationConfig(Protocol):
    def get_translation_provider(self) -> str: ...

    def get_translation_provider_config(
        self, provider_id: str
    ) -> Mapping[str, Any]: ...


class TranslationService:
    def __init__(
        self, registry: ProviderRegistry, config: TranslationConfig
    ) -> None:
        self.registry = registry
        self.config = config

    def active_provider_id(self) -> str:
        return self.config.get_translation_provider().strip().lower()

    def provider(
        self,
        provider_id: str | None = None,
        overrides: Mapping[str, Any] | None = None,
    ):
        selected = (provider_id or self.active_provider_id()).strip().lower()
        provider_config = dict(
            self.config.get_translation_provider_config(selected)
        )
        if overrides:
            provider_config.update(overrides)
        return self.registry.create(selected, provider_config)

    def provider_name(self, provider_id: str | None = None) -> str:
        selected = provider_id or self.active_provider_id()
        return self.registry.metadata(selected).display_name

    def provider_label(
        self,
        translate: Callable[[str], str] = str,
        provider_id: str | None = None,
        overrides: Mapping[str, Any] | None = None,
    ) -> str:
        """翻译窗口上显示的服务名。自定义服务可以换成用户起的名字，
        所以要按当前配置建出 provider 来问，不能只看注册表。

        注册表也收不继承 TranslationProvider 的工厂，那种就只用注册名。
        """
        name = translate(self.provider_name(provider_id))
        label = getattr(self.provider(provider_id, overrides), "display_label", None)
        return label(name) if callable(label) else name

    def is_configured(
        self,
        provider_id: str | None = None,
        overrides: Mapping[str, Any] | None = None,
    ) -> bool:
        try:
            return self.provider(provider_id, overrides).is_configured()
        except ValueError:
            return False

    def translate(
        self,
        request: TranslationRequest,
        *,
        provider_id: str | None = None,
        overrides: Mapping[str, Any] | None = None,
    ) -> TranslationResult:
        try:
            provider = self.provider(provider_id, overrides)
        except ValueError as exc:
            return TranslationResult(
                success=False,
                error_code=TranslationErrorCode.NOT_CONFIGURED,
                error_message=str(exc),
            )
        if not provider.is_configured():
            return TranslationResult(
                success=False,
                error_code=TranslationErrorCode.NOT_CONFIGURED,
                error_message=f"{provider.display_name} is not configured",
            )
        return provider.translate(request)


def create_default_translation_service(config=None) -> TranslationService:
    if config is None:
        from settings import get_tool_settings_manager

        config = get_tool_settings_manager()

    from .providers import (
        AmazonTranslateProvider,
        AzureTranslateProvider,
        BaiduTranslateProvider,
        CUSTOM_LLM_PROVIDERS,
        DeepSeekProvider,
        DeepLProvider,
        GoogleTranslateProvider,
    )

    registry = ProviderRegistry()
    registry.register(
        DeepLProvider.provider_id,
        DeepLProvider,
        display_name=DeepLProvider.display_name,
    )
    registry.register(
        AmazonTranslateProvider.provider_id,
        AmazonTranslateProvider,
        display_name=AmazonTranslateProvider.display_name,
    )
    registry.register(
        GoogleTranslateProvider.provider_id,
        GoogleTranslateProvider,
        display_name=GoogleTranslateProvider.display_name,
    )
    registry.register(
        AzureTranslateProvider.provider_id,
        AzureTranslateProvider,
        display_name=AzureTranslateProvider.display_name,
    )
    registry.register(
        BaiduTranslateProvider.provider_id,
        BaiduTranslateProvider,
        display_name=BaiduTranslateProvider.display_name,
    )
    registry.register(
        DeepSeekProvider.provider_id,
        DeepSeekProvider,
        display_name=DeepSeekProvider.display_name,
    )
    for provider in CUSTOM_LLM_PROVIDERS:
        registry.register(
            provider.provider_id,
            provider,
            display_name=provider.display_name,
        )
    return TranslationService(registry, config)
