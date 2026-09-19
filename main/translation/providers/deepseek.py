# -*- coding: utf-8 -*-
"""DeepSeek 翻译 provider（OpenAI 兼容接口）。"""

from __future__ import annotations

from ..provider import TextField
from .openai_compatible import OpenAICompatibleProvider


class DeepSeekProvider(OpenAICompatibleProvider):
    provider_id = "deepseek"
    display_name = "DeepSeek"

    # 注意没有 /v1 后缀，DeepSeek 的 OpenAI 兼容端点就挂在根上
    DEFAULT_BASE_URL = "https://api.deepseek.com"
    DEFAULT_MODEL = "deepseek-flash"
    # DeepSeek 文档按场景给推荐值，翻译是 1.3（默认 1.0）。
    # 比直觉高，但那是官方针对翻译质量给的；输出格式由 JSON mode 约束，
    # 不靠低温度保证。
    TEMPERATURE = 1.3
    # 关掉思考链。DeepSeek 的思考模式默认开启、强度 high，而翻译用不上它：
    # 实测 500 汉字中译英，开着要 3.5 秒、输出 769 token，其中 2403 字符是
    # reasoning_content；关掉只要 1.5 秒、282 token。用户撞到的 30 秒超时
    # 就是这么来的——长文本把思考链一起拖长了。
    # 用 thinking 而不是 reasoning_effort：后者的取值只有 low/high/max，
    # 没有 none（虽然实测传 none 也生效，但那是没文档保证的行为）。
    EXTRA_BODY = {"thinking": {"type": "disabled"}}

    CREDENTIAL_FIELDS = (
        TextField("deepseek_api_key", "DeepSeek API Key", "sk-...",
                  secret=True),
        # 模型名做成可填项，是因为它真的会变：deepseek-chat 这一代名字已经
        # 换成 deepseek-flash / deepseek-v4-pro，旧名虽然还接受但已停用。
        # 留个口子，下次改名不用等发版。
        TextField("deepseek_model", "Model", DEFAULT_MODEL),
        # 地址一般不用动。留着是为了走代理/镜像，或者指向任何别的
        # OpenAI 兼容服务（通义、Kimi、本地 Ollama）。
        TextField("deepseek_base_url", "API Base URL", DEFAULT_BASE_URL),
    )
    NOTICE = (
        "LLM translation: better with context and idioms, but slower, "
        "billed per token, and may refuse some content"
    )
    HELP_LABEL = "platform.deepseek.com"
    HELP_URL = "https://platform.deepseek.com/api_keys"
