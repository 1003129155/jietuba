# -*- coding: utf-8 -*-
"""自定义 OpenAI 兼容服务：OpenAI、Ollama、LM Studio、各家云厂商的兼容端点。

和 DeepSeek 共用请求实现，区别在于这里什么都不预设：地址和模型必填，
Key 可以不填（本地服务不校验），温度不填就不传（有的模型只接受默认温度，
传了直接 400），DeepSeek 专用的关思考参数也不带。服务之间真正不同的参数
留给「附加请求体」，由用户按自己服务的文档填。
"""

from __future__ import annotations

import json
from typing import Any, Mapping

from ..provider import ModelField, TextField, ToggleField
from .openai_compatible import OpenAICompatibleProvider


class CustomLLMProvider(OpenAICompatibleProvider):
    provider_id = "custom_llm"
    display_name = "Custom LLM"

    TEMPERATURE = None
    REQUIRES_API_KEY = False
    # 本地模型在 CPU 上跑，长文本一分钟以上很常见
    DEFAULT_TIMEOUT = 60
    TIMEOUT_RANGE = (5, 600)
    # 思考模型（如 Ollama 上的 Qwen3.5）默认开思考，输出额度可能被思考耗尽。
    # Ollama 的 /v1 兼容接口认 reasoning_effort，不认 think。
    THINKING_HINT = (
        'Turn thinking off by adding {"reasoning_effort": "none"} to Extra Body'
    )

    CREDENTIAL_FIELDS = (
        TextField("custom_llm_base_url", "API Base URL",
                  "https://api.openai.com/v1"),
        TextField("custom_llm_api_key", "API Key",
                  "Optional for local models", secret=True),
        ModelField("custom_llm_model", "Model", "gpt-4o-mini"),
    )
    OPTION_FIELDS = (
        TextField("custom_llm_temperature", "Temperature", "Server default"),
        TextField("custom_llm_timeout", "Timeout (s)", str(DEFAULT_TIMEOUT)),
        TextField("custom_llm_instructions", "Instructions",
                  "e.g. Keep product names untranslated"),
        TextField("custom_llm_extra_body", "Extra Body",
                  '{"reasoning_effort": "none"}'),
        ToggleField(
            "custom_llm_json_mode", "JSON Mode",
            "Ask for JSON replies. If the service rejects it, the request is "
            "retried without; turn it off to skip that retry",
            icon_name="DOCUMENT",
        ),
    )
    NOTICE = (
        "Any OpenAI-compatible service. Ollama: http://localhost:11434/v1, "
        "LM Studio: http://localhost:1234/v1"
    )

    def __init__(self, config: Mapping[str, Any]):
        super().__init__(config)
        self._json_mode = bool(config.get("json_mode", True))
        self._instructions = str(config.get("instructions", "") or "").strip()
        errors = []

        temperature = str(config.get("temperature", "") or "").strip()
        if temperature:
            try:
                self._temperature = float(temperature)
            except ValueError:
                errors.append("Temperature must be a number")

        # 实例属性遮住类上的下限，effective_timeout 按它算
        self.MIN_TIMEOUT = self.DEFAULT_TIMEOUT
        timeout = str(config.get("timeout", "") or "").strip()
        if timeout:
            try:
                low, high = self.TIMEOUT_RANGE
                self.MIN_TIMEOUT = min(max(int(float(timeout)), low), high)
            except ValueError:
                errors.append("Timeout must be a number of seconds")

        extra = str(config.get("extra_body", "") or "").strip()
        if extra:
            try:
                parsed = json.loads(extra)
            except ValueError:
                parsed = None
            if isinstance(parsed, dict):
                self._extra_body = parsed
            else:
                errors.append("Extra Body must be a JSON object")

        self._config_error = "; ".join(errors)
