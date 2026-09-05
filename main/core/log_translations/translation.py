"""main/translation/ 目录下 log_* 调用的中→英翻译表。"""

TRANSLATIONS: dict[str, str] = {
    # worker.py
    "翻译线程异常: {exc}": "Translation thread exception: {exc}",

    # smart_translation_controller.py
    "智能翻译探测开始: token={token}": "Smart translation probe started: token={token}",
    "外部选区直接翻译: {char_count} 字符": "Translating external selection directly: {char_count} characters",
    "发送智能翻译复制快捷键": "Sending smart translation copy shortcut",
    "剪贴板监听未启用，直接读取当前文本: {char_count} 字符": "Clipboard monitoring not enabled, reading current text directly: {char_count} characters",
    "智能翻译获取文本成功: {char_count} 字符": "Smart translation text captured successfully: {char_count} characters",
    "智能翻译忽略非文本内容: {content_type_display}": "Smart translation ignored non-text content: {content_type_display}",
    "智能翻译转入小窗手动输入: {reason}": "Smart translation fell back to compact popup manual input: {reason}",

    # translation_manager.py
    "TranslationManager 已初始化": "TranslationManager initialized",
    "翻译服务已配置 (Pro: {use_pro}, split_sentences: {split_sentences}, preserve_formatting: {preserve_formatting})": "Translation service configured (Pro: {use_pro}, split_sentences: {split_sentences}, preserve_formatting: {preserve_formatting})",
    "翻译引擎未配置": "Translation engine not configured",
    "开始翻译: {text_preview}...": "Starting translation: {text_preview}...",
    "打开翻译窗口（待用户输入）": "Opening translation window (waiting for user input)",
    "开始划词翻译: {text_preview}...": "Starting selection translation: {text_preview}...",
    "开始小窗输入翻译: {text_preview}...": "Starting compact popup input translation: {text_preview}...",
    "小窗目标语言已更新: {new_lang}": "Compact popup target language updated: {new_lang}",
    "保存目标语言失败: {e}": "Failed to save target language: {e}",
    "创建新翻译窗口": "Creating new translation window",
    "复用现有翻译窗口": "Reusing existing translation window",
    "调用翻译引擎 {provider_name}: target={target_lang}, preserve_formatting={preserve_formatting}": "Calling translation engine {provider_name}: target={target_lang}, preserve_formatting={preserve_formatting}",
    "忽略已被新请求替代的翻译结果": "Ignoring translation result superseded by a newer request",
    "翻译完成: success={success}, detected_lang={detected_lang}": "Translation finished: success={success}, detected_lang={detected_lang}",
    "翻译请求: -> {target_lang}": "Translation requested: -> {target_lang}",
    "翻译窗口已关闭，清理资源": "Translation window closed, cleaning up resources",
    "退出时翻译网络线程未在期限内结束": "Translation network thread did not finish within the deadline at shutdown",
    "TranslationManager 已清理": "TranslationManager cleaned up",
    "截图翻译模式：显示窗口并启动OCR": "Screenshot translation mode: showing window and starting OCR",
    "传入 pixmap 为空，跳过OCR": "Provided pixmap is empty, skipping OCR",
    "QImage 转换失败，跳过OCR": "QImage conversion failed, skipping OCR",
    "OCR线程已启动": "OCR thread started",
    "OCR完成: success={success}, result_len={result_len}": "OCR finished: success={success}, result_len={result_len}",
    "翻译窗口已关闭，忽略OCR结果": "Translation window closed, ignoring OCR result",
    "OCR识别成功: {result_preview}...": "OCR recognition succeeded: {result_preview}...",
    "OCR识别失败: {result}": "OCR recognition failed: {result}",

    # deepl_service.py
    "发送翻译请求: {char_count} 字符 -> {target_lang}": "Sending translation request: {char_count} characters -> {target_lang}",
    "翻译成功: {detected_lang} -> {target_lang}": "Translation succeeded: {detected_lang} -> {target_lang}",
    "HTTP 错误: {error_msg}": "HTTP error: {error_msg}",
    "网络错误: {reason}": "Network error: {reason}",
    "JSON 解析失败: {e}": "Failed to parse JSON: {e}",
    "未知错误: {e}": "Unknown error: {e}",
    "翻译线程异常: {e}": "Translation thread exception: {e}",
}
