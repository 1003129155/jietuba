"""Provider selection and translation orchestration."""

from __future__ import annotations

from collections.abc import Callable, Mapping
from typing import Any, Protocol

from core.logger import log_info

from .models import TranslationErrorCode, TranslationRequest, TranslationResult
from .registry import ProviderRegistry


class TranslationConfig(Protocol):
    def get_translation_provider(self) -> str: ...

    def get_translation_provider_config(
        self, provider_id: str
    ) -> Mapping[str, Any]: ...

    def get_translation_usage_chars(self, provider_id: str) -> int: ...

    def add_translation_usage_chars(self, provider_id: str, chars: int) -> None: ...

    def get_translation_quota_enabled(self, provider_id: str) -> bool: ...

    def get_translation_quota_limit(self, provider_id: str) -> int: ...


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
        # 额度保护：provider 声明了 QUOTA_LIMIT_CHARS 就按月记账、超限即拦，
        # 防止误超免费额度产生费用。只有火山/腾讯等声明了上限的引擎受约束。
        quota_error = self._check_quota(provider)
        if quota_error is not None:
            return quota_error
        result = provider.translate(request)
        if result.success:
            self._record_usage(provider, request.text)
        return result

    def _check_quota(self, provider) -> TranslationResult | None:
        """当前月已用字符数达到上限时返回锁定错误，否则返回 None。"""
        limit = self._effective_quota_limit(provider)
        if limit is None or limit <= 0:
            return None
        used = self.config.get_translation_usage_chars(provider.provider_id)
        if used >= limit:
            log_info(
                f"{provider.display_name} hit monthly quota {used}/{limit}",
                "TranslationService",
            )
            return TranslationResult(
                success=False,
                error_code=TranslationErrorCode.QUOTA_EXCEEDED,
                error_message=(
                    f"{provider.display_name} 本月免费额度已用尽（{used}/"
                    f"{limit} 字符），已自动停用，下月 1 号自动恢复。"
                ),
            )
        return None

    def check_quota(self, provider) -> TranslationResult | None:
        """供 worker 在主线程做额度闸门：返回超限结果或 None。

        截图翻译/设置页测试走 TranslationWorker，不经过 translate()，
        所以这个公开入口让 worker 在创建时（GUI 线程）检查额度，既满足
        「工作线程不碰 QSettings」，又能让额度锁在每一条翻译路径上生效。
        """
        return self._check_quota(provider)

    def _effective_quota_limit(self, provider) -> int | None:
        """provider 声明上限时，用配置里的开关/自定义上限覆盖默认值。

        额度锁做成设置里可见的选项：开关关掉就不拦截，上限可改。
        未声明 QUOTA_LIMIT_CHARS 的引擎（DeepL 等）不受本机制约束。
        """
        default_limit = getattr(provider, "QUOTA_LIMIT_CHARS", None)
        if default_limit is None or default_limit <= 0:
            return None
        provider_id = provider.provider_id
        if not self.config.get_translation_quota_enabled(provider_id):
            return None
        custom = self.config.get_translation_quota_limit(provider_id)
        if custom and custom > 0:
            return custom
        return default_limit

    def _record_usage(self, provider, text: str) -> None:
        """翻译成功后累加本月的已用字符数（按请求文本长度估算）。"""
        limit = getattr(provider, "QUOTA_LIMIT_CHARS", None)
        if limit is None or limit <= 0:
            return
        self.config.add_translation_usage_chars(
            provider.provider_id, len(text or "")
        )

    @property
    def monthly_usage(self) -> dict[str, int]:
        """当前月各受额度约束 provider 的已用字符数（供设置页展示）。"""
        usage = {}
        for meta in self.registry.available_providers():
            provider_id = meta.provider_id
            try:
                provider = self.registry.create(provider_id, {})
            except Exception:
                continue
            limit = getattr(provider, "QUOTA_LIMIT_CHARS", None)
            if limit is not None and limit > 0:
                usage[provider_id] = self.config.get_translation_usage_chars(
                    provider_id
                )
        return usage


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
        TencentTranslateProvider,
        VolcengineTranslateProvider,
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
        TencentTranslateProvider.provider_id,
        TencentTranslateProvider,
        display_name=TencentTranslateProvider.display_name,
    )
    registry.register(
        VolcengineTranslateProvider.provider_id,
        VolcengineTranslateProvider,
        display_name=VolcengineTranslateProvider.display_name,
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
