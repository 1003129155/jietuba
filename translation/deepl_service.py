# -*- coding: utf-8 -*-
"""
deepl_service.py - DeepL 翻译 API 服务

提供 DeepL API 的封装，支持异步翻译请求。

使用方式:
    service = DeepLService(api_key="your-api-key")
    result = service.translate("Hello", target_lang="ZH")
    
或使用异步线程:
    thread = TranslationThread(text, api_key, target_lang)
    thread.finished_signal.connect(on_finished)
    thread.start()
"""

import json
import urllib.request
import urllib.error
from typing import Optional, Dict, Any
from PyQt6.QtCore import QThread, pyqtSignal

from core import log_info, log_debug, log_error, log_warning


class DeepLService:
    """DeepL 翻译 API 服务"""
    
    # DeepL API 端点
    API_URL_FREE = "https://api-free.deepl.com/v2/translate"
    API_URL_PRO = "https://api.deepl.com/v2/translate"
    
    def __init__(self, api_key: str, use_pro: bool = False):
        """
        初始化 DeepL 服务
        
        Args:
            api_key: DeepL API 密钥
            use_pro: 是否使用 Pro 版 API（付费版）
        """
        self.api_key = api_key
        self.api_url = self.API_URL_PRO if use_pro else self.API_URL_FREE
        
    def translate(
        self, 
        text: str, 
        target_lang: str = "ZH",
        source_lang: Optional[str] = None,
        split_sentences: str = "1",
        preserve_formatting: bool = True,
        timeout: int = 30
    ) -> Dict[str, Any]:
        """
        翻译文本
        
        Args:
            text: 要翻译的文本
            target_lang: 目标语言代码 (例如: "ZH", "EN", "JA", "KO")
            source_lang: 源语言代码 (可选，不填则自动检测)
            split_sentences: 分句模式 ("0"=不分句, "1"=自动分句, "nonewlines"=忽略换行)
            preserve_formatting: 保留格式 (默认开启)
            timeout: 请求超时时间（秒）
            
        Returns:
            dict: {
                "success": bool,
                "translated_text": str,  # 翻译结果
                "detected_source_lang": str,  # 检测到的源语言
                "error": str  # 错误信息（如果失败）
            }
        """
        if not text or not text.strip():
            return {
                "success": False,
                "translated_text": "",
                "error": "Text is empty"
            }
        
        if not self.api_key:
            return {
                "success": False,
                "translated_text": "",
                "error": "API key not configured"
            }
        
        try:
            # 构建请求数据
            # split_sentences: "0"=不分句, "1"=自动分句(保留换行), "nonewlines"=忽略换行按标点分句
            data = {
                "auth_key": self.api_key,
                "text": text,
                "target_lang": target_lang.upper(),
                "split_sentences": split_sentences,
                "preserve_formatting": "1" if preserve_formatting else "0"
            }
            
            if source_lang:
                data["source_lang"] = source_lang.upper()
            
            # 编码数据
            post_data = urllib.parse.urlencode(data).encode('utf-8')
            
            # 创建请求
            request = urllib.request.Request(
                self.api_url,
                data=post_data,
                method='POST'
            )
            request.add_header('Content-Type', 'application/x-www-form-urlencoded')
            
            log_debug(f"发送翻译请求: {len(text)} 字符 -> {target_lang}", "DeepL")
            
            # 发送请求
            with urllib.request.urlopen(request, timeout=timeout) as response:
                result = json.loads(response.read().decode('utf-8'))
                
                if 'translations' in result and result['translations']:
                    translation = result['translations'][0]
                    translated_text = translation.get('text', '')
                    detected_lang = translation.get('detected_source_language', '')
                    
                    log_info(f"翻译成功: {detected_lang} -> {target_lang}", "DeepL")
                    
                    return {
                        "success": True,
                        "translated_text": translated_text,
                        "detected_source_lang": detected_lang,
                        "error": ""
                    }
                else:
                    return {
                        "success": False,
                        "translated_text": "",
                        "error": "Invalid API response format"
                    }
                    
        except urllib.error.HTTPError as e:
            error_msg = self._parse_http_error(e)
            log_error(f"HTTP 错误: {error_msg}", "DeepL")
            return {
                "success": False,
                "translated_text": "",
                "error": error_msg
            }
            
        except urllib.error.URLError as e:
            log_error(f"网络错误: {e.reason}", "DeepL")
            return {
                "success": False,
                "translated_text": "",
                "error": f"Network error: {e.reason}"
            }
            
        except json.JSONDecodeError as e:
            log_error(f"JSON 解析失败: {e}", "DeepL")
            return {
                "success": False,
                "translated_text": "",
                "error": "Failed to parse API response"
            }
            
        except Exception as e:
            log_error(f"未知错误: {e}", "DeepL")
            return {
                "success": False,
                "translated_text": "",
                "error": f"Translation failed: {str(e)}"
            }
    
    def _parse_http_error(self, error: urllib.error.HTTPError) -> str:
        """解析 HTTP 错误"""
        error_messages = {
            400: "Bad request parameters",
            401: "Invalid API key",
            403: "API key permission denied",
            404: "API endpoint not found",
            413: "Text too long",
            429: "Too many requests, please try again later",
            456: "Quota exceeded",
            500: "DeepL server internal error",
            503: "DeepL service temporarily unavailable"
        }
        
        return error_messages.get(error.code, f"HTTP {error.code}: {error.reason}")


# 需要导入 urllib.parse
import urllib.parse


class TranslationThread(QThread):
    """异步翻译线程"""
    
    # 信号：翻译完成 (success, translated_text, error_msg, detected_source_lang)
    finished_signal = pyqtSignal(bool, str, str, str)
    
    def __init__(
        self, 
        text: str, 
        api_key: str, 
        target_lang: str = "ZH",
        use_pro: bool = False,
        split_sentences: str = "1",
        preserve_formatting: bool = True,
        parent=None
    ):
        """
        初始化翻译线程
        
        Args:
            text: 要翻译的文本
            api_key: DeepL API 密钥
            target_lang: 目标语言代码
            use_pro: 是否使用 Pro API
            split_sentences: 分句模式 ("0"=不分句, "1"=自动分句, "nonewlines"=忽略换行)
            preserve_formatting: 保留格式
            parent: 父对象
        """
        super().__init__(parent)
        self.text = text
        self.api_key = api_key
        self.target_lang = target_lang
        self.use_pro = use_pro
        self.split_sentences = split_sentences
        self.preserve_formatting = preserve_formatting
        
    def run(self):
        """执行翻译"""
        try:
            service = DeepLService(self.api_key, use_pro=self.use_pro)
            result = service.translate(
                self.text, 
                target_lang=self.target_lang,
                split_sentences=self.split_sentences,
                preserve_formatting=self.preserve_formatting
            )
            
            self.finished_signal.emit(
                result["success"],
                result.get("translated_text", ""),
                result.get("error", ""),
                result.get("detected_source_lang", "")
            )
            
        except Exception as e:
            log_error(f"翻译线程异常: {e}", "DeepL")
            self.finished_signal.emit(False, "", f"翻译失败: {str(e)}", "")
