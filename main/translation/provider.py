"""Translation provider contract."""

from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass
from typing import Any, Mapping

from .models import TranslationRequest, TranslationResult


@dataclass(frozen=True)
class CredentialField:
    """一个凭据输入项的界面声明。

    config_key 同时给出读写方法名：ToolSettingsManager 上的
    ``get_<config_key>`` / ``set_<config_key>``。

    由 provider 自己声明、而不是在界面里各写一份表单，是因为下拉框是从注册表
    动态生成的：界面里漏补一个 provider 不会报错，只会表现为「选得到这个引擎，
    下面却是空白或上一个引擎的表单」——azure 就这么漏过一次。
    """

    config_key: str
    label: str
    placeholder: str = ""
    secret: bool = False


@dataclass(frozen=True)
class ProviderMetadata:
    provider_id: str
    display_name: str
    credentials: tuple = ()
    help_label: str = ""
    help_url: str = ""


class TranslationProvider(ABC):
    provider_id: str
    display_name: str
    # 子类按需覆写：这个引擎需要哪些凭据、去哪里申请
    CREDENTIAL_FIELDS: tuple = ()
    HELP_LABEL: str = ""
    HELP_URL: str = ""

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
        return ProviderMetadata(
            cls.provider_id,
            cls.display_name,
            credentials=cls.CREDENTIAL_FIELDS,
            help_label=cls.HELP_LABEL,
            help_url=cls.HELP_URL,
        )


ProviderConfig = Mapping[str, Any]

