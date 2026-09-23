"""Translation provider contract."""

from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Any, Mapping

from .models import TranslationRequest, TranslationResult


@dataclass(frozen=True)
class Field:
    """一个设置项的界面声明。

    config_key 同时给出读写方法名：ToolSettingsManager 上的
    ``get_<config_key>`` / ``set_<config_key>``。界面只认这个约定，不认
    provider 的名字，所以加一家引擎不需要改任何界面代码。

    由 provider 自己声明、而不是在界面里各写一份表单，是因为下拉框是从注册表
    动态生成的：界面里漏补一个 provider 不会报错，只会表现为「选得到这个引擎，
    下面却是空白或上一个引擎的表单」——azure 和 baidu 各漏过一次，后者还漏在
    保存那一侧，填了存不上、界面一声不吭。
    """

    config_key: str
    label: str


@dataclass(frozen=True)
class TextField(Field):
    """一行文本输入。secret=True 的会掩码显示，并统一带「显示/隐藏」按钮。

    显示/隐藏由这里的 secret 决定、而不是各页面自己加，是因为以前它只长在
    DeepL 那一行上：六个 secret 字段只有一个能查看，另外五个填错了也看不出来。
    """

    placeholder: str = ""
    secret: bool = False


@dataclass(frozen=True)
class ModelField(TextField):
    """模型名输入框。设置页会在旁边放一个按钮，用 provider 的 list_models()
    拉取可选模型；不认识它的界面（欢迎页）当普通 TextField 渲染。"""


@dataclass(frozen=True)
class ToggleField(Field):
    """一个开关。用于某一家引擎独有的布尔选项，例如 DeepL 的 Pro。"""

    description: str = ""
    icon_name: str = ""


@dataclass(frozen=True)
class ProviderMetadata:
    provider_id: str
    display_name: str
    credentials: tuple = ()
    options: tuple = ()
    # 「通用翻译选项」里这家吃哪些。纯粹是界面显隐用的：请求里这些参数始终都
    # 带着（preserve_formatting 是 TranslationRequest 的字段，split_sentences
    # 在 options 里），不支持的 provider 自己忽略。别把它理解成会裁剪请求。
    supported_request_options: frozenset = field(default_factory=frozenset)
    # 引擎页底部那行提示（免费额度之类）。有了它，界面就不用为了显示
    # 「DeepL 每月 50 万字符」而去判断当前选的是不是 DeepL。
    notice: str = ""
    help_label: str = ""
    help_url: str = ""


class TranslationProvider(ABC):
    provider_id: str
    display_name: str
    # 子类按需覆写：这个引擎需要哪些凭据、有哪些独有开关、去哪里申请
    CREDENTIAL_FIELDS: tuple = ()
    OPTION_FIELDS: tuple = ()
    SUPPORTED_REQUEST_OPTIONS: frozenset = frozenset()
    NOTICE: str = ""
    HELP_LABEL: str = ""
    HELP_URL: str = ""
    MIN_TIMEOUT: int = 0

    def effective_timeout(self, request: TranslationRequest) -> int:
        return max(request.timeout, self.MIN_TIMEOUT)

    @abstractmethod
    def is_configured(self) -> bool:
        """Return whether the provider has enough configuration to run."""

    @abstractmethod
    def translate(self, request: TranslationRequest) -> TranslationResult:
        """Perform one synchronous translation request."""

    def supported_target_languages(self) -> set[str] | None:
        return None

    def list_models(self) -> list[str]:
        """接口可用的模型名。只有声明了 ModelField 的 provider 需要实现；
        失败时抛 RuntimeError，消息直接给用户看。"""
        raise NotImplementedError

    @classmethod
    def metadata(cls) -> ProviderMetadata:
        return ProviderMetadata(
            cls.provider_id,
            cls.display_name,
            credentials=cls.CREDENTIAL_FIELDS,
            options=cls.OPTION_FIELDS,
            supported_request_options=frozenset(
                cls.SUPPORTED_REQUEST_OPTIONS
            ),
            notice=cls.NOTICE,
            help_label=cls.HELP_LABEL,
            help_url=cls.HELP_URL,
        )

    @classmethod
    def all_fields(cls) -> tuple:
        """凭据 + 独有选项。界面要「这家的全部设置项」时用这个。"""
        return tuple(cls.CREDENTIAL_FIELDS) + tuple(cls.OPTION_FIELDS)


ProviderConfig = Mapping[str, Any]
